// windows_io_backend.hpp
#pragma once
#include "../io_backend.hpp"
#include "../iocp.hpp"
#include "../handle_pool.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// §7  Windows IO Backend
// ════════════════════════════════════════════════════════════════════════════

class WindowsIOBackend : public IOBackendBase {
public:
    WindowsIOBackend(const std::string& path, const std::string& mode);
    WindowsIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~WindowsIOBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override {
        // Windows: 把 HANDLE 转回 CRT fd
        return _open_osfhandle((intptr_t)m_handle, 0);
    }
    void close_impl() override;

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    PoolKey m_poolKey;
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;

    PyObject* check_closed_or_raise() {
        if (!m_running.load(std::memory_order_relaxed) || m_handle == INVALID_HANDLE_VALUE) {
            PyObject *future = PyObject_CallNoArgs(m_create_future);
            if (future) {
                resolve_exc(future, g_ValueError, 0, "I/O operation on closed file.");
            }
            return future;
        }
        return nullptr;
    }
};