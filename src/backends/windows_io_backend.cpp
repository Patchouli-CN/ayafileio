#define NOMINMAX
#include "windows_io_backend.hpp"
#include <algorithm>

// cached event loop
static PyObject   *g_cachedLoop       = nullptr;
static PyObject   *g_cachedFutureFn   = nullptr;
static LoopHandle *g_cachedLoopHandle = nullptr;

static void refresh_loop_cache(PyObject *loop) {
    if (loop == g_cachedLoop) return;
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop       = loop;
    g_cachedFutureFn   = PyObject_GetAttr(loop, g_str_create_future);
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
}

WindowsIOBackend::WindowsIOBackend(const std::string &path, const std::string &mode) {
    static bool ctrl_reg = false;
    if (!ctrl_reg) { SetConsoleCtrlHandler(ctrl_handler, TRUE); ctrl_reg = true; }

    PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    refresh_loop_cache(loop);
    m_loop          = loop;
    m_create_future = g_cachedFutureFn; Py_INCREF(m_create_future);
    m_loop_handle   = g_cachedLoopHandle;

    { std::lock_guard<std::mutex> lk(g_openFilesMtx); g_openFiles.insert(this); }

    DWORD access = 0, disp = OPEN_EXISTING;
    bool canRead=false, canWrite=false, appendMode=false;
    bool plus=mode.find('+')!=std::string::npos;
    bool hasR=mode.find('r')!=std::string::npos, hasW=mode.find('w')!=std::string::npos;
    bool hasA=mode.find('a')!=std::string::npos, hasX=mode.find('x')!=std::string::npos;

    if (hasX&&hasW) throw py::value_error("Invalid mode: cannot combine x and w");
    if (!hasR&&!hasW&&!hasA&&!hasX) throw py::value_error("Invalid mode: missing mode character");
    if (hasR) { canRead=true;  disp=OPEN_EXISTING; }
    if (hasW) { canWrite=true; disp=CREATE_ALWAYS; }
    if (hasA) { canWrite=true; appendMode=true; disp=OPEN_ALWAYS; }
    if (hasX) { canWrite=true; disp=CREATE_NEW; }
    if (plus) { canRead=true; canWrite=true; }
    if (canRead)  access |= GENERIC_READ;
    if (canWrite) access |= GENERIC_WRITE;

    m_poolKey    = make_pool_key(path, access, disp);
    m_appendMode = appendMode;

    bool canReuse = (disp == OPEN_EXISTING || disp == OPEN_ALWAYS);
    if (canReuse) m_handle = handle_pool_acquire(m_poolKey);

    if (m_handle == INVALID_HANDLE_VALUE) {
        m_handle = CreateFileA(path.c_str(), access,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            NULL, disp, FILE_FLAG_OVERLAPPED, NULL);
        if (m_handle == INVALID_HANDLE_VALUE)
            throw_os_error(GetLastError(), "Failed to open file", path.c_str());
        if (!CreateIoCompletionPort(m_handle, g_iocp, (ULONG_PTR)this, 0)) {
            CloseHandle(m_handle);
            throw_os_error(GetLastError(), "Failed to associate file to IOCP");
        }
        // Suppress IOCP packets when I/O completes synchronously (data in cache).
        // Allows inline future resolution without cross-thread wakeup.
        SetFileCompletionNotificationModes(m_handle,
            FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    } else {
        // Re-associate pooled handle with this FileHandle as completion key.
        if (!CreateIoCompletionPort(m_handle, g_iocp, (ULONG_PTR)this, 0)) {
            CloseHandle(m_handle);
            m_handle = CreateFileA(path.c_str(), access,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                NULL, disp, FILE_FLAG_OVERLAPPED, NULL);
            if (m_handle == INVALID_HANDLE_VALUE)
                throw_os_error(GetLastError(), "Failed to open file", path.c_str());
            if (!CreateIoCompletionPort(m_handle, g_iocp, (ULONG_PTR)this, 0)) {
                CloseHandle(m_handle);
                throw_os_error(GetLastError(), "Failed to associate file to IOCP");
            }
            SetFileCompletionNotificationModes(m_handle,
                FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
        }
        // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS persists across pool recycles.
    }

    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    if (m_appendMode) {
        LARGE_INTEGER li{}; if (GetFileSizeEx(m_handle,&li)) m_filePos=(uint64_t)li.QuadPart;
    }
}

WindowsIOBackend::~WindowsIOBackend() {
    close_impl();
    { std::lock_guard<std::mutex> lk(g_openFilesMtx); g_openFiles.erase(this); }
    Py_XDECREF(m_create_future);
    Py_XDECREF(m_loop);
}

PyObject *WindowsIOBackend::read(int64_t size) {
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    if (g_ctrlcTriggered.load(std::memory_order_relaxed) ||
        !m_running.load(std::memory_order_relaxed)) {
        resolve_exc(future, g_KeyboardInterrupt, 0, "interrupted");
        return future;
    }

    uint64_t offset; size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        LARGE_INTEGER fs{};
        if (!GetFileSizeEx(m_handle, &fs)) {
            resolve_exc(future, g_OSError, GetLastError(), "GetFileSizeEx failed");
            return future;
        }
        int64_t rem = (int64_t)fs.QuadPart - (int64_t)m_filePos;
        if (rem <= 0) { resolve_bytes(future, nullptr, 0); return future; }
        readSize = (size<0||size>rem) ? (size_t)rem : (size_t)size;
        if (readSize == 0) { resolve_bytes(future, nullptr, 0); return future; }
        offset = m_filePos;
        m_filePos += readSize;
    }

    IORequest *req = make_req(readSize, future, ReqType::Read);
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    DWORD got = 0;
    BOOL ok = ReadFile(m_handle, req->buf(), (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion: resolve inline, no IOCP round-trip.
        m_pending.fetch_sub(1, std::memory_order_relaxed);
        PyObject *val = PyBytes_FromStringAndSize(req->buf(), got);
        resolve_ok(future, val); Py_DECREF(val);
        delete req;
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            m_pending.fetch_sub(1, std::memory_order_relaxed);
            complete_error_inline(req, err);
        }
    }
    return future;
}

PyObject *WindowsIOBackend::write(Py_buffer *view) {
    size_t size = (size_t)view->len;
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    if (g_ctrlcTriggered.load(std::memory_order_relaxed) ||
        !m_running.load(std::memory_order_relaxed)) {
        resolve_exc(future, g_KeyboardInterrupt, 0, "interrupted");
        return future;
    }
    if (size == 0) {
        PyObject *z = PyLong_FromLong(0);
        resolve_ok(future, z); Py_DECREF(z);
        return future;
    }

    uint64_t offset;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (m_appendMode) {
            LARGE_INTEGER li{};
            if (!GetFileSizeEx(m_handle, &li)) {
                resolve_exc(future, g_OSError, GetLastError(), "GetFileSizeEx failed");
                return future;
            }
            offset = (uint64_t)li.QuadPart;
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
    }

    IORequest *req = make_req(size, future, ReqType::Write);
    memcpy(req->buf(), view->buf, size);
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    DWORD wrote = 0;
    BOOL ok = WriteFile(m_handle, req->buf(), (DWORD)size, &wrote, &req->ov);
    if (ok) {
        // Synchronous completion: resolve inline.
        m_pending.fetch_sub(1, std::memory_order_relaxed);
        PyObject *val = PyLong_FromSsize_t((Py_ssize_t)wrote);
        resolve_ok(future, val); Py_DECREF(val);
        delete req;
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            m_pending.fetch_sub(1, std::memory_order_relaxed);
            complete_error_inline(req, err);
        }
    }
    return future;
}

PyObject *WindowsIOBackend::seek(int64_t offset, int whence) {
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if      (whence == 0) m_filePos = (uint64_t)offset;
        else if (whence == 1) m_filePos = (uint64_t)((int64_t)m_filePos + offset);
        else if (whence == 2) {
            LARGE_INTEGER sz{};
            if (!GetFileSizeEx(m_handle, &sz)) {
                resolve_exc(future, g_OSError, GetLastError(), "GetFileSizeEx failed");
                return future;
            }
            m_filePos = (uint64_t)((int64_t)sz.QuadPart + offset);
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }
    }
    PyObject *pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject *WindowsIOBackend::flush() {
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    if (!m_running.load(std::memory_order_relaxed) || m_handle==INVALID_HANDLE_VALUE) {
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    if (!FlushFileBuffers(m_handle)) {
        resolve_exc(future, g_OSError, GetLastError(), "FlushFileBuffers failed");
        return future;
    }
    resolve_ok(future, Py_None);
    return future;
}

PyObject *WindowsIOBackend::close() {
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    close_impl();
    resolve_ok(future, Py_None);
    return future;
}

void WindowsIOBackend::close_impl() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) return;
    if (m_handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_handle, NULL);
        int w = 1;
        for (int i=0; i<12 && m_pending.load(std::memory_order_acquire)>0; ++i) {
            Sleep(w); w = std::min(w*2, 32); 
        }
        LARGE_INTEGER zero{};
        SetFilePointerEx(m_handle, zero, nullptr, FILE_BEGIN);
        handle_pool_release(m_poolKey, m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    { std::lock_guard<std::mutex> lk(g_openFilesMtx); g_openFiles.erase(this); }
}

void WindowsIOBackend::complete_ok(IORequest *req, size_t bytes) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *val = nullptr;
    if      (req->type == ReqType::Read)  val = PyBytes_FromStringAndSize(req->buf(), bytes);
    else if (req->type == ReqType::Write) val = PyLong_FromSsize_t((Py_ssize_t)bytes);
    else                                  { val = Py_None; Py_INCREF(Py_None); }

    PyObject *set_fn = req->set_result; req->set_result = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_exception); req->set_exception = nullptr;

    req->loop_handle->push(set_fn, val);
    delete req;

    PyGILState_Release(gs);
}

void WindowsIOBackend::complete_error(IORequest *req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *exc_class = map_win_error(err);
    PyObject *exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");

    PyObject *set_fn = req->set_exception; req->set_exception = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_result); req->set_result = nullptr;

    req->loop_handle->push(set_fn, exc);
    delete req;

    PyGILState_Release(gs);
}

IORequest *WindowsIOBackend::make_req(size_t size, PyObject *future, ReqType type) {
    auto *req          = new IORequest();
    req->file          = this;
    req->loop_handle   = m_loop_handle;
    req->future        = future; Py_INCREF(future);
    req->set_result    = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize       = size;
    req->type          = type;
    if (size <= POOL_BUF_SIZE) req->poolBuf = pool_acquire();
    else                       req->heapBuf = new char[size];
    return req;
}

void WindowsIOBackend::complete_error_inline(IORequest *req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject *exc_class = map_win_error(err);
    PyObject *exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    PyObject *set_fn = req->set_exception; req->set_exception = nullptr;
    PyObject *r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
    Py_XDECREF(r); Py_DECREF(set_fn); Py_DECREF(exc);
    delete req;
}


