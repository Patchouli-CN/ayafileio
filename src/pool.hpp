#pragma once
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <map>
#include <algorithm>
#include "config.hpp"

// ════════════════════════════════════════════════════════════════════════════
// §2  Buffer pool (按大小分桶，支持动态配置)
// ════════════════════════════════════════════════════════════════════════════

struct PoolBuf {
    char* data;
    size_t size;

    explicit PoolBuf(size_t sz) : data(static_cast<char*>(std::malloc(sz))), size(sz) {
        if (!data) throw std::bad_alloc();
    }
    ~PoolBuf() { std::free(data); }
    
    // 禁止拷贝
    PoolBuf(const PoolBuf&) = delete;
    PoolBuf& operator=(const PoolBuf&) = delete;
};

// 按大小分桶缓冲区池
class BufferPool {
public:
    static BufferPool& instance() {
        static BufferPool pool;
        return pool;
    }

    PoolBuf* acquire(size_t required_size) {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 找到足够大的最小缓冲区（使用 map 的 lower_bound）
        auto it = m_pools.lower_bound(required_size);
        if (it != m_pools.end() && !it->second.empty()) [[likely]] {
            PoolBuf* buf = it->second.back();
            it->second.pop_back();
            m_total--;
            return buf;
        }

        // 没有合适的，分配新的 — 冷路径
        return new PoolBuf(required_size);
    }

    void release(PoolBuf* buf) {
        if (!buf) return;

        std::lock_guard<std::mutex> lk(m_mutex);

        if (m_total >= m_cached_max.load(std::memory_order_relaxed)) {
            delete buf;
            return;
        }

        m_pools[buf->size].push_back(buf);
        m_total++;
    }

    void refresh_config() {
        m_cached_max.store(ayafileio::config().buffer_pool_max(), std::memory_order_relaxed);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& pair : m_pools) {
            for (auto* buf : pair.second) {
                delete buf;
            }
        }
        m_pools.clear();
        m_total = 0;
    }

    size_t total_buffers() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_total;
    }

private:
    BufferPool() {
        m_cached_max.store(ayafileio::config().buffer_pool_max(), std::memory_order_relaxed);
    }

    mutable std::mutex m_mutex;
    std::map<size_t, std::vector<PoolBuf*>> m_pools;
    size_t m_total = 0;
    std::atomic<size_t> m_cached_max{512};
};

// 便捷函数（带线程本地缓存，减少全局锁竞争）

namespace detail {
    static constexpr size_t TC_MAX = 8;
    inline std::vector<PoolBuf*>& thread_local_cache() {
        thread_local std::vector<PoolBuf*> cache;
        return cache;
    }
}

inline PoolBuf* pool_acquire_with_size(size_t size) {
    auto& tlc = detail::thread_local_cache();
    for (auto it = tlc.begin(); it != tlc.end(); ++it) {
        if ((*it)->size >= size) [[likely]] {
            PoolBuf* buf = *it;
            tlc.erase(it);
            return buf;
        }
    }
    return BufferPool::instance().acquire(size);
}

inline PoolBuf* pool_acquire() {
    return pool_acquire_with_size(ayafileio::config().buffer_size());
}

inline void pool_release(PoolBuf* p) {
    if (!p) return;
    auto& tlc = detail::thread_local_cache();
    if (tlc.size() < detail::TC_MAX) {
        tlc.push_back(p);
        return;
    }
    BufferPool::instance().release(p);
}

inline void pool_clear() {
    BufferPool::instance().clear();
}