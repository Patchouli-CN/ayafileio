// global_thread_pool.hpp — 跨平台全局共享线程池
// ThreadIOBackend 的 fallback 使用，所有平台编译。
#pragma once
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>
#include <atomic>
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#include <stop_token>
#define AYAFILEIO_HAS_JTHREAD 1
#endif

namespace ayafileio {

class GlobalThreadPool {
public:
    static GlobalThreadPool& instance();

    // 首次启动：按 num_workers 起线程；之后调用是空操作（见 target_workers）
    void ensure_started(unsigned num_workers);
    // 把池调整到 num_workers：比当前多就补线程，比当前少就让多余 worker
    // 在取下一个任务前自行退出。池还没启动时是空操作——首次启动会用当时的
    // 配置值（effective_io_worker_count()）。
    void target_workers(unsigned num_workers);
    // 当前活着的 worker 数（诊断/测试用）
    unsigned running_workers() const {
        return m_running_workers.load(std::memory_order_acquire);
    }
    void enqueue(std::function<void()> task);
    void shutdown();

private:
    GlobalThreadPool() = default;
    ~GlobalThreadPool();

#ifdef AYAFILEIO_HAS_JTHREAD
    std::vector<std::jthread> m_workers;
    void worker_loop(std::stop_token st);
#else
    std::vector<std::thread> m_workers;
    void worker_loop();
#endif

    // 退役检查 / 退出时配平计数（worker_loop 用）
    bool worker_retire_due();
    void worker_mark_exit();

    std::mutex m_mtx;
    std::queue<std::function<void()>> m_tasks;
    std::condition_variable m_cv;
    std::atomic<unsigned> m_running_workers{0};
    std::atomic<unsigned> m_target{0};   // 期望 worker 数（缩容时 worker 自查退出）
    std::atomic<bool> m_stop{false};
};

// 诊断/回归测试：当前全局线程池的 worker 数
unsigned py_debug_pool_workers();

} // namespace ayafileio
