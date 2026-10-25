#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace ayafileio::diagnostics::yuyuko {
namespace detail {

static constexpr size_t ASYNC_BUF_CAPACITY = 4096;
static constexpr size_t BUFFER_COUNT = 64;

struct LogBuffer {
    char messages[BUFFER_COUNT][ASYNC_BUF_CAPACITY];
    size_t count = 0;

    bool full() const {
        return count >= BUFFER_COUNT;
    }
};

inline LogBuffer g_buf_A, g_buf_B;
inline LogBuffer* g_active = &g_buf_A;
inline LogBuffer* g_flush = &g_buf_B;
inline std::mutex g_log_mtx;

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
inline std::jthread g_log_thread;
#else
inline std::thread g_log_thread;
#endif
inline std::atomic<bool> g_log_stop{false};
inline std::condition_variable g_log_cv;
inline std::mutex g_log_cv_mtx;
inline FILE* g_log_file = nullptr;
inline bool g_async_mode = false;

inline void swap_log_buffers() {
    LogBuffer* t = g_active;
    g_active = g_flush;
    g_flush = t;
    g_active->count = 0;
}

inline void push_log(const char* msg, size_t len) {
    if (!g_async_mode) {
        static std::mutex sync_mtx;
        std::lock_guard<std::mutex> lk(sync_mtx);
        fwrite(msg, 1, len, stderr);
        fflush(stderr);
        return;
    }
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        if (g_active->count < BUFFER_COUNT) {
            size_t n = (len < ASYNC_BUF_CAPACITY - 1) ? len : ASYNC_BUF_CAPACITY - 1;
            memcpy(g_active->messages[g_active->count], msg, n);
            g_active->messages[g_active->count][n] = '\0';
            g_active->count++;
        }
        notify = g_active->full();
    }
    if (notify) g_log_cv.notify_one();
}

inline void log_worker() {
    while (true) {
        {
            std::unique_lock<std::mutex> lk(g_log_cv_mtx);
            g_log_cv.wait_for(lk, std::chrono::milliseconds(50), [] {
                return g_log_stop.load(std::memory_order_relaxed);
            });
        }
        { std::lock_guard<std::mutex> lk(g_log_mtx); swap_log_buffers(); }
        if (g_log_file) {
            for (size_t i = 0; i < g_flush->count; ++i) fputs(g_flush->messages[i], g_log_file);
            fflush(g_log_file);
        }
        g_flush->count = 0;
        if (g_log_stop.load(std::memory_order_relaxed)) break;
    }
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

} // namespace detail

inline void start_async_log(const char* filepath = "yuyuko_memory.log") {
    if (detail::g_async_mode) return;

    detail::g_log_file = fopen(filepath, "w");
    if (!detail::g_log_file) {
        fprintf(stderr, "[Yuyuko] 无法打开日志文件 %s，将使用 stderr 同步输出\n", filepath);
        return;
    }

    detail::g_async_mode = true;
    detail::g_log_stop.store(false);
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    detail::g_log_thread = std::jthread(detail::log_worker);
#else
    detail::g_log_thread = std::thread(detail::log_worker);
#endif

    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), "[Yuyuko] 异步日志引擎启动，输出文件: %s\n", filepath);
    detail::push_log(buf, static_cast<size_t>(len));
}

inline void stop_async_log() {
    if (!detail::g_async_mode) return;

    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), "[Yuyuko] 异步日志引擎停止\n");
    detail::push_log(buf, static_cast<size_t>(len));

    detail::g_log_stop.store(true);
    detail::g_log_cv.notify_one();

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    detail::g_log_thread = std::jthread{};
#else
    if (detail::g_log_thread.joinable()) {
        detail::g_log_thread.join();
    }
#endif

    detail::g_async_mode = false;
}

namespace detail {

template<typename... Args>
inline void yuyuko_logln(const char* fmt, Args... args) {
    char buf[ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), fmt, args...);
    if (len < 0) return;
    size_t n = static_cast<size_t>(len);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (n + 2 <= sizeof(buf)) {
        buf[n] = '\n';
        buf[n + 1] = '\0';
        push_log(buf, n + 1);
    } else {
        push_log(buf, n);
    }
}

template<typename... Args>
inline void yuyuko_trace(const char* fmt, Args... args) {
    if (g_async_mode) yuyuko_logln(fmt, args...);
}

} // namespace detail

} // namespace ayafileio::diagnostics::yuyuko
