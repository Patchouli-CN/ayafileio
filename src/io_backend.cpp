// io_backend.cpp
#include "io_backend.hpp"
#include "globals.hpp"
#include "result_batcher.hpp"
#include "utils/debug_log.hpp"
#include "utils/yuyuko_memlife.hpp"
#include <cstdlib>

namespace ayafileio {

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
    m_close_wake.release();
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject* val;
    switch (req->type) {
        case ReqType::Read:  [[likely]]
            if (req->isReadinto) [[unlikely]] {
                val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
            } else if (req->preResult) {
                // 零拷贝快路径：数据已直接读进预建的 PyBytes
                val = req->preResult;
                req->preResult = nullptr;
                if (static_cast<size_t>(bytes) < req->reqSize) [[unlikely]] {
                    // 短读（文件在读期间被外部截短）：收缩到实际字节数
                    PyObject* exact = PyBytes_FromStringAndSize(
                        PyBytes_AS_STRING(val), static_cast<Py_ssize_t>(bytes));
                    Py_DECREF(val);
                    val = exact;
                }
            } else {
                val = PyBytes_FromStringAndSize(req->buf(), static_cast<Py_ssize_t>(bytes));
            }
            break;
        case ReqType::Write: [[likely]]
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

    // 只在达到批量阈值或这是本 loop 最后一个在飞 op（计数归 0，短期
    // 内不会再有新完成到达）时才 flush —— 不再为每个完成都付出一次
    // call_soon_threadsafe，与 IOCP 路径行为一致。
    bool last = req->batcher ? req->batcher->op_completed() : false;
    if (set_fn && val && req->batcher) {
        bool threshold = req->batcher->push(set_fn, val);
        if (threshold || last) [[unlikely]] req->batcher->flush();
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
    }
    TRACKED_DELETE(req);

    PyGILState_Release(gs);
}

void IOBackendBase::complete_error(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    m_close_wake.release();
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject* exc_class;
#ifdef _WIN32
    exc_class = map_win_error(static_cast<int>(err));
#else
    exc_class = map_posix_error(static_cast<int>(err));
#endif
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");

    PyObject* set_fn = req->set_exception; req->set_exception = nullptr;
    if (!set_fn) {
        // set_exception 未预取（罕见路径），此处持 GIL 按需获取
        set_fn = PyObject_GetAttr(req->future, g_str_set_exception);
        if (!set_fn) PyErr_Clear();
    }
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_result); req->set_result = nullptr;

    // 与 complete_ok 相同的批量 flush 策略
    bool last = req->batcher ? req->batcher->op_completed() : false;
    if (set_fn && exc && req->batcher) {
        bool threshold = req->batcher->push(set_fn, exc);
        if (threshold || last) [[unlikely]] req->batcher->flush();
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(exc);
    }
    TRACKED_DELETE(req);

    PyGILState_Release(gs);
}

// ════════════════════════════════════════════════════════════════════════════
// 请求构造 — 所有后端共享
// ════════════════════════════════════════════════════════════════════════════

IORequest* IOBackendBase::make_req(size_t size, PyObject* future, ReqType type) {
    auto* req = TRACKED_NEW(IORequest);
    req->file = this;
    req->batcher = m_batcher;
    if (m_batcher) m_batcher->op_submitted();
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取：错误是罕见路径，complete_error 持 GIL 时按需获取
    req->reqSize = size;
    req->type = type;

    if (size <= m_cached_buffer_size)
        req->poolBuf = pool_acquire_with_size(size);
    else
        req->heapBuf = static_cast<char*>(std::malloc(size));
    return req;
}

// 读专用：预建 PyBytes 作为读取目标缓冲，完成时零拷贝直接作为结果返回
IORequest* IOBackendBase::make_req_read_bytes(size_t size, PyObject* future) {
    PyObject* bytes = PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size));
    if (!bytes) return nullptr;  // MemoryError 已设置

    auto* req = TRACKED_NEW(IORequest);
    req->file = this;
    req->batcher = m_batcher;
    if (m_batcher) m_batcher->op_submitted();
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取，同 make_req
    req->reqSize = size;
    req->type = ReqType::Read;
    req->preResult = bytes;
    return req;
}

// 零拷贝写：持有调用方缓冲区视图，I/O 直接读用户内存
IORequest* IOBackendBase::make_req_held_write(Py_buffer* view, PyObject* future) {
    auto* req = TRACKED_NEW(IORequest);
    req->file = this;
    req->batcher = m_batcher;
    if (m_batcher) m_batcher->op_submitted();
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    req->reqSize = static_cast<size_t>(view->len);
    req->type = ReqType::Write;
    req->holdsWriteBuf = true;

    // 自行 GetBuffer 持有视图 —— 调用方在 write() 返回后会释放自己的
    // 那份 Py_buffer，我们必须持有独立的视图引用
    if (PyObject_GetBuffer(view->obj, &req->userBufView, PyBUF_SIMPLE) < 0) {
        if (m_batcher) m_batcher->op_completed();
        TRACKED_DELETE(req);
        return nullptr;
    }
    req->userBuf = view->obj;
    Py_INCREF(view->obj);
    return req;
}

IORequest* IOBackendBase::make_req_readinto(PyObject* buf, Py_buffer* view, size_t size, PyObject* future) {
    auto* req = TRACKED_NEW(IORequest);
    req->file = this;
    req->batcher = m_batcher;
    if (m_batcher) m_batcher->op_submitted();
    req->future = future;
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取，同 make_req
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
    m_close_wake.release();
    if (req->batcher) req->batcher->op_completed();
    PyObject* exc_class;
#ifdef _WIN32
    exc_class = map_win_error(static_cast<int>(err));
#else
    exc_class = map_posix_error(static_cast<int>(err));
#endif
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");
    PyObject* set_fn = req->set_exception; req->set_exception = nullptr;
    if (!set_fn && req->future) {
        // set_exception 未预取（罕见路径），此处按需获取（调用方持 GIL）
        set_fn = PyObject_GetAttr(req->future, g_str_set_exception);
        if (!set_fn) PyErr_Clear();
    }
    if (set_fn && exc) {
        PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
        Py_XDECREF(r);
    }
    Py_XDECREF(set_fn);
    Py_DECREF(exc);
    TRACKED_DELETE(req);
}

} // namespace ayafileio
