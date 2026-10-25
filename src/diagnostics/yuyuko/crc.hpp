#pragma once

#include "memory_record.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace ayafileio::diagnostics::yuyuko {

static constexpr size_t CRC_CHUNK_SIZE = 4096;

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#define YUYUKO_HW_CRC32C 1
namespace detail {
inline uint32_t hw_crc32c(uint32_t crc, const uint8_t* data, size_t len) {
    while (len >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, chunk);
        data += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t chunk;
        std::memcpy(&chunk, data, 4);
        crc = _mm_crc32_u32(crc, chunk);
        data += 4;
        len -= 4;
    }
    while (len-- > 0)
        crc = _mm_crc32_u8(crc, *data++);
    return crc;
}
} // namespace detail

#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define YUYUKO_HW_CRC32C 1
namespace detail {
inline uint32_t hw_crc32c(uint32_t crc, const uint8_t* data, size_t len) {
    while (len >= 8) {
        crc = __crc32cd(crc, *(const uint64_t*)data);
        data += 8;
        len -= 8;
    }
    while (len >= 4) {
        crc = __crc32cw(crc, *(const uint32_t*)data);
        data += 4;
        len -= 4;
    }
    while (len-- > 0)
        crc = __crc32cb(crc, *data++);
    return crc;
}
} // namespace detail
#endif

namespace detail {
inline const uint32_t* crc32c_table() {
    static const uint32_t* tbl = []() -> const uint32_t* {
        static uint32_t t[256];
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1) ? 0x82F63B78u : 0);
            t[i] = c;
        }
        return t;
    }();
    return tbl;
}
} // namespace detail

inline uint32_t crc32c_update(uint32_t crc, const uint8_t* data, size_t len) {
#ifdef YUYUKO_HW_CRC32C
    return detail::hw_crc32c(crc, data, len);
#else
    const uint32_t* tbl = detail::crc32c_table();
    for (size_t i = 0; i < len; ++i)
        crc = tbl[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
#endif
}

inline std::vector<uint32_t> crc32_chunked(const uint8_t* data, size_t size, size_t chunk_size) {
    if (chunk_size == 0) chunk_size = CRC_CHUNK_SIZE;
    std::vector<uint32_t> crcs;
    size_t n_chunks = (size + chunk_size - 1) / chunk_size;
    if (n_chunks == 0) n_chunks = 1;
    crcs.reserve(n_chunks);
    for (size_t off = 0; off < size; off += chunk_size) {
        size_t len = (off + chunk_size <= size) ? chunk_size : (size - off);
        crcs.push_back(crc32c_update(0xFFFFFFFFu, data + off, len));
    }
    return crcs;
}

inline std::vector<uint32_t> crc32_chunked(const uint8_t* data, size_t size) {
    return crc32_chunked(data, size, CRC_CHUNK_SIZE);
}

struct MemCheckpoint {
    std::vector<uint32_t> crcs;
    uint64_t timestamp;
    size_t chunk_size;
    char tag[32];
};

inline std::unordered_map<void*, MemCheckpoint> g_checkpoints;
inline std::shared_mutex g_cp_mtx;

inline void mem_snapshot_ex(void* ptr, size_t chunk_size, const char* tag = "default") {
    if (!ptr) return;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_alive.find(ptr);
    if (it == g_soul_alive.end()) return;
    size_t sz = it->second.size;
    lk.unlock();
    MemCheckpoint cp;
    cp.crcs = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, chunk_size);
    cp.timestamp = current_time_us();
    cp.chunk_size = chunk_size;
    { size_t i = 0; while (i < sizeof(cp.tag) - 1 && tag[i]) { cp.tag[i] = tag[i]; ++i; } cp.tag[i] = '\0'; }
    std::unique_lock<std::shared_mutex> clk(g_cp_mtx);
    g_checkpoints[ptr] = std::move(cp);
}

inline void mem_snapshot(void* ptr, const char* tag = "default") {
    mem_snapshot_ex(ptr, CRC_CHUNK_SIZE, tag);
}

inline bool mem_snapshot_update(void* ptr, size_t offset, size_t len) {
    if (!ptr || len == 0) return false;

    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) return false;
    size_t sz = ait->second.size;
    if (offset >= sz) return false;
    slk.unlock();

    std::unique_lock<std::shared_mutex> clk(g_cp_mtx);
    auto cit = g_checkpoints.find(ptr);
    if (cit == g_checkpoints.end()) return false;

    size_t cs = cit->second.chunk_size ? cit->second.chunk_size : CRC_CHUNK_SIZE;
    auto& crcs = cit->second.crcs;
    if (crcs.empty()) return false;

    size_t chunk_start = offset / cs;
    size_t chunk_end = (offset + len + cs - 1) / cs;
    if (chunk_end > crcs.size()) chunk_end = crcs.size();
    if (chunk_start >= crcs.size()) return false;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(ptr);
    for (size_t i = chunk_start; i < chunk_end; ++i) {
        size_t chunk_off = i * cs;
        size_t chunk_len = (chunk_off + cs <= sz) ? cs : (sz - chunk_off);
        crcs[i] = crc32c_update(0xFFFFFFFFu, data + chunk_off, chunk_len);
    }
    cit->second.timestamp = current_time_us();
    return true;
}

inline bool mem_verify(void* ptr) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) return false;
    size_t sz = ait->second.size;
    slk.unlock();
    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto cit = g_checkpoints.find(ptr);
    if (cit == g_checkpoints.end()) return true;
    size_t cs = cit->second.chunk_size ? cit->second.chunk_size : CRC_CHUNK_SIZE;
    auto now = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, cs);
    return now == cit->second.crcs;
}

inline bool mem_check_ex(void* ptr, size_t chunk_size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) {
        if (g_soul_released.find(ptr) != g_soul_released.end())
            detail::yuyuko_logln("[Yuyuko] CHECK %p FREED @ %s:%d %s()", ptr, file, line, func);
        else
            detail::yuyuko_logln("[Yuyuko] CHECK %p NOT TRACKED @ %s:%d %s()", ptr, file, line, func);
        return false;
    }
    const SoulRecord& s = ait->second;
    size_t sz = s.size;
    const char* af = s.file;
    int al = s.line;
    const char* ath = s.thread_name;
    const char* ati = s.thread_id;
    slk.unlock();

    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto cit = g_checkpoints.find(ptr);
    if (cit == g_checkpoints.end()) {
        clk.unlock();
        detail::yuyuko_logln("[Yuyuko] CHECK %p NO-SNAPSHOT @ %s:%d %s() — mem_snapshot first",
            ptr, file, line, func);
        return false;
    }
    const auto& old = cit->second.crcs;
    uint64_t elapsed = current_time_us() - cit->second.timestamp;
    const char* tag = cit->second.tag;
    size_t cs = cit->second.chunk_size ? cit->second.chunk_size : chunk_size;

    auto now_crcs = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, cs);

    if (now_crcs.size() != old.size()) {
        clk.unlock();
        detail::yuyuko_logln("[Yuyuko] CHECK %p SIZE-CHANGE chunks %zu→%zu @ %s:%d %s()",
            ptr, old.size(), now_crcs.size(), file, line, func);
        return false;
    }

    if (cs == 0) cs = CRC_CHUNK_SIZE;
    for (size_t i = 0; i < now_crcs.size(); ++i) {
        if (now_crcs[i] != old[i]) {
            size_t corrupt_off = i * cs;
            size_t corrupt_end = (corrupt_off + cs < sz) ? corrupt_off + cs : sz;
            clk.unlock();
            detail::yuyuko_logln(
                "[Yuyuko] MEM-CORRUPTION %p chunk[%zu] offset=%zu-%zu size=%zu cs=%zu @ %s:%d %s()\n"
                "  snapshot: [%s] crc[%zu]=0x%08X (%s ago)\n"
                "  now:      crc[%zu]=0x%08X (alloc'd %s:%d [%s/%s])",
                ptr, i, corrupt_off, corrupt_end, sz, cs, file, line, func,
                tag, i, old[i], format_duration_us(elapsed),
                i, now_crcs[i], af, al, ath, ati);
            return false;
        }
    }
    return true;
}

inline bool mem_check(void* ptr, const char* file, int line, const char* func) {
    return mem_check_ex(ptr, CRC_CHUNK_SIZE, file, line, func);
}

class MemGuard {
    void* m_ptr;
    const char* m_file;
    int m_line;
    const char* m_func;
    bool m_active;
public:
    MemGuard(void* ptr, const char* tag, const char* file, int line, const char* func)
        : m_ptr(ptr), m_file(file), m_line(line), m_func(func), m_active(ptr != nullptr) {
        if (m_active) mem_snapshot(ptr, tag);
    }
    ~MemGuard() {
        if (m_active) mem_check(m_ptr, m_file, m_line, m_func);
    }
    MemGuard(const MemGuard&) = delete;
    MemGuard& operator=(const MemGuard&) = delete;

    void dismiss() { m_active = false; }
};

class MemGuardEx {
    void* m_ptr;
    size_t m_chunk_size;
    const char* m_file;
    int m_line;
    const char* m_func;
    bool m_active;
public:
    MemGuardEx(void* ptr, size_t chunk_size, const char* tag,
               const char* file, int line, const char* func)
        : m_ptr(ptr), m_chunk_size(chunk_size), m_file(file), m_line(line), m_func(func), m_active(ptr != nullptr) {
        if (m_active) mem_snapshot_ex(ptr, chunk_size, tag);
    }
    ~MemGuardEx() {
        if (m_active) mem_check_ex(m_ptr, m_chunk_size, m_file, m_line, m_func);
    }
    MemGuardEx(const MemGuardEx&) = delete;
    MemGuardEx& operator=(const MemGuardEx&) = delete;

    void dismiss() { m_active = false; }
};

class MemGuardBatch {
    struct Entry { void* ptr; const char* file; int line; const char* func; };
    std::vector<Entry> m_entries;
    bool m_active = true;
public:
    MemGuardBatch() = default;

    void add(void* ptr, const char* tag, const char* file, int line, const char* func) {
        if (ptr) {
            mem_snapshot(ptr, tag);
            m_entries.push_back({ptr, file, line, func});
        }
    }

    ~MemGuardBatch() {
        if (!m_active) return;
        for (auto& e : m_entries)
            mem_check(e.ptr, e.file, e.line, e.func);
    }

    MemGuardBatch(const MemGuardBatch&) = delete;
    MemGuardBatch& operator=(const MemGuardBatch&) = delete;
    void dismiss() { m_active = false; }
};

} // namespace ayafileio::diagnostics::yuyuko
