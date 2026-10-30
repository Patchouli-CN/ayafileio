#include "mem_watch.hpp"

#include "memory_record.hpp"   // 复用 yuyuko_logln 与线程名缓存

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#endif

namespace ayafileio::diagnostics::yuyuko {
namespace {

struct WatchSlot {
    void* addr{nullptr};
    std::size_t size{0};
    char tag[64]{};
    bool used{false};
};

WatchSlot g_slots[4];
std::mutex g_watch_mtx;

#ifdef _WIN32

PVOID g_veh{nullptr};
bool g_installed{false};

// ── Dr7 编码：本地使能 L0..L3、读写位 RW、长度位 LEN ────────────────────────
//   Dr7 bit(2i)   = Li   本地使能
//   Dr7 bit(2i+1) = Gi   全局使能（不用）
//   Dr7 bit(16+4i .. 17+4i) = RWi  00=执行 01=写 11=读写
//   Dr7 bit(18+4i .. 19+4i) = LENi 00=1B 01=2B 11=4B 10=8B
void write_dr7(DWORDLONG& dr7, int slot, std::size_t size) {
    DWORDLONG len = (size == 8) ? 2 : (size == 1 ? 0 : (size == 2 ? 1 : 3));
    dr7 |= (DWORDLONG(1) << (2 * slot));                 // Li
    dr7 |= (DWORDLONG(1) << (16 + 4 * slot));            // RWi = 01 (写)
    dr7 |= (len << (18 + 4 * slot));                     // LENi
    dr7 &= ~(DWORDLONG(1) << (17 + 4 * slot));           // RWi 高位清零
}

void configure_context(CONTEXT& ctx) {
    std::lock_guard<std::mutex> lk(g_watch_mtx);
    ctx.ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
    ctx.Dr7 = 0;
    int slot = 0;
    for (const auto& w : g_slots) {
        if (!w.used) continue;
        auto addr = reinterpret_cast<DWORDLONG>(w.addr);
        switch (slot) {
            case 0: ctx.Dr0 = addr; break;
            case 1: ctx.Dr1 = addr; break;
            case 2: ctx.Dr2 = addr; break;
            default: ctx.Dr3 = addr; break;
        }
        write_dr7(ctx.Dr7, slot, w.size);
        ++slot;
        if (slot == 4) break;
    }
    ctx.Dr6 = 0;
    // ⚠ 必须清 TF：单步陷阱处理完若残留 TF，会每条指令都触发一次，进程会被拖死。
    ctx.EFlags &= ~static_cast<DWORD>(0x100);
}

// 把 rip 翻成「模块 + 偏移」——这正是后面能对上 PDB 的形式。
void describe_rip(void* rip, char* out, std::size_t out_size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(rip), &module)) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            const auto offset = reinterpret_cast<std::uintptr_t>(rip) -
                                reinterpret_cast<std::uintptr_t>(module);
            std::snprintf(out, out_size, "%s+0x%llx", base,
                          static_cast<unsigned long long>(offset));
            return;
        }
    }
    std::snprintf(out, out_size, "%p", rip);
}

LONG CALLBACK watch_veh(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const DWORD dr6 = static_cast<DWORD>(ctx->Dr6);
    char where[320]{};
    describe_rip(reinterpret_cast<void*>(ctx->Rip), where, sizeof(where));

    std::lock_guard<std::mutex> lk(g_watch_mtx);
    for (int i = 0; i < 4; ++i) {
        if ((dr6 & (1u << i)) == 0) continue;
        const WatchSlot& w = g_slots[i];
        if (!w.used) continue;
        detail::yuyuko_logln(
            "[Yuyuko] WATCH-HIT tag=%s watched=%p len=%zu by=%s rip=0x%llx tid=%lu",
            w.tag, w.addr, w.size, where,
            static_cast<unsigned long long>(ctx->Rip),
            static_cast<unsigned long>(GetCurrentThreadId()));
        // 命中一次后就摘掉这个槽：一次写入足以定性，继续留着会刷屏
        g_slots[i].used = false;
    }
    ctx->Dr6 = 0;
    ctx->EFlags &= ~static_cast<DWORD>(0x100);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool set_thread_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &ctx)) return false;
    configure_context(ctx);
    return SetThreadContext(thread, &ctx) != 0;
}

void arm_all_threads() {
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        // 拿不到快照至少把自己装了
        HANDLE me = GetCurrentThread();
        set_thread_watch(me);
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) continue;
            if (entry.th32ThreadID == self) continue;   // 自己用伪句柄单独装
            HANDLE thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread) continue;
            set_thread_watch(thread);
            CloseHandle(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    set_thread_watch(GetCurrentThread());
}

void clear_all_threads() {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) continue;
            HANDLE thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &ctx)) {
                ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
                ctx.Dr7 = 0;
                ctx.Dr6 = 0;
                ctx.EFlags &= ~static_cast<DWORD>(0x100);
                SetThreadContext(thread, &ctx);
            }
            CloseHandle(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
}

#endif // _WIN32

} // namespace

bool mem_watch_supported() {
#ifdef _WIN32
#if defined(_M_X64) || defined(__x86_64__)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

bool mem_watch(void* addr, std::size_t size, const char* tag) {
#ifdef _WIN32
    if (!mem_watch_supported() || !addr) return false;
    if (size != 1 && size != 2 && size != 4 && size != 8) return false;
    if (reinterpret_cast<std::uintptr_t>(addr) % size != 0) return false;

    {
        std::lock_guard<std::mutex> lk(g_watch_mtx);
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (!g_slots[i].used) { slot = i; break; }
        }
        if (slot < 0) return false;
        g_slots[slot].addr = addr;
        g_slots[slot].size = size;
        g_slots[slot].used = true;
        std::snprintf(g_slots[slot].tag, sizeof(g_slots[slot].tag), "%s", tag ? tag : "?");
        if (!g_installed) {
            g_veh = AddVectoredExceptionHandler(1, watch_veh);
            g_installed = g_veh != nullptr;
        }
    }
    arm_all_threads();
    detail::yuyuko_logln("[Yuyuko] WATCH-ARM tag=%s addr=%p len=%zu (支持数据断点)",
                         tag ? tag : "?", addr, size);
    return true;
#else
    (void)addr; (void)size; (void)tag;
    return false;
#endif
}

void mem_watch_clear() {
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lk(g_watch_mtx);
        for (auto& w : g_slots) w.used = false;
    }
    clear_all_threads();
    if (g_installed) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
        g_installed = false;
    }
#endif
}

std::size_t mem_watch_rearm_current_thread() {
#ifdef _WIN32
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lk(g_watch_mtx);
        for (const auto& w : g_slots) if (w.used) ++count;
    }
    if (count == 0) return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE me = GetCurrentThread();
    if (!GetThreadContext(me, &ctx)) return 0;
    configure_context(ctx);
    return SetThreadContext(me, &ctx) ? count : 0;
#else
    return 0;
#endif
}

std::size_t mem_watch_count() {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lk(g_watch_mtx);
    for (const auto& w : g_slots) if (w.used) ++count;
    return count;
}

} // namespace ayafileio::diagnostics::yuyuko
