#pragma once
#include <cstddef>
#include <mutex>
#include <vector>
#include "config.hpp"

// ════════════════════════════════════════════════════════════════════════════
// §2  Buffer pool (动态大小)
// ════════════════════════════════════════════════════════════════════════════

// 获取当前缓冲区大小（动态）
inline size_t get_pool_buf_size() {
    return ayafileio::config().buffer_size();
}

// 获取池大小限制
inline size_t get_pool_max() {
    return ayafileio::config().buffer_pool_max();
}

// 注意：PoolBuf 的大小现在是动态的，需要改为指针 + 大小管理
struct PoolBuf {
    char* data;
    size_t size;
    
    PoolBuf(size_t sz) : data(new char[sz]), size(sz) {}
    ~PoolBuf() { delete[] data; }
    
    // 禁止拷贝
    PoolBuf(const PoolBuf&) = delete;
    PoolBuf& operator=(const PoolBuf&) = delete;
};

PoolBuf* pool_acquire();
void pool_release(PoolBuf* p);