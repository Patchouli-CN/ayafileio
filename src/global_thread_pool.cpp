// global_thread_pool.cpp — 跨平台全局共享线程池实现
#include "global_thread_pool.hpp"
#include "utils/debug_log.hpp"

GlobalThreadPool& GlobalThreadPool::instance() {
    static GlobalThreadPool pool;
    return pool;
}

GlobalThreadPool::~GlobalThreadPool() {
    shutdown();
}

void GlobalThreadPool::ensure_started(unsigned num_workers) {
    if (!m_workers.empty()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_workers.empty()) return;
    m_workers.reserve(num_workers);
    for (unsigned i = 0; i < num_workers; ++i) {
        m_workers.emplace_back(&GlobalThreadPool::worker_loop, this);
    }
    UR_DEBUG_LOG("GlobalThreadPool: started %u workers", num_workers);
}

void GlobalThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_tasks.push(std::move(task));
    }
    m_cv.notify_one();
}

void GlobalThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop.load(std::memory_order_acquire)) return;
        m_stop.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();
    UR_DEBUG_LOG0("GlobalThreadPool: all workers shut down");
}

void GlobalThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] {
                return m_stop.load(std::memory_order_acquire) || !m_tasks.empty();
            });
            if (m_stop.load(std::memory_order_acquire) && m_tasks.empty()) {
                return;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        if (task) task();
    }
}
