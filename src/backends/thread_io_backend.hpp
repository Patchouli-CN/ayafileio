// thread_io_backend.hpp
#pragma once
#include "../io_backend.hpp"
#include "../global_thread_pool.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <functional>
#include <cstdint>
#include <condition_variable>
#include <vector>

namespace ayafileio {

class ThreadIOBackend : public IOBackendBase {
public:
    ThreadIOBackend(const std::string& path, const std::string& mode);
    ThreadIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~ThreadIOBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* read_at(int64_t offset, int64_t size) override;
    PyObject* write(Py_buffer* view, int64_t position = -1) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override { return m_fd; }
    void close_impl() override;

private:
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    uint64_t m_cachedFileSize = 0;  // open 时缓存，写/截断路径乐观更新
    bool m_appendMode = false;

    // 事件循环相关成员 - 延迟初始化
    std::atomic<bool> m_loop_initialized{false};
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;

    unsigned m_num_workers = 0;

    // 初始化方法
    void ensure_loop_initialized();
    void enqueue_task(std::function<void()> task);
};

} // namespace ayafileio