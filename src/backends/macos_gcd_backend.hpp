// macos_gcd_backend.hpp
#pragma once
#ifdef __APPLE__

#include "../io_backend.hpp"
#include "../debug_log.hpp"
#include <dispatch/dispatch.h>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

class MacOSGCDBackend : public IOBackendBase {
public:
    MacOSGCDBackend(const std::string& path, const std::string& mode);
    MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~MacOSGCDBackend() override;

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

private:
    dispatch_io_t m_channel = nullptr;
    dispatch_queue_t m_queue = nullptr;
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    std::string m_path;
    
    // Python 事件循环集成
    bool m_loop_initialized = false;
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    
    void ensure_loop_initialized();
};

#endif // __APPLE__