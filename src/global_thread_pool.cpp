// global_thread_pool.cpp — 跨平台全局共享线程池实现
#include "global_thread_pool.hpp"
#include "utils/debug_log.hpp"

namespace ayafileio {

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
    m_stop.store(false, std::memory_order_release);
    m_workers.reserve(num_workers);

#ifdef AYAFILEIO_HAS_JTHREAD
    for (unsigned i = 0; i < num_workers; ++i) {
        m_workers.emplace_back(
            [this](std::stop_token st) { worker_loop(std::move(st)); });
    }
#else
    for (unsigned i = 0; i < num_workers; ++i) {
        m_workers.emplace_back(&GlobalThreadPool::worker_loop, this);
    }
#endif
    m_target.store(num_workers, std::memory_order_release);
    m_running_workers.store(num_workers, std::memory_order_release);
    UR_DEBUG_LOG("GlobalThreadPool: started %u workers", num_workers);
}

void GlobalThreadPool::target_workers(unsigned num_workers) {
    if (num_workers == 0) num_workers = 1;
    std::lock_guard<std::mutex> lk(m_mtx);
    // 还没启动：什么都不做。首次启动时调用方会用当时的配置值起池，
    // 不至于因为一次 configure 就凭空拉起一堆线程。
    if (m_workers.empty()) return;

    m_target.store(num_workers, std::memory_order_release);
    const unsigned cur = m_running_workers.load(std::memory_order_acquire);
    if (num_workers > cur) {
        m_stop.store(false, std::memory_order_release);
        for (unsigned i = cur; i < num_workers; ++i) {
#ifdef AYAFILEIO_HAS_JTHREAD
            m_workers.emplace_back(
                [this](std::stop_token st) { worker_loop(std::move(st)); });
#else
            m_workers.emplace_back(&GlobalThreadPool::worker_loop, this);
#endif
        }
        m_running_workers.store(num_workers, std::memory_order_release);
        UR_DEBUG_LOG("GlobalThreadPool: grow %u -> %u workers", cur, num_workers);
    } else if (num_workers < cur) {
        UR_DEBUG_LOG("GlobalThreadPool: shrink %u -> %u workers", cur, num_workers);
    }
    m_cv.notify_all();   // 扩容后叫醒新线程；缩容后叫醒多余线程自查退出
}

unsigned py_debug_pool_workers() {
    return GlobalThreadPool::instance().running_workers();
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
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();

#ifdef AYAFILEIO_HAS_JTHREAD
    for (auto& t : m_workers) t.request_stop();
    m_workers.clear();  // jthread dtor auto-joins
#else
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();
#endif
    UR_DEBUG_LOG0("GlobalThreadPool: all workers shut down");
}

#ifdef AYAFILEIO_HAS_JTHREAD
void GlobalThreadPool::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        if (worker_retire_due()) return;
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            // 只是在等任务：超时后回到循环顶部，重新评估「该不该退役」。
            // 之前这里是 `if (m_tasks.empty()) break;` —— 空闲 50ms 就把线程
            // 杀掉，m_running_workers 还不减，于是池在空闲后名存实亡。
            m_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_tasks.empty() || st.stop_requested();
            });
            if (m_tasks.empty()) continue;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        if (task) [[likely]] task();
    }
    worker_mark_exit();
}
#else
void GlobalThreadPool::worker_loop() {
    while (!m_stop.load(std::memory_order_acquire)) {
        if (worker_retire_due()) return;
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return !m_tasks.empty() || m_stop.load(std::memory_order_acquire);
            });
            if (m_tasks.empty()) continue;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        if (task) [[likely]] task();
    }
    worker_mark_exit();
}
#endif

// configure 缩容后：目标小于当前线程数时，多余 worker 在取下一个任务前退出。
// 返回 true 表示本线程已经把自己从计数里摘掉、应当结束。
bool GlobalThreadPool::worker_retire_due() {
    unsigned cur = m_running_workers.load(std::memory_order_acquire);
    const unsigned want = m_target.load(std::memory_order_acquire);
    while (cur > want) {
        if (m_running_workers.compare_exchange_weak(
                cur, cur - 1, std::memory_order_acq_rel)) {
            UR_DEBUG_LOG("GlobalThreadPool: worker retired, left=%u", cur - 1u);
            return true;
        }
    }
    return false;
}

// 停池路径：shutdown() 可能已经把计数 exchange(0)，所以只往下减、不减成负数。
void GlobalThreadPool::worker_mark_exit() {
    unsigned cur = m_running_workers.load(std::memory_order_acquire);
    while (cur > 0 && !m_running_workers.compare_exchange_weak(
                          cur, cur - 1, std::memory_order_acq_rel)) {
    }
}

} // namespace ayafileio
