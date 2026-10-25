#pragma once

#include "async_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ayafileio::diagnostics::yuyuko {

inline const char* cached_thread_id() {
    thread_local char buf[20] = {};
    if (buf[0] == '\0') {
        auto id = std::hash<std::thread::id>{}(std::this_thread::get_id());
        snprintf(buf, sizeof(buf), "0x%zx", id);
    }
    return buf;
}

inline const char* cached_thread_name() {
    thread_local char buf[64] = {};
    if (buf[0] == '\0') {
#ifdef _WIN32
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
            WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
            LocalFree(name);
        } else {
            snprintf(buf, sizeof(buf), "tid-%s", cached_thread_id());
        }
#else
        snprintf(buf, sizeof(buf), "tid-%s", cached_thread_id());
#endif
    }
    return buf;
}

inline uint64_t current_time_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline const char* format_duration_us(uint64_t us) {
    thread_local char buf[32];
    if (us < 1000)
        snprintf(buf, sizeof(buf), "%lluμs", (unsigned long long)us);
    else if (us < 1000000)
        snprintf(buf, sizeof(buf), "%.2fms", us / 1000.0);
    else if (us < 60000000)
        snprintf(buf, sizeof(buf), "%.2fs", us / 1000000.0);
    else {
        auto min = us / 60000000;
        auto sec = (us % 60000000) / 1000000;
        snprintf(buf, sizeof(buf), "%llumin%llus",
                 (unsigned long long)min, (unsigned long long)sec);
    }
    return buf;
}

enum class AllocSource : uint8_t {
    OPERATOR_NEW,
    STD_MALLOC
};

struct SoulRecord {
    void* ptr;
    size_t size;
    uint64_t timestamp;
    uint16_t line;
    uint8_t source;
    char _pad[1];
    char file[80];
    char func[64];
    char thread_name[64];
    char thread_id[20];
};
static_assert(sizeof(SoulRecord) <= 256, "SoulRecord too large");

inline std::shared_mutex g_soul_mtx;
inline std::unordered_map<void*, SoulRecord> g_soul_alive;
inline std::unordered_map<void*, SoulRecord> g_soul_released;
inline std::unordered_set<void*> g_soul_permanent;
inline std::set<uintptr_t> g_soul_alive_range;
inline std::set<uintptr_t> g_soul_released_range;
inline bool _soul_book_init = [] {
    g_soul_alive.reserve(65536);
    g_soul_released.reserve(16384);
    return true;
}();

inline const SoulRecord* find_soul(void* ptr) {
    auto it = g_soul_alive.find(ptr);
    if (it != g_soul_alive.end()) return &it->second;
    auto rit = g_soul_released.find(ptr);
    return (rit != g_soul_released.end()) ? &rit->second : nullptr;
}

inline void str_copy(char* dst, size_t cap, const char* src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

inline void register_soul_impl(void* ptr, size_t size, AllocSource source,
                               const char* file, int line, const char* func) {
    SoulRecord sr;
    sr.ptr = ptr;
    sr.size = size;
    sr.source = static_cast<uint8_t>(source);
    sr.timestamp = current_time_us();
    sr.line = static_cast<uint16_t>(line);
    str_copy(sr.file, sizeof(sr.file), file);
    str_copy(sr.func, sizeof(sr.func), func);
    str_copy(sr.thread_name, sizeof(sr.thread_name), cached_thread_name());
    str_copy(sr.thread_id, sizeof(sr.thread_id), cached_thread_id());

    {
        std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
        auto rit = g_soul_released.find(ptr);
        if (rit != g_soul_released.end()) {
            g_soul_released_range.erase(reinterpret_cast<uintptr_t>(ptr));
            g_soul_released.erase(rit);
        }
        g_soul_permanent.erase(ptr);
        g_soul_alive[ptr] = sr;
        g_soul_alive_range.insert(reinterpret_cast<uintptr_t>(ptr));
    }

    detail::yuyuko_trace(
        "[Yuyuko] ALLOC %p size=%zu src=%s @ %s:%d %s() [%s/%s]",
        ptr, size,
        (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc",
        file, line, func, sr.thread_name, sr.thread_id);
}

inline bool release_soul_impl(void* ptr, AllocSource source,
                              const char* file, int line, const char* func) {
    if (!ptr) return false;
    const char* tn = cached_thread_name();
    const char* ti = cached_thread_id();
    uint64_t now = current_time_us();
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    std::unique_lock<std::shared_mutex> lk(g_soul_mtx);

    auto rit = g_soul_released.find(ptr);
    if (rit != g_soul_released.end()) {
        detail::yuyuko_logln("[Yuyuko] DOUBLE-FREE %p freed %s ago\n"
            "  1st: [%s/%s] %s:%d %s()\n  2nd: [%s/%s] %s:%d %s()",
            ptr, format_duration_us(now - rit->second.timestamp),
            rit->second.thread_name, rit->second.thread_id,
            rit->second.file, rit->second.line, rit->second.func,
            tn, ti, file, line, func);
        return false;
    }

    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) {
        detail::yuyuko_logln("[Yuyuko] FREE-UNKNOWN %p @ %s:%d [%s/%s]", ptr, file, line, tn, ti);
        return false;
    }

    SoulRecord& s = ait->second;
    if (s.source != static_cast<uint8_t>(source)) {
        detail::yuyuko_logln("[Yuyuko] MISMATCH %p reg=%s tried=%s @ %s:%d [%s/%s]",
            ptr, (s.source == 0) ? "new" : "malloc",
            (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc", file, line, tn, ti);
        return false;
    }

    detail::yuyuko_trace("[Yuyuko] FREE %p life=%s src=%s [%s/%s]",
        ptr, format_duration_us(now - s.timestamp),
        (source == AllocSource::OPERATOR_NEW) ? "new" : "malloc", tn, ti);
    s.timestamp = now;
    g_soul_released.emplace(ptr, std::move(s));
    g_soul_alive.erase(ait);
    g_soul_alive_range.erase(addr);
    g_soul_released_range.insert(addr);
    return true;
}

inline void register_new(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::OPERATOR_NEW, file, line, func);
}

inline void register_malloc(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::STD_MALLOC, file, line, func);
}

inline bool release_new(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::OPERATOR_NEW, file, line, func);
}

inline bool release_malloc(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::STD_MALLOC, file, line, func);
}

inline bool check_access(void* ptr, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_released.find(ptr);
    if (it == g_soul_released.end()) return true;
    detail::yuyuko_logln(
        "[Yuyuko] USE-AFTER-FREE %p @ %s:%d %s() — %s ago [%s/%s]",
        ptr, file, line, func, format_duration_us(current_time_us() - it->second.timestamp),
        it->second.thread_name, it->second.thread_id);
    return false;
}

inline const SoulRecord* find_containing_soul(uintptr_t ps, uintptr_t pe) {
    auto lookup = [&](const std::set<uintptr_t>& range,
                      const std::unordered_map<void*, SoulRecord>& map) -> const SoulRecord* {
        if (range.empty()) return nullptr;
        auto it = range.upper_bound(ps);
        if (it == range.begin()) return nullptr;
        --it;
        auto sit = map.find(reinterpret_cast<void*>(*it));
        if (sit == map.end()) return nullptr;
        uintptr_t as = reinterpret_cast<uintptr_t>(sit->second.ptr);
        return (ps >= as && pe <= as + sit->second.size) ? &sit->second : nullptr;
    };
    const SoulRecord* s = lookup(g_soul_alive_range, g_soul_alive);
    return s ? s : lookup(g_soul_released_range, g_soul_released);
}

inline bool check_bounds(
    void* ptr, size_t size, const char* file, int line, [[maybe_unused]] const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    uintptr_t ps = reinterpret_cast<uintptr_t>(ptr), pe = ps + size;

    auto ait = g_soul_alive.find(ptr);
    if (ait != g_soul_alive.end()) {
        SoulRecord& s = ait->second;
        uintptr_t ae = reinterpret_cast<uintptr_t>(s.ptr) + s.size;
        if (pe > ae) {
            detail::yuyuko_logln("[Yuyuko] BUFFER-OVERFLOW %p+%zu @ %s:%d alloc=%zu @ %s:%d",
                ptr, size, file, line, s.size, s.file, s.line);
            return false;
        }
        return true;
    }

    const SoulRecord* s = find_containing_soul(ps, pe);
    if (!s) {
        detail::yuyuko_logln("[Yuyuko] WILD-WRITE %p+%zu @ %s:%d — not in any tracked allocation",
            ptr, size, file, line);
        return false;
    }
    return true;
}

inline bool is_released(void* ptr) {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_released.find(ptr) != g_soul_released.end();
}

inline size_t get_size(void* ptr) {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    {
        auto it = g_soul_alive.find(ptr);
        if (it != g_soul_alive.end()) return it->second.size;
    }
    {
        auto it = g_soul_released.find(ptr);
        if (it != g_soul_released.end()) return it->second.size;
    }
    return 0;
}

inline size_t get_total_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_alive.size() + g_soul_released.size();
}

inline size_t get_alive_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_alive.size();
}

inline size_t get_permanent_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_permanent.size();
}

inline void reserve_souls(size_t alive_cap = 65536, size_t released_cap = 16384) {
    std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
    g_soul_alive.reserve(alive_cap);
    g_soul_released.reserve(released_cap);
}

inline void mark_permanent(void* ptr) {
    if (!ptr) return;
    std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_alive.find(ptr);
    if (it == g_soul_alive.end()) return;
    g_soul_permanent.insert(ptr);
}

inline size_t leak_check() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    size_t total = g_soul_alive.size() + g_soul_released.size();
    size_t leaks = 0;
    for (auto& kv : g_soul_alive) {
        if (g_soul_permanent.count(kv.first)) continue;
        const SoulRecord& sr = kv.second;
        ++leaks;
        detail::yuyuko_logln(
            "[Yuyuko] LEAK %p %zub [%s/%s] %s:%d %s() age=%s",
            sr.ptr, sr.size, sr.thread_name, sr.thread_id,
            sr.file, sr.line, sr.func,
            format_duration_us(current_time_us() - sr.timestamp));
    }
    size_t permanent = g_soul_permanent.size();
    detail::yuyuko_logln(leaks ? "[Yuyuko] leak: %zu/%zu (permanent: %zu)"
                               : "[Yuyuko] no leaks (permanent: %zu/%zu)",
                         leaks, total, permanent);
    return leaks;
}

} // namespace ayafileio::diagnostics::yuyuko
