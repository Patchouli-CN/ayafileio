// io_uring_backend.hpp
#pragma once
#ifdef HAVE_IO_URING

#include "../io_backend.hpp"
#include "../debug_log.hpp"
#include "uring_pool.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

class IOUringBackend : public IOBackendBase {
public:
    IOUringBackend(const std::string& path, const std::string& mode);
    IOUringBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~IOUringBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override { return m_fd; }
    void close_impl() override;

    // reaper 循环入口（由池调用）
    static void reaper_loop_entry(UringInstance* inst);

private:
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    
    // 事件循环相关
    bool m_loop_initialized = false;
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    
    // io_uring 实例（从池中获取）
    std::shared_ptr<UringInstance> m_uring;
    
    void ensure_loop_initialized();
    void submit_io(IORequest* req, int op, int fd, const void* buf, size_t len, off_t offset);
};

#endif // HAVE_IO_URING