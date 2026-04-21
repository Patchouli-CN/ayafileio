// src/debug_log.hpp
#pragma once

#include <chrono>
#include <cstdio>
#include <thread>
#include <atomic>
#include <functional>

// ════════════════════════════════════════════════════════════════════════════
// 调试日志宏 - 支持运行时开关
// ════════════════════════════════════════════════════════════════════════════

#ifdef AYAFILEIO_VERBOSE_LOGGING

#define UR_LOG(fmt, ...) \
    do { \
        auto now = std::chrono::steady_clock::now(); \
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
            now.time_since_epoch()).count() % 1000000; \
        std::fprintf(stderr, "[%6ld][0x%zx] " fmt "\n", \
            ms, std::hash<std::thread::id>{}(std::this_thread::get_id()), ##__VA_ARGS__); \
        std::fflush(stderr); \
    } while(0)

#define UR_LOG_RAW(fmt, ...) \
    do { \
        std::fprintf(stderr, fmt, ##__VA_ARGS__); \
        std::fflush(stderr); \
    } while(0)

#define UR_LOG_RATELIMIT(counter_var, interval, fmt, ...) \
    do { \
        static std::atomic<int> _cnt{0}; \
        int c = _cnt.fetch_add(1, std::memory_order_relaxed); \
        if (c % (interval) == 0) { \
            UR_LOG(fmt " (rate-limited, count=%d)", ##__VA_ARGS__, c); \
        } \
    } while(0)

#else
#define UR_LOG(fmt, ...) ((void)0)
#define UR_LOG_RAW(fmt, ...) ((void)0)
#define UR_LOG_RATELIMIT(counter, interval, fmt, ...) ((void)0)
#endif

#define UR_DEBUG_LOG(fmt, ...) \
        UR_LOG(fmt, ##__VA_ARGS__);