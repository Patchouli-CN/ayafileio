#include "pool.hpp"
#include "config.hpp"

static std::mutex g_poolMtx;
static std::vector<PoolBuf*> g_pool;

PoolBuf* pool_acquire() {
    std::lock_guard<std::mutex> lk(g_poolMtx);
    if (!g_pool.empty()) {
        auto* p = g_pool.back();
        g_pool.pop_back();
        return p;
    }
    // 使用动态配置的缓冲区大小
    return new PoolBuf(ayafileio::config().buffer_size());
}

void pool_release(PoolBuf* p) {
    std::lock_guard<std::mutex> lk(g_poolMtx);
    if (g_pool.size() < ayafileio::config().buffer_pool_max()) {
        g_pool.push_back(p);
    } else {
        delete p;
    }
}