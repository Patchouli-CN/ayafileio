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
    if (m_running_workers.load(std::memory_order_acquire) > 0) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_workers.empty()) return;
    m_workers.reserve(num_workers);
    for (unsigned i = 0; i < num_workers; ++i) {
        m_workers.emplace_back(
            [this](std::stop_token st) { worker_loop(std::move(st)); });
    }
    m_running_workers.store(num_workers, std::memory_order_release);
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
    unsigned n = m_running_workers.exchange(0, std::memory_order_acq_rel);
    if (n == 0) return;
    m_cv.notify_all();
    for (auto& t : m_workers) {
        t.request_stop();
    }
    m_workers.clear(); // jthread destructor auto-joins
    UR_DEBUG_LOG0("GlobalThreadPool: all workers shut down");
}

void GlobalThreadPool::worker_loop(std::stop_token st) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(50),
                          [this, &st] { return !m_tasks.empty() || st.stop_requested(); });
            if (m_tasks.empty() && st.stop_requested()) return;
            if (m_tasks.empty()) continue;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        if (task) [[likely]] task();
    }
}
