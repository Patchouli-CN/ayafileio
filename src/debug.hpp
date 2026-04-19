#pragma once

#ifdef AYAFILEIO_VERBOSE_LOGGING
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>

#define AYA_LOG(msg) do { \
    auto now = std::chrono::system_clock::now(); \
    auto time = std::chrono::system_clock::to_time_t(now); \
    std::stringstream ss; \
    ss << "[ayafileio] " << std::ctime(&time) << " [" << __FILE__ << ":" << __LINE__ << "] " << msg << std::endl; \
    std::cout << ss.str(); \
} while(0)

#define AYA_LOG_ENTER() AYA_LOG(">>> " << __FUNCTION__ << " entered")
#define AYA_LOG_EXIT() AYA_LOG("<<< " << __FUNCTION__ << " exiting")
#define AYA_LOG_ERROR(msg) AYA_LOG("ERROR: " << msg)
#define AYA_LOG_WARN(msg) AYA_LOG("WARNING: " << msg)

#else
#define AYA_LOG(msg) ((void)0)
#define AYA_LOG_ENTER() ((void)0)
#define AYA_LOG_EXIT() ((void)0)
#define AYA_LOG_ERROR(msg) ((void)0)
#define AYA_LOG_WARN(msg) ((void)0)
#endif

// 断言宏，在调试模式下更详细
#ifdef AYAFILEIO_DEBUG
#define AYA_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        AYA_LOG_ERROR("Assertion failed: " #cond " - " << msg); \
        std::abort(); \
    } \
} while(0)
#else
#define AYA_ASSERT(cond, msg) ((void)0)
#endif