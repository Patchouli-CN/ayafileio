// yuyuko_memlife.hpp — 幽幽子内存生命周期追踪系统
//
// 在 Debug 模式下追踪堆内存的分配/释放。
// Release 模式下所有宏退化为标准 new/delete，零开销。
//
// 用法：
//   TRACKED_NEW(type)              → 追踪对象，使用 operator new/delete
//   TRACKED_ALLOC(ptr, size)       → 追踪原始内存块，使用 std::malloc/free
//   TRACKED_DELETE(ptr)            → 释放追踪对象
//   TRACKED_FREE(ptr)              → 释放追踪内存块
//   Yuyuko::check_access(ptr)      → 手动检查地址是否已被释放（基于魂簿）

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cstdlib>  // std::malloc, std::free

#ifdef ENABLE_ASAN

// ════════════════════════════════════════════════════════════════════════════
// 幽幽子命名空间
// ════════════════════════════════════════════════════════════════════════════

namespace Yuyuko {

// ════════════════════════════════════════════════════════════════════════════
// 工具函数
// ════════════════════════════════════════════════════════════════════════════

/// 获取当前线程的格式化 ID 字符串（十六进制）
static std::string format_thread_id() {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::hash<std::thread::id>{}(std::this_thread::get_id());
    return oss.str();
}

/// 获取当前线程名称（Windows 上使用 GetThreadDescription，其他平台返回 "unnamed"）
#ifdef _WIN32
#include <windows.h>
static std::string get_thread_name() {
    PWSTR name = nullptr;
    HRESULT hr = GetThreadDescription(GetCurrentThread(), &name);
    if (SUCCEEDED(hr) && name) {
        std::wstring ws(name);
        LocalFree(name);
        return std::string(ws.begin(), ws.end());
    }
    return "unnamed";
}
#else
static std::string get_thread_name() {
    return "unnamed";
}
#endif

/// 获取当前时间戳（毫秒级，相对于启动时间）
static uint64_t current_time_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

// ════════════════════════════════════════════════════════════════════════════
// 分配器类型
// ════════════════════════════════════════════════════════════════════════════

/// 内存分配来源
enum class AllocSource : uint8_t {
    OPERATOR_NEW,   ///< 通过 operator new / operator delete 分配
    STD_MALLOC      ///< 通过 std::malloc / std::free 分配
};

// ════════════════════════════════════════════════════════════════════════════
// 灵魂记录
// ════════════════════════════════════════════════════════════════════════════

/// 单条内存分配记录
struct SoulRecord {
    void* ptr;               ///< 内存地址
    size_t size;             ///< 分配大小（字节）
    AllocSource source;      ///< 分配来源（new 或 malloc）
    std::string file;        ///< 分配时的源文件名
    int line;                ///< 分配时的行号
    std::string func;        ///< 分配时的函数名
    uint64_t timestamp;      ///< 分配/最后操作的时间戳（毫秒）
    std::string thread_name; ///< 操作线程的名称
    std::string thread_id;   ///< 操作线程的格式化 ID
    bool released;           ///< 是否已释放
};

/// 全局魂簿：记录所有被追踪的内存块
static std::mutex g_soul_mtx;
static std::unordered_map<void*, SoulRecord> g_soul_book;

/// 在魂簿中查找记录（调用方需持有 g_soul_mtx）
static SoulRecord* find_soul(void* ptr) {
    auto it = g_soul_book.find(ptr);
    return (it != g_soul_book.end()) ? &it->second : nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
// 内部：统一的注册/释放实现
// ════════════════════════════════════════════════════════════════════════════

/// 注册一次内存分配（内部函数）
static void register_soul_impl(void* ptr, size_t size, AllocSource source,
                                const char* file, int line, const char* func) {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    g_soul_book[ptr] = {
        ptr,
        size,
        source,
        file ? file : "unknown",
        line,
        func ? func : "unknown",
        current_time_ms(),
        get_thread_name(),
        format_thread_id(),
        false
    };
    UR_DEBUG_LOG(
        "[Yuyuko memory] thread=%s id=%s — ALLOC %p size=%zu source=%s at %s:%d (%s)",
        g_soul_book[ptr].thread_name.c_str(),
        g_soul_book[ptr].thread_id.c_str(),
        ptr, size,
        (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc",
        file, line, func
    );
}

/// 记录一次内存释放（内部函数）
/// @return true if the caller should free the memory, false if it was
///         a double-free (already freed — do not free again).
static bool release_soul_impl(void* ptr, AllocSource source,
                               const char* file, int line, const char* func) {
    if (!ptr) return false;

    std::string current_thread_name = get_thread_name();
    std::string current_thread_id   = format_thread_id();
    uint64_t now = current_time_ms();
    bool should_free = true;

    {
        std::lock_guard<std::mutex> lk(g_soul_mtx);
        SoulRecord* soul = find_soul(ptr);

        if (!soul) {
            UR_DEBUG_LOG(
                "[Yuyuko memory] thread=%s id=%s — FREE UNKNOWN %p at %s:%d (%s) "
                "-- 这块内存不是白玉楼的注册灵魂！",
                current_thread_name.c_str(), current_thread_id.c_str(),
                ptr, file, line, func
            );
            should_free = true;
        } else if (soul->source != source) {
            UR_DEBUG_LOG(
                "[Yuyuko memory] thread=%s id=%s — ALLOCATOR MISMATCH %p at %s:%d (%s) "
                "-- 登记为 %s 但尝试用 %s 释放！",
                current_thread_name.c_str(), current_thread_id.c_str(),
                ptr, file, line, func,
                (soul->source == AllocSource::OPERATOR_NEW) ? "new" : "malloc",
                (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc"
            );
            should_free = true;
        } else if (soul->released) {
            uint64_t elapsed = now - soul->timestamp;
            UR_DEBUG_LOG(
                "[Yuyuko memory] thread=%s id=%s — DOUBLE FREE %p — "
                "这个灵魂已经回归冥界了！\n"
                "  首次登记: [thread=%s id=%s] %s:%d (%s) — %llums 前\n"
                "  本次调用: [thread=%s id=%s] %s:%d (%s)",
                current_thread_name.c_str(), current_thread_id.c_str(),
                ptr,
                soul->thread_name.c_str(), soul->thread_id.c_str(),
                soul->file.c_str(), soul->line, soul->func.c_str(),
                static_cast<unsigned long long>(elapsed),
                current_thread_name.c_str(), current_thread_id.c_str(),
                file, line, func
            );
            should_free = false;
        } else {
            uint64_t elapsed = now - soul->timestamp;
            soul->released = true;
            soul->timestamp = now;
            UR_DEBUG_LOG(
                "[Yuyuko memory] thread=%s id=%s — FREE %p (存续 %llums) source=%s\n"
                "  登记: [thread=%s id=%s] %s:%d (%s)\n"
                "  释放: [thread=%s id=%s] %s:%d (%s)",
                current_thread_name.c_str(), current_thread_id.c_str(),
                ptr, static_cast<unsigned long long>(elapsed),
                (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc",
                soul->thread_name.c_str(), soul->thread_id.c_str(),
                soul->file.c_str(), soul->line, soul->func.c_str(),
                current_thread_name.c_str(), current_thread_id.c_str(),
                file, line, func
            );
        }
    }

    return should_free;
}

// ════════════════════════════════════════════════════════════════════════════
// 公开 API
// ════════════════════════════════════════════════════════════════════════════

inline void register_new(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::OPERATOR_NEW, file, line, func);
}

inline void register_malloc(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::STD_MALLOC, file, line, func);
}

/// Bookkeeping only — does NOT free memory. Returns true if caller should free.
inline bool release_new(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::OPERATOR_NEW, file, line, func);
}

/// Bookkeeping only — does NOT free memory. Returns true if caller should free.
inline bool release_malloc(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::STD_MALLOC, file, line, func);
}

/// 检查地址是否属于已释放的内存块（基于魂簿查找）
inline void check_access(void* ptr, const char* file, int line, const char* func) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    SoulRecord* soul = find_soul(ptr);
    if (!soul) {
        // 未注册的指针 — 可能是没被追踪的内存，不报错
        return;
    }
    if (soul->released) {
        uint64_t elapsed = current_time_ms() - soul->timestamp;
        UR_DEBUG_LOG(
            "[Yuyuko memory] USE-AFTER-FREE %p at %s:%d (%s) — "
            "这块内存在 %llums 前已被释放！\n"
            "  原始登记: [thread=%s id=%s] %s:%d (%s)\n"
            "  当前线程: [thread=%s id=%s]",
            ptr, file, line, func,
            static_cast<unsigned long long>(elapsed),
            soul->thread_name.c_str(), soul->thread_id.c_str(),
            soul->file.c_str(), soul->line, soul->func.c_str(),
            get_thread_name().c_str(), format_thread_id().c_str()
        );
    }
}

/// 查询内存块是否已被释放
inline bool is_released(void* ptr) {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    SoulRecord* soul = find_soul(ptr);
    return soul ? soul->released : true;
}

/// 获取内存块的大小（如果已注册）
inline size_t get_size(void* ptr) {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    SoulRecord* soul = find_soul(ptr);
    return soul ? soul->size : 0;
}

/// 获取魂簿中当前注册的内存块总数
inline size_t get_total_souls() {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    return g_soul_book.size();
}

/// 获取魂簿中仍存活的（未释放的）内存块数量
inline size_t get_alive_souls() {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    size_t count = 0;
    for (const auto& kv : g_soul_book) {
        if (!kv.second.released) ++count;
    }
    return count;
}

} // namespace Yuyuko

// ════════════════════════════════════════════════════════════════════════════
// 便捷宏
// ════════════════════════════════════════════════════════════════════════════

#define TRACKED_NEW(type) \
    [](const char* file, int line, const char* func) -> type* { \
        type* ptr = new type; \
        Yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(__FILE__, __LINE__, __FUNCTION__)

#define TRACKED_DELETE(ptr) \
    do { \
        if (ptr) { \
            if (Yuyuko::release_new(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                delete ptr; \
            } \
        } \
    } while(0)

#define TRACKED_ALLOC(size) \
    [](size_t sz, const char* file, int line, const char* func) -> void* { \
        void* ptr = std::malloc(sz); \
        if (ptr) Yuyuko::register_malloc(ptr, sz, file, line, func); \
        return ptr; \
    }(size, __FILE__, __LINE__, __FUNCTION__)

#define TRACKED_FREE(ptr) \
    do { \
        if (ptr) { \
            if (Yuyuko::release_malloc(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                std::free(ptr); \
            } \
        } \
    } while(0)

#else // ENABLE_ASAN 未启用

// Release 模式：所有宏退化为标准操作，零开销

namespace Yuyuko {
    inline void check_access(void*, const char*, int, const char*) {}
    inline bool is_released(void*) { return false; }
    inline size_t get_size(void*) { return 0; }
    inline size_t get_total_souls() { return 0; }
    inline size_t get_alive_souls() { return 0; }
}

#define TRACKED_NEW(type)   new type
#define TRACKED_DELETE(ptr) delete ptr
#define TRACKED_ALLOC(size) std::malloc(size)
#define TRACKED_FREE(ptr)   std::free(ptr)

#endif // ENABLE_ASAN
