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
    void close_impl() override;

    void complete_ok(IORequest* req, size_t bytes) override;
    void complete_error(IORequest* req, DWORD err) override;

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    PoolKey m_poolKey;
    std::atomic<bool> m_running{false};
    std::atomic<long> m_pending{0};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    LoopHandle* m_loop_handle = nullptr;

    IORequest* make_req(size_t size, PyObject* future, ReqType type) override;
    void complete_error_inline(IORequest* req, DWORD err) override;

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