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

class GlobalThreadPool {
public:
    static GlobalThreadPool& instance();

    void ensure_started(unsigned num_workers);
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

    std::mutex m_mtx;
    std::queue<std::function<void()>> m_tasks;
    std::condition_variable m_cv;
    std::atomic<unsigned> m_running_workers{0};
    std::atomic<bool> m_stop{false};
};
