// io_backend.cpp
#include "io_backend.hpp"
#include "globals.hpp"

// ════════════════════════════════════════════════════════════════════════════
// 静态辅助方法
// ════════════════════════════════════════════════════════════════════════════

void IOBackendBase::resolve_ok(PyObject* future, PyObject* val) {
    PyObject* fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject* r  = PyObject_CallFunctionObjArgs(fn, val, nullptr);
    Py_XDECREF(r); Py_DECREF(fn);
}

void IOBackendBase::resolve_bytes(PyObject* future, const char* buf, Py_ssize_t n) {
    PyObject* b = PyBytes_FromStringAndSize(buf, n);
    resolve_ok(future, b); Py_DECREF(b);
}

void IOBackendBase::resolve_exc(PyObject* future, PyObject* cls, DWORD err, const char* msg) {
    PyObject* exc = err
        ? PyObject_CallFunction(cls, "is", (int)err, msg)
        : PyObject_CallFunction(cls, "s", msg);
    PyObject* fn = PyObject_GetAttr(future, g_str_set_exception);
    PyObject* r  = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
    Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
}

// ════════════════════════════════════════════════════════════════════════════
// 完成处理 — 所有后端共享
// ════════════════════════════════════════════════════════════════════════════

void IOBackendBase::complete_ok(IORequest* req, size_t bytes) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject* val;
    switch (req->type) {
        case ReqType::Read:
            if (req->isReadinto)
                val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
            else
                val = PyBytes_FromStringAndSize(req->buf(), static_cast<Py_ssize_t>(bytes));
            break;
        case ReqType::Write:
            val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
            break;
        default:
            val = Py_None;
            Py_INCREF(val);
            break;
    }

    PyObject* set_fn = req->set_result; req->set_result = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_exception); req->set_exception = nullptr;

    if (set_fn && val) req->loop_handle->push(set_fn, val);
    else { Py_XDECREF(set_fn); Py_XDECREF(val); }
    delete req;

    PyGILState_Release(gs);
}

void IOBackendBase::complete_error(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject* exc_class;
#ifdef _WIN32
    exc_class = map_win_error(static_cast<int>(err));
#else
    exc_class = map_posix_error(static_cast<int>(err));
#endif
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");

    PyObject* set_fn = req->set_exception; req->set_exception = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_result); req->set_result = nullptr;

    if (set_fn && exc) req->loop_handle->push(set_fn, exc);
    else { Py_XDECREF(set_fn); Py_XDECREF(exc); }
    delete req;

    PyGILState_Release(gs);
}

// ════════════════════════════════════════════════════════════════════════════
// 请求构造 — 所有后端共享
// ════════════════════════════════════════════════════════════════════════════

IORequest* IOBackendBase::make_req(size_t size, PyObject* future, ReqType type) {
    auto* req = new IORequest();
    req->file = this;
    req->loop_handle = m_loop_handle;
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize = size;
    req->type = type;

    if (size <= m_cached_buffer_size)
        req->poolBuf = pool_acquire_with_size(size);
    else
        req->heapBuf = new char[size];
    return req;
}

IORequest* IOBackendBase::make_req_readinto(PyObject* buf, Py_buffer* view, size_t size, PyObject* future) {
    auto* req = new IORequest();
    req->file = this;
    req->loop_handle = m_loop_handle;
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize = size;
    req->type = ReqType::Read;
    
    // readinto 专用设置
    req->isReadinto = true;
    req->userBuf = buf;
    Py_INCREF(buf);
    req->userBufView = *view;  // 复制 Py_buffer 结构体
    
    return req;
}

// ════════════════════════════════════════════════════════════════════════════
// 内联错误处理 — 所有后端共享
// ════════════════════════════════════════════════════════════════════════════

void IOBackendBase::complete_error_inline(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject* exc_class;
#ifdef _WIN32
    exc_class = map_win_error(static_cast<int>(err));
#else
    exc_class = map_posix_error(static_cast<int>(err));
#endif
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");
    PyObject* set_fn = req->set_exception; req->set_exception = nullptr;
    if (set_fn && exc) {
        PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
        Py_XDECREF(r);
    }
    Py_XDECREF(set_fn);
    Py_DECREF(exc);
    delete req;
}
