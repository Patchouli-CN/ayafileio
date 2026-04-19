#pragma once
#include <cstddef>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "config.hpp"

// ════════════════════════════════════════════════════════════════════════════
// §2  Buffer pool (按大小分桶，支持动态配置)
// ════════════════════════════════════════════════════════════════════════════

struct PoolBuf {
    char* data;
    size_t size;
    
    explicit PoolBuf(size_t sz) : data(new char[sz]), size(sz) {}
    ~PoolBuf() { delete[] data; }
    
    // 禁止拷贝
    PoolBuf(const PoolBuf&) = delete;
    PoolBuf& operator=(const PoolBuf&) = delete;
};

// 按大小分桶的缓冲区池
class BufferPool {
public:
    static BufferPool& instance() {
        static BufferPool pool;
        return pool;
    }
    
    PoolBuf* acquire(size_t required_size) {
        std::lock_guard<std::mutex> lk(m_mutex);
        
        // 找到足够大的最小缓冲区
        auto it = m_pools.lower_bound(required_size);
        if (it != m_pools.end() && !it->second.empty()) {
            PoolBuf* buf = it->second.back();
            it->second.pop_back();
            m_total--;
            return buf;
        }
        
        // 没有合适的，分配新的
        return new PoolBuf(required_size);
    }
    
    void release(PoolBuf* buf) {
        if (!buf) return;
        
        std::lock_guard<std::mutex> lk(m_mutex);
        
        size_t max_total = ayafileio::config().buffer_pool_max();
        if (m_total >= max_total) {
            delete buf;
            return;
        }
        
        m_pools[buf->size].push_back(buf);
        m_total++;
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
    BufferPool() = default;
    
    mutable std::mutex m_mutex;
    std::unordered_map<size_t, std::vector<PoolBuf*>> m_pools;
    size_t m_total = 0;
};

// 便捷函数
inline PoolBuf* pool_acquire() {
    return BufferPool::instance().acquire(ayafileio::config().buffer_size());
}

inline PoolBuf* pool_acquire_with_size(size_t size) {
    return BufferPool::instance().acquire(size);
}

inline void pool_release(PoolBuf* p) {
    BufferPool::instance().release(p);
}

inline void pool_clear() {
    BufferPool::instance().clear();
}