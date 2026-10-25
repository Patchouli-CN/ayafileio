#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef ENABLE_YUYUKO

#include "memory_record.hpp"
#include "crc.hpp"
#include "report.hpp"

#define TRACKED_NEW(type) \
    [](const char* file, int line, const char* func) -> type* { \
        type* ptr = new type; \
        ayafileio::diagnostics::yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(__FILE__, __LINE__, __FUNCTION__)

#define TRACKED_NEW_ARGS(type, ...) \
    [](const char* file, int line, const char* func) -> type* { \
        type* ptr = new type(__VA_ARGS__); \
        ayafileio::diagnostics::yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(__FILE__, __LINE__, __FUNCTION__)

#define TRACKED_PLACEMENT_NEW(type, raw_ptr) \
    [](void* rp, const char* file, int line, const char* func) -> type* { \
        type* ptr = new (rp) type; \
        ayafileio::diagnostics::yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(raw_ptr, __FILE__, __LINE__, __FUNCTION__)

#define TRACKED_PLACEMENT_NEW_ARGS(type, raw_ptr, ...) \
    [](void* rp, const char* file, int line, const char* func) -> type* { \
        type* ptr = new (rp) type(__VA_ARGS__); \
        ayafileio::diagnostics::yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(raw_ptr, __FILE__, __LINE__, __FUNCTION__)

#define TRACKED_DELETE(ptr) \
    do { \
        if (ptr) { \
            if (ayafileio::diagnostics::yuyuko::release_new(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                delete ptr; \
            } \
        } \
    } while(0)

#define TRACKED_ALLOC(size) \
    [](size_t sz, const char* file, int line, const char* func) -> void* { \
        void* ptr = std::malloc(sz); \
        if (ptr) ayafileio::diagnostics::yuyuko::register_malloc(ptr, sz, file, line, func); \
        return ptr; \
    }(size, __FILE__, __LINE__, __FUNCTION__)

#define TRACKED_FREE(ptr) \
    do { \
        if (ptr) { \
            if (ayafileio::diagnostics::yuyuko::release_malloc(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                std::free(ptr); \
            } \
        } \
    } while(0)

#define TRACKED_MEMSET(ptr, val, size) \
    do { \
        if (ayafileio::diagnostics::yuyuko::check_bounds((ptr), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memset((ptr), (val), (size)); \
        } \
    } while(0)

#define TRACKED_MEMCPY(dst, src, size) \
    do { \
        if (ayafileio::diagnostics::yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memcpy((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_MEMMOVE(dst, src, size) \
    do { \
        if (ayafileio::diagnostics::yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memmove((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_STRCPY(dst, src) \
    TRACKED_MEMCPY((dst), (src), std::strlen(src) + 1)

#define MEM_SNAPSHOT(ptr, tag)           ayafileio::diagnostics::yuyuko::mem_snapshot((ptr), (tag))
#define MEM_SNAPSHOT_UPDATE(ptr, off, n) ayafileio::diagnostics::yuyuko::mem_snapshot_update((ptr), (off), (n))
#define MEM_VERIFY(ptr)                  ayafileio::diagnostics::yuyuko::mem_verify(ptr)
#define MEM_CHECK(ptr)                   ayafileio::diagnostics::yuyuko::mem_check((ptr), __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD(ptr) \
    ayafileio::diagnostics::yuyuko::MemGuard _yuko_g_##__LINE__((ptr), __FUNCTION__, __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_TAG(ptr, tag) \
    ayafileio::diagnostics::yuyuko::MemGuard _yuko_g_##__LINE__((ptr), (tag), __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_EX(ptr, chunk_size) \
    ayafileio::diagnostics::yuyuko::MemGuardEx _yuko_gx_##__LINE__((ptr), (chunk_size), __FUNCTION__, __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_EX_TAG(ptr, chunk_size, tag) \
    ayafileio::diagnostics::yuyuko::MemGuardEx _yuko_gx_##__LINE__((ptr), (chunk_size), (tag), __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_BATCH_DEF(name) \
    ayafileio::diagnostics::yuyuko::MemGuardBatch name

#define MEM_GUARD_BATCH_ADD(name, ptr, tag) \
    (name).add((ptr), (tag), __FILE__, __LINE__, __FUNCTION__)

#define PERMANENT_SOUL(ptr) ayafileio::diagnostics::yuyuko::mark_permanent(ptr)

#define TRACKED_STRNCPY(dst, src, n) \
    do { \
        size_t len = std::strnlen((src), (n)); \
        if (ayafileio::diagnostics::yuyuko::check_bounds((dst), (n), __FILE__, __LINE__, __FUNCTION__)) { \
            std::strncpy((dst), (src), (n)); \
        } \
    } while(0)

#else

namespace ayafileio::diagnostics::yuyuko {
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
    inline size_t get_permanent_souls() { return 0; }
    inline size_t leak_check() { return 0; }
    inline void reserve_souls(size_t = 65536, size_t = 16384) {}
    inline void mark_permanent(void*) {}
    inline void mem_snapshot(void*, const char* = "default") {}
    inline void mem_snapshot_ex(void*, size_t, const char* = "default") {}
    inline bool mem_snapshot_update(void*, size_t, size_t) { return false; }
    inline bool mem_verify(void*) { return true; }
    inline bool mem_check(void*, const char*, int, const char*) { return true; }
    inline bool mem_check_ex(void*, size_t, const char*, int, const char*) { return true; }
    class MemGuard {
    public:
        MemGuard(void*, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    class MemGuardEx {
    public:
        MemGuardEx(void*, size_t, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    class MemGuardBatch {
    public:
        void add(void*, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    inline void generate_html_report(const char* = nullptr, const char* = nullptr) {}
} // namespace ayafileio::diagnostics::yuyuko

#define TRACKED_NEW(type)   new type
#define TRACKED_NEW_ARGS(type, ...)   new type(__VA_ARGS__)
#define TRACKED_PLACEMENT_NEW(type, raw_ptr)   new (raw_ptr) type
#define TRACKED_PLACEMENT_NEW_ARGS(type, raw_ptr, ...)   new (raw_ptr) type(__VA_ARGS__)
#define TRACKED_DELETE(ptr) delete ptr
#define TRACKED_ALLOC(size) std::malloc(size)
#define TRACKED_FREE(ptr)   std::free(ptr)
#define TRACKED_MEMSET(ptr, val, size)   std::memset((ptr), (val), (size))
#define TRACKED_MEMCPY(dst, src, size)   std::memcpy((dst), (src), (size))
#define TRACKED_MEMMOVE(dst, src, size)  std::memmove((dst), (src), (size))
#define TRACKED_STRCPY(dst, src)         std::strcpy((dst), (src))
#define TRACKED_STRNCPY(dst, src, n)     std::strncpy((dst), (src), (n))
#define MEM_SNAPSHOT(ptr, tag) ((void)0)
#define MEM_SNAPSHOT_UPDATE(ptr, off, n) (false)
#define MEM_VERIFY(ptr)        (true)
#define MEM_CHECK(ptr)         (true)
#define MEM_GUARD(ptr)         ((void)0)
#define MEM_GUARD_TAG(ptr, tag) ((void)0)
#define MEM_GUARD_EX(ptr, chunk_size) ((void)0)
#define MEM_GUARD_EX_TAG(ptr, chunk_size, tag) ((void)0)
#define MEM_GUARD_BATCH_DEF(name)       ayafileio::diagnostics::yuyuko::MemGuardBatch name
#define MEM_GUARD_BATCH_ADD(name, ptr, tag) ((void)0)
#define PERMANENT_SOUL(ptr) ((void)0)

#endif // ENABLE_YUYUKO
