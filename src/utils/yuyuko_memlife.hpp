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
// 前向声明（异步日志部分）
// ════════════════════════════════════════════════════════════════════════════
namespace detail {
    void async_log_callback(const char* msg);
}

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
// 双缓冲异步日志引擎
// ════════════════════════════════════════════════════════════════════════════

namespace detail {

// 日志缓冲区配置
static constexpr size_t ASYNC_BUF_CAPACITY = 4096;   // 单条日志最大长度
static constexpr size_t BUFFER_COUNT       = 64;      // 每个 EventBuffer 可容纳的日志条数

// 单个日志缓冲区
struct LogBuffer {
    char messages[BUFFER_COUNT][ASYNC_BUF_CAPACITY];
    size_t count = 0;  // 改用普通 size_t，手动管理
    
    bool full() const {
        return count >= BUFFER_COUNT;
    }
};

// 双缓冲区
static LogBuffer g_active_log_buf;   // 业务线程写入
static LogBuffer g_flush_log_buf;    // 后台线程刷盘
static std::mutex g_log_swap_mtx;

// 后台线程控制
static std::thread g_log_thread;
static std::atomic<bool> g_log_running{false};
static std::atomic<bool> g_log_stop{false};
static std::condition_variable g_log_cv;
static std::mutex g_log_cv_mtx;
static FILE* g_log_file = nullptr;
static bool g_async_mode = false;

// 快速格式化：把格式化字符串写入缓冲区（栈上）
template<typename... Args>
static int fast_format(char* buf, size_t buf_size, const char* fmt, Args... args) {
    return snprintf(buf, buf_size, fmt, args...);
}

// 交换两个 LogBuffer（手动交换，因为不能用 std::swap）
static void swap_log_buffers() {
    // 交换 messages 数组
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        char tmp[ASYNC_BUF_CAPACITY];
        memcpy(tmp, g_active_log_buf.messages[i], ASYNC_BUF_CAPACITY);
        memcpy(g_active_log_buf.messages[i], g_flush_log_buf.messages[i], ASYNC_BUF_CAPACITY);
        memcpy(g_flush_log_buf.messages[i], tmp, ASYNC_BUF_CAPACITY);
    }
    // 交换 count
    size_t tmp_count = g_active_log_buf.count;
    g_active_log_buf.count = g_flush_log_buf.count;
    g_flush_log_buf.count = tmp_count;
}

// 业务线程：把日志消息推送到活跃缓冲区
// 在 g_log_swap_mtx 保护下调用
static void push_log(const char* msg, size_t len) {
    if (!g_async_mode) {
        // 同步模式：直接写到 stderr（兼容旧行为）
        fwrite(msg, 1, len, stderr);
        fflush(stderr);
        return;
    }
    
    if (g_active_log_buf.count < BUFFER_COUNT) {
        size_t copy_len = (len < ASYNC_BUF_CAPACITY - 1) ? len : ASYNC_BUF_CAPACITY - 1;
        memcpy(g_active_log_buf.messages[g_active_log_buf.count], msg, copy_len);
        g_active_log_buf.messages[g_active_log_buf.count][copy_len] = '\0';
        g_active_log_buf.count++;
    }
    
    // 如果满了，通知后台线程
    if (g_active_log_buf.full()) {
        g_log_cv.notify_one();
    }
}

// 后台线程：批量消费日志
static void log_worker() {
    while (!g_log_stop.load(std::memory_order_relaxed)) {
        // 等待日志或停止信号
        {
            std::unique_lock<std::mutex> lk(g_log_cv_mtx);
            g_log_cv.wait_for(lk, std::chrono::milliseconds(50), [] {
                return g_active_log_buf.full() || g_log_stop.load(std::memory_order_relaxed);
            });
        }
        
        // 交换缓冲区
        {
            std::lock_guard<std::mutex> lk(g_log_swap_mtx);
            swap_log_buffers();
            // 重置 active buffer count
            g_active_log_buf.count = 0;
        }
        
        // 写入文件
        size_t n = g_flush_log_buf.count;
        
        if (g_log_file && n > 0) {
            for (size_t i = 0; i < n; ++i) {
                fputs(g_flush_log_buf.messages[i], g_log_file);
            }
            fflush(g_log_file);  // 批量落盘
        }
        
        g_flush_log_buf.count = 0;
    }
    
    // 停止前最后刷一次
    {
        std::lock_guard<std::mutex> lk(g_log_swap_mtx);
        swap_log_buffers();
        g_active_log_buf.count = 0;
    }
    
    size_t n = g_flush_log_buf.count;
    
    if (g_log_file && n > 0) {
        for (size_t i = 0; i < n; ++i) {
            fputs(g_flush_log_buf.messages[i], g_log_file);
        }
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

} // namespace detail

// ════════════════════════════════════════════════════════════════════════════
// 异步日志公共 API
// ════════════════════════════════════════════════════════════════════════════

/// 启动异步日志线程，所有日志输出到指定文件
/// @param filepath 日志文件路径
inline void start_async_log(const char* filepath = "yuyuko_memory.log") {
    if (detail::g_async_mode) return;  // 已经启动了
    
    detail::g_log_file = fopen(filepath, "w");
    if (!detail::g_log_file) {
        // 如果文件打不开，回退到 stderr 同步模式
        fprintf(stderr, "[Yuyuko] 无法打开日志文件 %s，将使用 stderr 同步输出\n", filepath);
        return;
    }
    
    detail::g_async_mode = true;
    detail::g_log_stop.store(false);
    detail::g_log_running.store(true);
    detail::g_log_thread = std::thread(detail::log_worker);
    
    // 写一条启动日志
    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), 
        "[Yuyuko] 异步日志引擎启动，输出文件: %s\n", filepath);
    detail::push_log(buf, static_cast<size_t>(len));
}

/// 停止异步日志线程，等待所有日志落盘
inline void stop_async_log() {
    if (!detail::g_async_mode) return;
    
    // 写一条停止日志
    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), "[Yuyuko] 异步日志引擎停止\n");
    detail::push_log(buf, static_cast<size_t>(len));
    
    detail::g_log_stop.store(true);
    detail::g_log_cv.notify_one();
    
    if (detail::g_log_thread.joinable()) {
        detail::g_log_thread.join();
    }
    
    detail::g_async_mode = false;
    detail::g_log_running.store(false);
}

// ════════════════════════════════════════════════════════════════════════════
// 内部日志宏（供系统内部使用）
// ════════════════════════════════════════════════════════════════════════════

namespace detail {

// 内部日志函数：格式化并推送到日志系统
template<typename... Args>
static void yuyuko_log(const char* fmt, Args... args) {
    char buf[ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), fmt, args...);
    if (len > 0) {
        push_log(buf, static_cast<size_t>(len));
    }
}

// 带换行的日志
template<typename... Args>
static void yuyuko_logln(const char* fmt, Args... args) {
    char buf[ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), fmt, args...);
    if (len > 0 && static_cast<size_t>(len) < sizeof(buf) - 2) {
        buf[len] = '\n';
        buf[len + 1] = '\0';
        push_log(buf, static_cast<size_t>(len + 1));
    } else {
        push_log(buf, static_cast<size_t>(len));
    }
}

} // namespace detail

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
    detail::yuyuko_logln(
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
            detail::yuyuko_logln(
                "[Yuyuko memory] thread=%s id=%s — FREE UNKNOWN %p at %s:%d (%s) "
                "-- 这块内存不是白玉楼的注册灵魂！",
                current_thread_name.c_str(), current_thread_id.c_str(),
                ptr, file, line, func
            );
            should_free = true;
        } else if (soul->source != source) {
            detail::yuyuko_logln(
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
            detail::yuyuko_logln(
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
            detail::yuyuko_logln(
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
/// @return true if safe to access, false if already freed (caller MUST NOT touch it)
inline bool check_access(void* ptr, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    SoulRecord* soul = find_soul(ptr);
    if (!soul) return true;  // not tracked — assume safe
    if (!soul->released) return true;  // still alive — safe
    uint64_t elapsed = current_time_ms() - soul->timestamp;
    detail::yuyuko_logln(
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
    return false;  // caller MUST NOT touch this pointer
}

/// 检查指针是否存活，以及 [ptr, ptr+size) 是否在分配范围内
/// @return true if safe to write, false if out of bounds or already freed
inline bool check_bounds(void* ptr, size_t size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    SoulRecord* soul = find_soul(ptr);
    
    if (!soul) {
        // 不是我们追踪的内存，无法检测，假设安全
        return true;
    }
    
    if (soul->released) {
        uint64_t elapsed = current_time_ms() - soul->timestamp;
        detail::yuyuko_logln(
            "[Yuyuko memory] USE-AFTER-FREE %p at %s:%d (%s) — "
            "尝试写入已释放的内存！该内存在 %llums 前被释放。",
            ptr, file, line, func,
            static_cast<unsigned long long>(elapsed)
        );
        return false;
    }
    
    // 检查是否写越界
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t alloc_start = reinterpret_cast<uintptr_t>(soul->ptr);
    uintptr_t alloc_end = alloc_start + soul->size;
    uintptr_t write_end = start + size;
    
    if (start < alloc_start || write_end > alloc_end) {
        ptrdiff_t overflow;
        const char* direction;
        if (start < alloc_start) {
            overflow = static_cast<ptrdiff_t>(alloc_start - start);
            direction = "前";
        } else {
            overflow = static_cast<ptrdiff_t>(write_end - alloc_end);
            direction = "后";
        }
        detail::yuyuko_logln(
            "[Yuyuko memory] BUFFER OVERFLOW at %s:%d (%s) — "
            "写入 %zu 字节到 %p 时向%s越界 %td 字节！"
            "  分配大小: %zu 字节 (分配于 %s:%d)",
            file, line, func,
            size, ptr, direction, overflow,
            soul->size, soul->file.c_str(), soul->line
        );
        return false;
    }
    
    return true;
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

/// 程序退出时调用，检查是否还有未释放的内存。
/// 如果有泄漏，打印每个泄漏块的完整分配信息到日志。
/// @return 泄漏的内存块数量（0 表示全部释放，无泄漏）
inline size_t leak_check() {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    size_t leak_count = 0;
    
    for (const auto& kv : g_soul_book) {
        const SoulRecord& soul = kv.second;
        if (!soul.released) {
            ++leak_count;
            uint64_t elapsed = current_time_ms() - soul.timestamp;
            detail::yuyuko_logln(
                "[Yuyuko memory] LEAK: %p (%zu bytes) — 已存续 %llums，仍未释放！\n"
                "  分配地点: [thread=%s id=%s] %s:%d (%s)",
                soul.ptr,
                soul.size,
                static_cast<unsigned long long>(elapsed),
                soul.thread_name.c_str(),
                soul.thread_id.c_str(),
                soul.file.c_str(),
                soul.line,
                soul.func.c_str()
            );
        }
    }
    
    if (leak_count > 0) {
        detail::yuyuko_logln(
            "[Yuyuko memory] 内存泄漏检测完成: %zu 个内存块泄漏 (总计 %zu 次分配)",
            leak_count, g_soul_book.size());
    } else {
        detail::yuyuko_logln("[Yuyuko memory] 所有灵魂均已安息，无内存泄漏。");
    }
    
    return leak_count;
}

/// 生成 HTML 内存状态报告
/// @param filepath 输出文件路径（如 "yuyuko_report.html"）
/// @param title 报告标题
inline void generate_html_report(const char* filepath = "yuyuko_report.html",
                                  const char* title = "幽幽子·内存魂簿报告") {
    std::lock_guard<std::mutex> lk(g_soul_mtx);
    
    FILE* f = fopen(filepath, "w");
    if (!f) return;
    
    // 统计
    size_t total = g_soul_book.size();
    size_t alive = 0;
    size_t leaked_bytes = 0;
    size_t total_bytes = 0;
    for (const auto& kv : g_soul_book) {
        total_bytes += kv.second.size;
        if (!kv.second.released) {
            alive++;
            leaked_bytes += kv.second.size;
        }
    }
    size_t released = total - alive;
    uint64_t now = current_time_ms();
    
    // ═══════════════ HTML 头部 ═══════════════
    fprintf(f, 
R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>%s</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif;
    background: #0d1117;
    color: #c9d1d9;
    padding: 20px;
  }
  .header {
    text-align: center;
    padding: 30px;
    background: linear-gradient(135deg, #1a1a2e 0%%, #16213e 50%%, #0f3460 100%%);
    border-radius: 12px;
    margin-bottom: 20px;
    border: 1px solid #30363d;
  }
  .header h1 {
    font-size: 2em;
    color: #ff79c6;
    margin-bottom: 8px;
  }
  .header .subtitle {
    color: #8b949e;
    font-size: 0.9em;
  }
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 16px;
    margin-bottom: 20px;
  }
  .card {
    background: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
    padding: 20px;
    text-align: center;
  }
  .card .number {
    font-size: 2.5em;
    font-weight: bold;
  }
  .card .label {
    color: #8b949e;
    margin-top: 4px;
    font-size: 0.9em;
  }
  .card.total .number { color: #58a6ff; }
  .card.alive .number { color: #3fb950; }
  .card.released .number { color: #d2a8ff; }
  .card.leaked .number { color: #f85149; }
  .card.bytes .number { color: #d2991d; font-size: 1.8em; }
  
  .toolbar {
    display: flex;
    gap: 10px;
    margin-bottom: 16px;
    flex-wrap: wrap;
  }
  .toolbar button {
    padding: 8px 16px;
    border: 1px solid #30363d;
    border-radius: 6px;
    background: #21262d;
    color: #c9d1d9;
    cursor: pointer;
    font-size: 0.9em;
    transition: all 0.2s;
  }
  .toolbar button:hover { background: #30363d; }
  .toolbar button.active { background: #1f6feb; border-color: #1f6feb; }
  .toolbar input {
    padding: 8px 12px;
    border: 1px solid #30363d;
    border-radius: 6px;
    background: #0d1117;
    color: #c9d1d9;
    font-size: 0.9em;
    flex: 1;
    min-width: 200px;
  }
  
  table {
    width: 100%%;
    border-collapse: collapse;
    background: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
    overflow: hidden;
  }
  th {
    background: #21262d;
    padding: 12px;
    text-align: left;
    font-weight: 600;
    color: #8b949e;
    font-size: 0.85em;
    cursor: pointer;
    user-select: none;
  }
  th:hover { color: #c9d1d9; }
  th .arrow { font-size: 0.7em; margin-left: 4px; }
  td {
    padding: 10px 12px;
    border-top: 1px solid #21262d;
    font-size: 0.9em;
  }
  tr:hover { background: #1c2128; }
  tr.alive { border-left: 3px solid #3fb950; }
  tr.released_row { border-left: 3px solid #484f58; opacity: 0.65; }
  
  .badge {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 0.8em;
    font-weight: 600;
  }
  .badge-new { background: #1f6feb22; color: #58a6ff; }
  .badge-malloc { background: #d2991d22; color: #d2991d; }
  .badge-alive { background: #3fb95022; color: #3fb950; }
  .badge-released { background: #484f5822; color: #8b949e; }
  .badge-leak { background: #f8514922; color: #f85149; }
  
  .addr { font-family: 'Cascadia Code', 'Fira Code', monospace; color: #7ee787; }
  .location { color: #58a6ff; cursor: pointer; }
  .location:hover { text-decoration: underline; }
  .thread-tag { color: #d2a8ff; font-size: 0.85em; }
  
  .footer {
    text-align: center;
    color: #484f58;
    margin-top: 20px;
    font-size: 0.85em;
  }
</style>
</head>
<body>
)", title);
    
    // ═══════════════ 页面头部 ═══════════════
    fprintf(f, R"(
<div class="header">
  <h1>🦋 %s</h1>
  <div class="subtitle">白玉楼内存魂簿 · 生成时间：系统运行时</div>
</div>
)", title);
    
    // ═══════════════ 统计卡片 ═══════════════
    fprintf(f, R"(
<div class="cards">
  <div class="card total">
    <div class="number">%zu</div>
    <div class="label">📋 总计注册灵魂</div>
  </div>
  <div class="card alive">
    <div class="number">%zu</div>
    <div class="label">🟢 存活中</div>
  </div>
  <div class="card released">
    <div class="number">%zu</div>
    <div class="label">🟣 已安息</div>
  </div>
  <div class="card leaked">
    <div class="number">%zu</div>
    <div class="label">🔴 可能泄漏</div>
  </div>
  <div class="card bytes">
    <div class="number">%zu B</div>
    <div class="label">💾 存活内存</div>
  </div>
</div>
)", total, alive, released, alive, leaked_bytes);
    
    // ═══════════════ 工具栏 ═══════════════
    fprintf(f, R"HTML(
<div class="toolbar">
  <button class="active" onclick="filter('all', this)">全部</button>
  <button onclick="filter('alive', this)">🟢 存活</button>
  <button onclick="filter('released', this)">🟣 已安息</button>
  <input type="text" placeholder="🔍 搜索文件名、函数名或地址..." oninput="search(this.value)">
</div>
)HTML");
    
    // ═══════════════ 数据表格 ═══════════════
    fprintf(f, R"HTML(
<table>
<thead>
<tr>
  <th onclick="sortTable(0)">状态<span class="arrow"></span></th>
  <th onclick="sortTable(1)">地址<span class="arrow"></span></th>
  <th onclick="sortTable(2)">大小<span class="arrow"></span></th>
  <th onclick="sortTable(3)">来源<span class="arrow"></span></th>
  <th onclick="sortTable(4)">分配位置<span class="arrow"></span></th>
  <th onclick="sortTable(5)">线程<span class="arrow"></span></th>
  <th onclick="sortTable(6)">存续时间<span class="arrow"></span></th>
</tr>
</thead>
<tbody>
)HTML");
    
    // 遍历魂簿，生成每一行
    for (const auto& kv : g_soul_book) {
        const SoulRecord& soul = kv.second;
        
        const char* row_class = soul.released ? "released_row" : "alive";
        const char* status_badge;
        const char* status_text;
        
        if (soul.released) {
            status_badge = "badge-released";
            status_text = "已安息";
        } else {
            status_badge = "badge-leak";
            status_text = "存活";
        }
        
        const char* source_badge = (soul.source == AllocSource::OPERATOR_NEW) 
            ? "badge-new" : "badge-malloc";
        const char* source_text = (soul.source == AllocSource::OPERATOR_NEW) 
            ? "new" : "malloc";
        
        uint64_t elapsed = now - soul.timestamp;
        const char* time_unit = "ms";
        if (elapsed > 60000) { elapsed /= 60000; time_unit = "min"; }
        else if (elapsed > 10000) { elapsed /= 1000; time_unit = "s"; }
        
        fprintf(f, R"HTML(
<tr class="%s" data-status="%s">
  <td><span class="badge %s">%s</span></td>
  <td><span class="addr">%p</span></td>
  <td>%zu B</td>
  <td><span class="badge %s">%s</span></td>
  <td><span class="location" title="%s:%d">%s:%d</span><br><small style="color:#8b949e">%s()</small></td>
  <td><span class="thread-tag">%s</span></td>
  <td>%llu %s</td>
</tr>
)HTML",
            row_class,
            soul.released ? "released" : "alive",
            status_badge, status_text,
            soul.ptr,
            soul.size,
            source_badge, source_text,
            soul.file.c_str(), soul.line, soul.file.c_str(), soul.line,
            soul.func.c_str(),
            soul.thread_name.c_str(),
            static_cast<unsigned long long>(elapsed), time_unit
        );
    }
    
    // ═══════════════ HTML 尾部 + JavaScript ═══════════════
    fprintf(f, R"HTML(
</tbody>
</table>

<div class="footer">
  🦋 幽幽子内存魂簿报告 · 由 Yuyuko::generate_html_report() 生成 · 白玉楼出品
</div>

<script>
// 筛选功能
function filter(status, btn) {
  document.querySelectorAll('.toolbar button').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  document.querySelectorAll('tbody tr').forEach(row => {
    if (status === 'all') row.style.display = '';
    else row.style.display = row.dataset.status === status ? '' : 'none';
  });
}

// 搜索功能
function search(query) {
  const q = query.toLowerCase();
  document.querySelectorAll('tbody tr').forEach(row => {
    const text = row.textContent.toLowerCase();
    row.style.display = text.includes(q) ? '' : 'none';
  });
  // 搜索时取消筛选按钮的高亮
  document.querySelectorAll('.toolbar button').forEach(b => b.classList.remove('active'));
  if (!q) document.querySelector('.toolbar button').classList.add('active');
}

// 排序功能
let sortDir = {};
function sortTable(col) {
  const tbody = document.querySelector('tbody');
  const rows = Array.from(tbody.querySelectorAll('tr'));
  const key = 'col' + col;
  sortDir[key] = !(sortDir[key] || false);
  const dir = sortDir[key] ? 1 : -1;
  
  rows.sort((a, b) => {
    let va = a.cells[col].textContent.trim();
    let vb = b.cells[col].textContent.trim();
    // 尝试按数字排序
    let na = parseFloat(va);
    let nb = parseFloat(vb);
    if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dir;
    return va.localeCompare(vb) * dir;
  });
  
  // 更新箭头
  document.querySelectorAll('th .arrow').forEach(a => a.textContent = '');
  const th = document.querySelectorAll('th')[col];
  th.querySelector('.arrow').textContent = dir > 0 ? '▲' : '▼';
  
  rows.forEach(row => tbody.appendChild(row));
}
</script>
</body>
</html>
)HTML");
    
    fclose(f);
    
    // 同步模式下输出到终端；异步模式下已经在锁内，直接 fprintf stderr
    if (!detail::g_async_mode) {
        fprintf(stderr, "[Yuyuko] HTML 报告已生成: %s\n", filepath);
    }
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

// ════════════════════════════════════════════════════════════════════════════
// 带边界检查的内存操作宏
// ════════════════════════════════════════════════════════════════════════════

#define TRACKED_MEMSET(ptr, val, size) \
    do { \
        if (Yuyuko::check_bounds((ptr), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memset((ptr), (val), (size)); \
        } \
    } while(0)

#define TRACKED_MEMCPY(dst, src, size) \
    do { \
        if (Yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memcpy((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_MEMMOVE(dst, src, size) \
    do { \
        if (Yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memmove((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_STRCPY(dst, src) \
    TRACKED_MEMCPY((dst), (src), std::strlen(src) + 1)

#define TRACKED_STRNCPY(dst, src, n) \
    do { \
        size_t len = std::strnlen((src), (n)); \
        if (Yuyuko::check_bounds((dst), (n), __FILE__, __LINE__, __FUNCTION__)) { \
            std::strncpy((dst), (src), (n)); \
        } \
    } while(0)

#else // ENABLE_ASAN 未启用

// Release 模式：所有宏退化为标准操作，零开销

namespace Yuyuko {

    inline void start_async_log(const char* = nullptr) {}
    inline void stop_async_log() {}

    inline void register_new(void*, size_t, const char*, int, const char*) {}
    inline void register_malloc(void*, size_t, const char*, int, const char*) {}
    inline bool release_new(void*, const char*, int, const char*) { return true; }
    inline bool release_malloc(void*, const char*, int, const char*) { return true; }

    inline bool check_access(void*, const char*, int, const char*) { return true; }
    inline bool check_bounds(void*, size_t, const char*, int, const char*) { return true; }
    inline bool is_released(void*) { return false; }
    inline size_t get_size(void*) { return 0; }
    inline size_t get_total_souls() { return 0; }
    inline size_t get_alive_souls() { return 0; }
    inline size_t leak_check() { return 0; }

    inline void generate_html_report(const char* = nullptr, const char* = nullptr) {}

} // namespace Yuyuko

// 宏退化为原始操作
#define TRACKED_NEW(type)   new type
#define TRACKED_DELETE(ptr) delete ptr
#define TRACKED_ALLOC(size) std::malloc(size)
#define TRACKED_FREE(ptr)   std::free(ptr)

#define TRACKED_MEMSET(ptr, val, size)   std::memset((ptr), (val), (size))
#define TRACKED_MEMCPY(dst, src, size)   std::memcpy((dst), (src), (size))
#define TRACKED_MEMMOVE(dst, src, size)  std::memmove((dst), (src), (size))
#define TRACKED_STRCPY(dst, src)         std::strcpy((dst), (src))
#define TRACKED_STRNCPY(dst, src, n)     std::strncpy((dst), (src), (n))

#endif // ENABLE_ASAN
