// io_backend.hpp
#pragma once
#include "globals.hpp"
#include "io_request.hpp"
#include <string>
#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// §3  IO Backend Base Class
// ════════════════════════════════════════════════════════════════════════════

class IOBackendBase {
public:
    IOBackendBase() = default;
    virtual ~IOBackendBase() = default;

    virtual PyObject* read(int64_t size = -1) = 0;
    virtual PyObject* write(Py_buffer* view) = 0;
    virtual PyObject* seek(int64_t offset, int whence = 0) = 0;
    virtual PyObject* flush() = 0;
    virtual PyObject* close() = 0;
    virtual PyObject* tell() = 0;
    virtual PyObject* truncate(int64_t size) = 0;
    virtual PyObject* readinto(PyObject* buf) = 0;
    virtual int fileno() const = 0;
    virtual void close_impl() = 0;

    // ════════════════════════════════════════════════════════════════════════
    // 公共实现 — 所有后端共享（在 io_backend.cpp 中实现）
    // ════════════════════════════════════════════════════════════════════════

    virtual void complete_ok(IORequest* req, size_t bytes);
    virtual void complete_error(IORequest* req, DWORD err);

protected:
    virtual IORequest* make_req(size_t size, PyObject* future, ReqType type);
    virtual IORequest* make_req_readinto(PyObject* buf, Py_buffer* view, size_t size, PyObject* future);
    virtual void complete_error_inline(IORequest* req, DWORD err);

    static void resolve_ok(PyObject* future, PyObject* val);
    static void resolve_bytes(PyObject* future, const char* buf, Py_ssize_t n);
    static void resolve_exc(PyObject* future, PyObject* cls, DWORD err, const char* msg);

    // 子类需要设置的共享状态
    LoopHandle* m_loop_handle = nullptr;
    std::atomic<long> m_pending{0};
    
    size_t m_cached_buffer_size = 65536;
    size_t m_cached_buffer_pool_max = 512;
    unsigned m_cached_close_timeout_ms = 4000;
    unsigned m_cached_io_uring_queue_depth = 256;
    unsigned m_cached_io_uring_flags = 0;
    bool m_cached_io_uring_sqpoll = false;
    bool m_owns_fd = true;
};
