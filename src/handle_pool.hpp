// Cross-platform handle pool header: provide a single, safe interface
#pragma once
#include "globals.hpp"
#include <string>
#include <shared_mutex>
#include <vector>
#include <unordered_map>

// ════════════════════════════════════════════════════════════════════════════
// Handle Pool — the pool implementation is Windows-specific but the
// interface is exported on all platforms. On non-Windows platforms the
// implementation is a no-op / stub so the Python-visible API remains
// consistent and headers can be included without pulling Windows types.
// ════════════════════════════════════════════════════════════════════════════

struct PoolKey {
    std::string path;   // canonical (lowercased) path
    DWORD       access; // GENERIC_READ | GENERIC_WRITE combination (platform-defined)
    DWORD       disp;   // creation disposition (platform-defined)

    bool operator==(const PoolKey &o) const noexcept {
        return access == o.access && disp == o.disp && path == o.path;
    }
};

struct PoolKeyHash {
    size_t operator()(const PoolKey &k) const noexcept {
        size_t h = std::hash<std::string>{}(k.path);
        h ^= std::hash<DWORD>{}(k.access) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<DWORD>{}(k.disp)   + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Default values can be overridden via runtime API.
static constexpr size_t HANDLE_POOL_DEFAULT_MAX_PER_KEY = 64;
static constexpr size_t HANDLE_POOL_DEFAULT_MAX_TOTAL   = 2048;

// Try to acquire a cached HANDLE. Returns INVALID_HANDLE_VALUE on miss.
HANDLE handle_pool_acquire(const PoolKey &key);

// Return a HANDLE to the pool. Closes it immediately if pool is full.
void handle_pool_release(const PoolKey &key, HANDLE h);

// Remove all cached handles for a given key and close them.
// Called when an apparently-valid handle fails (e.g. IOCP association error).
void handle_pool_evict(const PoolKey &key);

// Close all pooled handles (called at shutdown).
void handle_pool_drain();

// Build a canonical PoolKey from open() parameters.
PoolKey make_pool_key(const std::string &path, DWORD access, DWORD disp);

// Runtime control of pool sizes
void set_handle_pool_limits(size_t max_per_key, size_t max_total);
std::pair<size_t, size_t> get_handle_pool_limits();

// ════════════════════════════════════════════════════════════════════════════
// POSIX stub：非 Windows 平台不需要句柄缓存
// ════════════════════════════════════════════════════════════════════════════
#ifndef _WIN32
inline HANDLE handle_pool_acquire(const PoolKey&) { return INVALID_HANDLE_VALUE; }
inline void handle_pool_release(const PoolKey&, HANDLE h) { if (h != -1) ::close(h); }
inline void handle_pool_evict(const PoolKey&) {}
inline void handle_pool_drain() {}
inline PoolKey make_pool_key(const std::string& path, DWORD access, DWORD disp) {
    return PoolKey{path, access, disp};
}
inline void set_handle_pool_limits(size_t, size_t) {}
inline std::pair<size_t, size_t> get_handle_pool_limits() { return {64, 2048}; }
#endif
