// windows_io_backend.hpp
#pragma once
#include "../io_backend.hpp"
#include "../iocp_context.hpp"
#include <string>

// ════════════════════════════════════════════════════════════════════════════
// §7  Windows IO Backend — lightweight forwarding layer
//
// All I/O state lives in IOCPContext::Session. WindowsIOBackend only holds
// a sessionId and delegates every operation to IOCPContext::submit_*().
// ════════════════════════════════════════════════════════════════════════════

class WindowsIOBackend : public IOBackendBase {
public:
    WindowsIOBackend(const std::string& path, const std::string& mode);
    WindowsIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~WindowsIOBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* read_at(int64_t offset, int64_t size) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override;
    void close_impl() override;

    // Completion handlers — unused; IOCPContext workers process completions
    // directly via sessionId lookup. Override as defensive no-ops.
    void complete_ok(IORequest* req, size_t /*bytes*/) override {
        if (req) delete req;
    }
    void complete_error(IORequest* req, DWORD /*err*/) override {
        if (req) delete req;
    }

private:
    uint64_t m_sessionId = 0;
};
