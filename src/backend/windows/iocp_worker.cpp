#include "iocp_context.hpp"

#include "../../core/completion.hpp"
#include "../../core/config.hpp"

#include <algorithm>
#include <chrono>
#include <deque>

namespace ayafileio::backend::windows {
namespace {

#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) \
    ((type*)((char*)(address) - (uintptr_t)(&((type*)0)->field)))
#endif

constexpr unsigned quarantine_ms = 1000;
struct QuarantineEntry {
    IocpRequest* request;
    std::chrono::steady_clock::time_point when;
};
thread_local std::deque<QuarantineEntry> quarantine;

std::atomic<uint64_t> q_live{0};
std::atomic<uint64_t> q_pushes{0};
std::atomic<uint64_t> q_scanned{0};
std::atomic<uint64_t> q_drains{0};
std::atomic<uint64_t> q_peak{0};

void note_push() {
    const auto live = q_live.fetch_add(1, std::memory_order_relaxed) + 1;
    q_pushes.fetch_add(1, std::memory_order_relaxed);
    auto peak = q_peak.load(std::memory_order_relaxed);
    while (live > peak && !q_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
}

void quarantine_drain() {
    const auto now = std::chrono::steady_clock::now();
    uint64_t expired = 0;
    while (!quarantine.empty() &&
           now - quarantine.front().when >= std::chrono::milliseconds(quarantine_ms)) {
        IOCP_REQ_FREE(quarantine.front().request);
        quarantine.pop_front();
        q_live.fetch_sub(1, std::memory_order_relaxed);
        ++expired;
    }
    q_scanned.fetch_add(expired, std::memory_order_relaxed);
    q_drains.fetch_add(1, std::memory_order_relaxed);
}

void quarantine_push(IocpRequest* request) {
    quarantine.push_back({request, std::chrono::steady_clock::now()});
    note_push();
    if (quarantine.size() >= 256) quarantine_drain();
}

DWORD quarantine_timeout() {
    if (quarantine.empty()) return INFINITE;
    const auto deadline = quarantine.front().when + std::chrono::milliseconds(quarantine_ms);
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (remaining < 1) remaining = 1;
    return static_cast<DWORD>(remaining + 1);
}


} // namespace

QuarantineStats quarantine_stats() {
    return {
        q_live.load(std::memory_order_relaxed),
        q_pushes.load(std::memory_order_relaxed),
        q_scanned.load(std::memory_order_relaxed),
        q_drains.load(std::memory_order_relaxed),
        q_peak.load(std::memory_order_relaxed),
    };
}

void IOCPContext::worker_proc(HANDLE iocp) {
    core::WorkerThreadAnchor thread_anchor;   // 锚定宿主线程状态（见 completion.hpp）
    auto batch_cap = core::config().iocp_batch_size();
    if (batch_cap < 1) batch_cap = 1;
    if (batch_cap > 256) batch_cap = 256;
    std::vector<OVERLAPPED_ENTRY> batch(batch_cap);

    while (running()) {
        DWORD timeout = INFINITE;
        if (auto* scheduler = core::completion_scheduler()) {
            timeout = scheduler->next_timeout_ms();
        }
        timeout = (std::min)(timeout, quarantine_timeout());
        ULONG count = 0;
        const BOOL ok = GetQueuedCompletionStatusEx(
            iocp, batch.data(), static_cast<ULONG>(batch.size()), &count, timeout, FALSE);
        if (count == 0) {
            quarantine_drain();
            if (auto* scheduler = core::completion_scheduler()) scheduler->flush_idle();
            if (!ok && !running()) break;
            continue;
        }

        for (ULONG index = 0; index < count; ++index) {
            const auto& entry = batch[index];
            if (!entry.lpOverlapped || entry.lpCompletionKey == 0) continue;
            auto* request = CONTAINING_RECORD(entry.lpOverlapped, IocpRequest, ov);
            process_one(
                static_cast<uint64_t>(entry.lpCompletionKey),
                request,
                entry.dwNumberOfBytesTransferred,
                entry.Internal == 0 ? 0 : static_cast<DWORD>(entry.Internal));
        }
        quarantine_drain();
        if (auto* scheduler = core::completion_scheduler()) scheduler->flush_idle();
    }
}

void IOCPContext::process_one(uint64_t id, IocpRequest* request, DWORD bytes, DWORD error) {
    // 走到这里的请求一律由**本函数**收尾：交付结果、减 pending、归还内存都在这条
    // 路径上，只有一套逻辑。
    //
    // 这里不再有"提交线程是否已经交付过"的判断分支。早先它依赖一个普通 bool
    // （inline_completed）：提交线程写、worker 读，既没有原子性也没有 release/
    // acquire，是实打实的数据竞争；而一旦 worker 读到 false，同一个 I/O 就会被
    // 交付两次 —— 第二次 make_io_completion 从已被搬走的 buffer 里取数据，只会
    // 造出一块未初始化的 PyBytes，再对已完成的 Future set_result 一次。
    //
    // 现在分工由内核模式唯一决定：FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 生效时，
    // 同步成功根本不投包，请求由提交线程就地收尾、当场释放；不生效时必然投包，
    // 那就只由这里收尾。不需要跨线程协商，也就没有窗口。
    auto expected = core::IOState::PENDING;
    if (!request->state.compare_exchange_strong(expected, core::IOState::RESOLVED)) return;
    if (!core::request_access_valid(request, __FILE__, __LINE__, __FUNCTION__)) return;

    auto session = get_session(id);
    if (!session) {
        request->operation->complete(core::Completion::failure(
            request->type,
            core::IoError::native(
                error ? error : ERROR_OPERATION_ABORTED,
                "I/O operation completed after the file was closed")));
        quarantine_push(request);
        return;
    }

    session->complete_request();
    if (error == 0) {
        request->operation->complete(core::make_io_completion(request, bytes));
    } else {
        request->operation->complete(core::Completion::failure(
            request->type,
            core::IoError::native(error, "I/O operation failed")));
    }
    quarantine_push(request);
}

} // namespace ayafileio::backend::windows

