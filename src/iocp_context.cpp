#include "iocp_context.hpp"
#include "pool.hpp"
#include "config.hpp"
#include "utils/debug_log.hpp"
#include "utils/yuyuko_memlife.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// CONTAINING_RECORD — safe cast from OVERLAPPED* to IORequest*
// IORequest::ov is the first member, but we use the macro for robustness.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - (uintptr_t)(&((type *)0)->field)))
#endif

// ════════════════════════════════════════════════════════════════════════════
// Singleton
// ════════════════════════════════════════════════════════════════════════════

IOCPContext &IOCPContext::instance() {
    static IOCPContext ctx;
    return ctx;
}

IOCPContext::~IOCPContext() {
    if (m_running.load(std::memory_order_relaxed)) shutdown();
}

// mark the request sync done — clean up Python references but don't delete req.
// The IOCP worker will handle the final delete when the completion arrives.
// All Python references are released here so that the worker can delete req
// without needing the GIL.
void mark_sync_done(IORequest *req) {
    if (!Yuyuko::check_access(req, __FILE__, __LINE__, __FUNCTION__)) return;
    UR_DEBUG_LOG("mark_sync_done req=%pfuture=%p set_result=%p set_exception=%p isReadinto=%d",
                 (void*)req, (void*)req->future, (void*)req->set_result, (void*)req->set_exception, req->isReadinto);
    Py_XDECREF(req->set_result);
    Py_XDECREF(req->set_exception);
    Py_DECREF(req->future);
    req->set_result = nullptr;
    req->set_exception = nullptr;
    req->future = nullptr;
    if (req->isReadinto && req->userBufView.buf) {
        PyBuffer_Release(&req->userBufView);
        req->userBufView.buf = nullptr;
    }
    Py_XDECREF(req->userBuf);
    req->userBuf = nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
// Lifecycle: init / shutdown
// ════════════════════════════════════════════════════════════════════════════

void IOCPContext::init() {
    if (m_running.load(std::memory_order_acquire)) return;

    auto &cfg = ayafileio::config();
    unsigned n = cfg.io_worker_count();

    // ── Create dual IOCP handles ──────────────────────────────────────────
    m_readIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!m_readIocp) throw std::runtime_error("Failed to create read IOCP");

    m_writeIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!m_writeIocp) {
        CloseHandle(m_readIocp);
        m_readIocp = NULL;
        throw std::runtime_error("Failed to create write IOCP");
    }

    m_running.store(true, std::memory_order_release);

    // ── Start worker threads ──────────────────────────────────────────────
    m_readWorker  = std::thread(&IOCPContext::worker_proc, this, m_readIocp);
    m_writeWorker = std::thread(&IOCPContext::worker_proc, this, m_writeIocp);

    UR_DEBUG_LOG("IOCPContext::init — read IOCP=%p, write IOCP=%p, workers=%u",
                 m_readIocp, m_writeIocp, n);
}

void IOCPContext::shutdown() {
    if (!m_running.load(std::memory_order_acquire)) return;

    UR_DEBUG_LOG("shutdown: begin, sessions=%zu", m_sessions.size());

    // 1. Close all sessions first — while workers are still running.
    //    CancelIoEx generates cancellation completion packets that workers
    //    must process to free IORequest objects and their buffers.
    close_all_sessions();

    // 2. Signal workers to stop
    m_running.store(false, std::memory_order_release);

    // 3. Wake both IOCP workers so they exit their loop
    if (m_readIocp)  PostQueuedCompletionStatus(m_readIocp, 0, 0, NULL);
    if (m_writeIocp) PostQueuedCompletionStatus(m_writeIocp, 0, 0, NULL);

    // 4. Wait for worker threads to fully exit.
    {
        PyThreadState *_save = PyEval_SaveThread();
        if (m_readWorker.joinable())  m_readWorker.join();
        if (m_writeWorker.joinable()) m_writeWorker.join();
        PyEval_RestoreThread(_save);
    }

    // 5. Clean up IOCP handles
    if (m_readIocp)  { CloseHandle(m_readIocp);  m_readIocp  = NULL; }
    if (m_writeIocp) { CloseHandle(m_writeIocp); m_writeIocp = NULL; }

    UR_DEBUG_LOG("shutdown: IOCP handles closed, clearing batchers");

    // 6. Clean up batchers
    {
        std::lock_guard<std::mutex> lk(m_batchersMtx);
        m_batchers.clear();
    }

    // 7. Clean up any remaining sessions (should already be empty)
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        m_sessions.clear();
    }

    UR_DEBUG_LOG("IOCPContext::shutdown complete");
}

// ════════════════════════════════════════════════════════════════════════════
// Session management
// ════════════════════════════════════════════════════════════════════════════

uint64_t IOCPContext::create_session(HANDLE h, PyObject *loop,
                                     PyObject *create_future,
                                     bool appendMode,
                                     const PoolKey &poolKey, bool owns_fd) {
    auto &cfg = ayafileio::config();
    auto s = std::make_shared<Session>();
    s->id                     = m_nextSessionId.fetch_add(1, std::memory_order_relaxed);
    s->handle                 = h;
    s->loop                   = loop;
    s->create_future          = create_future;
    Py_INCREF(create_future);
    s->appendMode             = appendMode;
    s->poolKey                = poolKey;
    s->owns_fd                = owns_fd;
    s->cached_buffer_size     = cfg.buffer_size();
    s->cached_buffer_pool_max = cfg.buffer_pool_max();
    s->cached_close_timeout_ms = cfg.close_timeout_ms();

    // Associate handle with the appropriate IOCP using sessionId as key
    HANDLE iocp = (poolKey.access & GENERIC_WRITE) && !(poolKey.access & GENERIC_READ)
                      ? m_writeIocp : m_readIocp;  // write-only → write IOCP; all others → read IOCP
    if (!CreateIoCompletionPort(h, iocp, (ULONG_PTR)s->id, 0)) {
        DWORD err = GetLastError();
        UR_DEBUG_LOG("create_session: CreateIoCompletionPort FAILED id=%llu h=%p err=%lu", s->id, h, err);
        if (!poolKey.path.empty()) handle_pool_evict(poolKey);
        CloseHandle(h);
        Py_DECREF(create_future);
        win_throw_os_error(err, "Failed to associate handle with IOCP");
    }
    SetFileCompletionNotificationModes(h,
        FILE_SKIP_SET_EVENT_ON_HANDLE);

    // Cache initial file size — avoids GetFileSizeEx syscall on every read
    {
        LARGE_INTEGER fs{};
        if (GetFileSizeEx(h, &fs))
            s->cachedFileSize = static_cast<uint64_t>(fs.QuadPart);
    }

    s->running.store(true, std::memory_order_release);

    uint64_t id = s->id;
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        m_sessions[id] = std::move(s);
    }
    UR_DEBUG_LOG("create_session id=%llu h=%p iocp=%s owns_fd=%d",
                 id, h, (iocp == m_readIocp ? "read" : "write"), owns_fd);
    return id;
}

std::shared_ptr<Session> IOCPContext::get_session(uint64_t id) const {
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    auto it = m_sessions.find(id);
    return (it != m_sessions.end()) ? it->second : nullptr;
}

void IOCPContext::remove_session(uint64_t id) {
    UR_DEBUG_LOG("remove_session id=%llu", id);
    std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
    m_sessions.erase(id);
}

// ════════════════════════════════════════════════════════════════════════════
// Batcher management
// ════════════════════════════════════════════════════════════════════════════

ResultBatcher *IOCPContext::get_batcher(PyObject *loop) {
    {
        std::lock_guard<std::mutex> lk(m_batchersMtx);
        auto it = m_batchers.find(loop);
        if (it != m_batchers.end()) return it->second.get();
    }
    // Create new batcher outside lock to avoid nested issues
    auto &cfg = ayafileio::config();
    auto batcher = std::make_unique<ResultBatcher>(
        loop, cfg.iocp_batch_size(), 5);  // 5ms idle timeout
    std::lock_guard<std::mutex> lk(m_batchersMtx);
    auto [it, inserted] = m_batchers.emplace(loop, std::move(batcher));
    return it->second.get();
}

DWORD IOCPContext::get_min_batcher_timeout_ms() {
    std::lock_guard<std::mutex> lk(m_batchersMtx);
    DWORD min_timeout = INFINITE;
    for (auto &kv : m_batchers) {
        DWORD t = kv.second->get_timeout_ms();
        if (t < min_timeout) min_timeout = t;
    }
    return min_timeout;
}

// ════════════════════════════════════════════════════════════════════════════
// Worker thread
// ════════════════════════════════════════════════════════════════════════════

void IOCPContext::worker_proc(HANDLE iocp) {
    auto &cfg = ayafileio::config();
    unsigned batch_cap = cfg.iocp_batch_size();
    if (batch_cap < 1) batch_cap = 1;
    if (batch_cap > 256) batch_cap = 256;
    std::vector<OVERLAPPED_ENTRY> batch(batch_cap);

    UR_DEBUG_LOG("worker_proc: start iocp=%p batch_cap=%u", iocp, batch_cap);

    while (m_running.load(std::memory_order_acquire)) {
        DWORD timeout = get_min_batcher_timeout_ms();
        ULONG count = 0;

        BOOL ok = GetQueuedCompletionStatusEx(
            iocp, batch.data(), (ULONG)batch.size(), &count, timeout, FALSE);

        if (count == 0) {
            // Timeout or error — flush idle batchers, then check for shutdown
            {
                PyGILState_STATE gs = PyGILState_Ensure();
                flush_batchers();
                PyGILState_Release(gs);
            }
            if (!ok && !m_running.load(std::memory_order_acquire)) break;
            continue;
        }

        UR_DEBUG_LOG("worker_proc: batch count=%lu iocp=%p", count, iocp);

        // Process all completions with a single GIL acquire
        PyGILState_STATE gs = PyGILState_Ensure();
        for (ULONG i = 0; i < count; ++i) {
            auto &e = batch[i];
            if (!e.lpOverlapped) {
                // Shutdown wake-up
                continue;
            }
            if (e.lpCompletionKey == 0) continue;

            auto *req = CONTAINING_RECORD(e.lpOverlapped, IORequest, ov);
            uint64_t sid = (uint64_t)e.lpCompletionKey;
            process_one(sid, req, e.dwNumberOfBytesTransferred,
                        (e.Internal == 0) ? 0 : (DWORD)e.Internal);
        }
        flush_batchers();
        PyGILState_Release(gs);
    }
    UR_DEBUG_LOG("worker_proc: exit iocp=%p", iocp);
}

void IOCPContext::process_one(uint64_t sessionId, IORequest *req,
                              DWORD bytes, DWORD err) {
    if (!Yuyuko::check_access(req, __FILE__, __LINE__, __FUNCTION__)) return;
    // ── Double‑delivery guard ────────────────────────────────────────────
    // Under extreme concurrency Windows IOCP may deliver the same OVERLAPPED
    // completion twice.  Atomically claim this request; if it was already
    // processed (by a previous process_one or mark_sync_done) just return.
    IOState expected = IOState::PENDING;
    if (!req->state.compare_exchange_strong(expected, IOState::RESOLVED)) {
        UR_DEBUG_LOG("process_one DOUBLE-DELIVERY req=%p sid=%llu state=%d — skipping",
                     (void*)req, sessionId, (int)expected);
        return;
    }

    UR_DEBUG_LOG("process_one ENTER sid=%llu req=%p future=%p set_res=%p set_exc=%p bytes=%lu err=%lu type=%d isReadinto=%d",
                 sessionId, (void*)req, (void*)req->future, (void*)req->set_result,
                 (void*)req->set_exception, (unsigned long)bytes, (unsigned long)err,
                 (int)req->type, (int)req->isReadinto);

    // Sync completion — already resolved inline by the submit path.
    // Still need to decrement pending so that close() waits for this
    // completion to be dequeued before releasing the handle.
    if (!req->future) {
        UR_DEBUG_LOG("process_one SYNC req=%p sid=%llu — looking up session", (void*)req, sessionId);
        std::shared_ptr<Session> session;
        {
            std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
            auto it = m_sessions.find(sessionId);
            if (it != m_sessions.end())
                session = it->second;
        }
        if (session) {
            long prev = session->pending.fetch_sub(1, std::memory_order_release);
            UR_DEBUG_LOG("process_one SYNC req=%p sid=%llu pending %ld→%ld — deleting",
                         (void*)req, sessionId, prev, prev - 1);
        } else {
            UR_DEBUG_LOG("process_one SYNC req=%p sid=%llu SESSION NOT FOUND — deleting req anyway",
                         (void*)req, sessionId);
        }
        TRACKED_DELETE(req);
        return;
    }

    // Look up session; if gone (closed while I/O was in flight), discard.
    std::shared_ptr<Session> session;
    {
        std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) {
            // Session already closed — just release memory.
            UR_DEBUG_LOG("process_one ASYNC req=%p sid=%llu SESSION GONE — deleting req", (void*)req, sessionId);
            TRACKED_DELETE(req);
            return;
        }
        session = it->second;
    }

    long prev_pending = session->pending.fetch_sub(1, std::memory_order_release);
    UR_DEBUG_LOG("process_one ASYNC req=%p sid=%llu pending %ld→%ld err=%lu",
                 (void*)req, sessionId, prev_pending, prev_pending - 1, (unsigned long)err);

    ResultBatcher *batcher = get_batcher(session->loop);

    PyObject *set_fn  = nullptr;
    PyObject *val     = nullptr;

    if (err == 0) {
        // ── success ───────────────────────────────────────────────────────
        set_fn = req->set_result;
        req->set_result = nullptr;

        switch (req->type) {
        case ReqType::Read:
            if (req->isReadinto)
                val = PyLong_FromSsize_t((Py_ssize_t)bytes);
            else
                val = PyBytes_FromStringAndSize(req->buf(), (Py_ssize_t)bytes);
            break;
        case ReqType::Write:
            val = PyLong_FromSsize_t((Py_ssize_t)bytes);
            break;
        default:
            val = Py_None;
            Py_INCREF(val);
            break;
        }
    } else {
        // ── error ─────────────────────────────────────────────────────────
        set_fn = req->set_exception;
        req->set_exception = nullptr;

        PyObject *exc_class = map_win_error(err);
        val = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    }

    if (set_fn && val) {
        bool threshold = batcher->push(set_fn, val);
        if (threshold) [[unlikely]] batcher->flush();
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
    }
    UR_DEBUG_LOG("process_one ASYNC req=%p sid=%llu — deleting after push", (void*)req, sessionId);
    TRACKED_DELETE(req);
}

void IOCPContext::flush_batchers() {
    // Called with GIL held from worker thread
    std::lock_guard<std::mutex> lk(m_batchersMtx);
    for (auto &kv : m_batchers) {
        auto &b = kv.second;
        if (b->has_pending() && b->idle_expired()) {
            b->flush();
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// IORequest factory (replaces IOBackendBase::make_req for the IOCP path)
// ════════════════════════════════════════════════════════════════════════════

namespace {

IORequest *make_req_iocp(size_t size, PyObject *future, ReqType type,
                         size_t buf_size, size_t /*pool_max*/) {
    auto *req = TRACKED_NEW(IORequest);
    req->file        = nullptr;   // not used — completion routed via sessionId
    req->batcher = nullptr;   // not used — batcher handles dispatch
    req->future      = future;
    Py_INCREF(future);
    req->set_result    = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize = size;
    req->type    = type;

    if (size <= buf_size)
        req->poolBuf = pool_acquire_with_size(size);
    else
        req->heapBuf = static_cast<char *>(std::malloc(size));

    UR_DEBUG_LOG("make_req_iocp req=%p future=%p type=%d size=%zu", (void*)req, (void*)future, (int)type, size);
    return req;
}

IORequest *make_req_readinto_iocp(PyObject *buf, Py_buffer *view, size_t size,
                                  PyObject *future) {
    auto *req = TRACKED_NEW(IORequest);
    req->file        = nullptr;
    req->batcher = nullptr;
    req->future      = future;
    Py_INCREF(future);
    req->set_result    = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize   = size;
    req->type      = ReqType::Read;
    req->isReadinto = true;
    req->userBuf   = buf;
    Py_INCREF(buf);
    req->userBufView = *view;

    UR_DEBUG_LOG("make_req_readinto_iocp req=%p future=%p size=%zu", (void*)req, (void*)future, size);
    return req;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════════════
// I/O submission helpers
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::check_session_closed(const std::shared_ptr<Session> &s) {
    if (!s->running.load(std::memory_order_relaxed) ||
        s->handle == INVALID_HANDLE_VALUE) {
        PyObject *future = PyObject_CallNoArgs(s->create_future);
        if (future) {
            PyObject *exc = PyObject_CallFunction(
                g_ValueError, "s", "I/O operation on closed file.");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        }
        return future;
    }
    return nullptr;
}

HANDLE IOCPContext::iocp_for_session(const std::shared_ptr<Session> &s) {
    // Only write-only sessions use write IOCP; everything else uses read IOCP
    return (s->poolKey.access & GENERIC_WRITE) &&
                   !(s->poolKey.access & GENERIC_READ)
               ? m_writeIocp
               : m_readIocp;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_read
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_read(uint64_t session_id, int64_t size) {
    auto s = get_session(session_id);
    if (!s) {
        // Session gone — return an already-failed future (best effort)
        PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) return nullptr;
        PyObject *cf = PyObject_GetAttr(loop, g_str_create_future);
        Py_DECREF(loop);
        PyObject *future = PyObject_CallNoArgs(cf);
        Py_DECREF(cf);
        if (future) {
            PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "File not open.");
            PyObject *fn = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        }
        return future;
    }

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    if (is_ctrlc_triggered() ||
        !s->running.load(std::memory_order_relaxed)) {
        PyObject *exc = PyObject_CallFunction(g_KeyboardInterrupt, "s", "interrupted");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    uint64_t offset;
    size_t   readSize;
    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        int64_t rem = static_cast<int64_t>(s->cachedFileSize) - static_cast<int64_t>(s->filePos);
        if (rem <= 0) {
            PyObject *b = PyBytes_FromStringAndSize(nullptr, 0);
            PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
            PyObject *r  = PyObject_CallFunctionObjArgs(fn, b, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(b);
            return future;
        }
        readSize = (size < 0 || (size_t)size > (size_t)rem)
                       ? (size_t)rem : (size_t)size;
        if (readSize == 0) {
            PyObject *b = PyBytes_FromStringAndSize(nullptr, 0);
            PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
            PyObject *r  = PyObject_CallFunctionObjArgs(fn, b, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(b);
            return future;
        }
        offset = s->filePos;
        s->filePos += readSize;
    }

    IORequest *req = make_req_iocp(readSize, future, ReqType::Read,
                                   s->cached_buffer_size, s->cached_buffer_pool_max);
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    DWORD got = 0;
    BOOL ok = ReadFile(s->handle, req->buf(), (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline, delete by IOCP worker.
        // Do NOT decrement pending here; the worker handles it so that
        // close() sees pending > 0 until all completions are drained.
        UR_DEBUG_LOG("submit_read SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        PyObject *val = PyBytes_FromStringAndSize(req->buf(), got);
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(val);
        mark_sync_done(req);
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->pending.fetch_sub(1, std::memory_order_relaxed);
            UR_DEBUG_LOG("submit_read FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "ReadFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            TRACKED_DELETE(req);
        } else {
            UR_DEBUG_LOG("submit_read ASYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        }
    }
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_write
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_write(uint64_t session_id, Py_buffer *view) {
    auto s = get_session(session_id);
    if (!s) {
        PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) return nullptr;
        PyObject *cf = PyObject_GetAttr(loop, g_str_create_future);
        Py_DECREF(loop);
        PyObject *future = PyObject_CallNoArgs(cf);
        Py_DECREF(cf);
        if (future) {
            PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "File not open.");
            PyObject *fn = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        }
        return future;
    }

    PyObject *future = check_session_closed(s);
    if (future) return future;

    size_t wsize = (size_t)view->len;
    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    if (is_ctrlc_triggered() ||
        !s->running.load(std::memory_order_relaxed)) {
        PyObject *exc = PyObject_CallFunction(g_KeyboardInterrupt, "s", "interrupted");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }
    if (wsize == 0) {
        PyObject *z = PyLong_FromLong(0);
        PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r  = PyObject_CallFunctionObjArgs(fn, z, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(z);
        return future;
    }

    uint64_t offset;
    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        if (s->appendMode) {
            offset = s->cachedFileSize;
        } else {
            offset = s->filePos;
        }
        s->filePos = offset + wsize;
        if (s->filePos > s->cachedFileSize)
            s->cachedFileSize = s->filePos;  // optimistic: assume write succeeds
    }

    IORequest *req = make_req_iocp(wsize, future, ReqType::Write,
                                   s->cached_buffer_size, s->cached_buffer_pool_max);
    memcpy(req->buf(), view->buf, wsize);
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    DWORD wrote = 0;
    BOOL ok = WriteFile(s->handle, req->buf(), (DWORD)wsize, &wrote, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline, delete by IOCP worker.
        // Do NOT decrement pending here; the worker handles it.
        UR_DEBUG_LOG("submit_write SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        PyObject *val = PyLong_FromSsize_t((Py_ssize_t)wrote);
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(val);
        mark_sync_done(req);
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->pending.fetch_sub(1, std::memory_order_relaxed);
            UR_DEBUG_LOG("submit_write FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "WriteFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            TRACKED_DELETE(req);
        } else {
            UR_DEBUG_LOG("submit_write ASYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        }
    }
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_seek
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_seek(uint64_t session_id, int64_t offset, int whence) {
    auto s = get_session(session_id);
    if (!s) return nullptr;

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        if (whence == 0)
            s->filePos = (uint64_t)offset;
        else if (whence == 1)
            s->filePos = (uint64_t)((int64_t)s->filePos + offset);
        else if (whence == 2) {
            s->filePos = static_cast<uint64_t>(static_cast<int64_t>(s->cachedFileSize) + offset);
        } else {
            PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "Invalid whence value");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            return future;
        }
    }
    PyObject *pos = PyLong_FromUnsignedLongLong(s->filePos);
    PyObject *fn  = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r   = PyObject_CallFunctionObjArgs(fn, pos, nullptr);
    Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(pos);
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_flush
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_flush(uint64_t session_id) {
    auto s = get_session(session_id);
    if (!s) return nullptr;

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    if (!FlushFileBuffers(s->handle)) {
        DWORD err = GetLastError();
        PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "FlushFileBuffers failed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }
    PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, Py_None, nullptr);
    Py_XDECREF(r); Py_DECREF(fn);
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_tell
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_tell(uint64_t session_id) {
    auto s = get_session(session_id);
    if (!s) return nullptr;

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    uint64_t pos;
    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        pos = s->filePos;
    }
    PyObject *py_pos = PyLong_FromUnsignedLongLong(pos);
    PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, py_pos, nullptr);
    Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(py_pos);
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_truncate
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_truncate(uint64_t session_id, int64_t size) {
    auto s = get_session(session_id);
    if (!s) return nullptr;

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    if (size < 0) {
        PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "negative size not allowed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    LARGE_INTEGER prev_pos;
    if (!SetFilePointerEx(s->handle, {0}, &prev_pos, FILE_CURRENT)) {
        DWORD err = GetLastError();
        PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "SetFilePointerEx failed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    LARGE_INTEGER li;
    li.QuadPart = size;
    if (!SetFilePointerEx(s->handle, li, NULL, FILE_BEGIN)) {
        DWORD err = GetLastError();
        SetFilePointerEx(s->handle, prev_pos, NULL, FILE_BEGIN);
        PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "SetFilePointerEx failed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    if (!SetEndOfFile(s->handle)) {
        DWORD err = GetLastError();
        SetFilePointerEx(s->handle, prev_pos, NULL, FILE_BEGIN);
        PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "SetEndOfFile failed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    SetFilePointerEx(s->handle, prev_pos, NULL, FILE_BEGIN);

    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        s->cachedFileSize = static_cast<uint64_t>(size);
        if (static_cast<uint64_t>(size) < s->filePos) s->filePos = static_cast<uint64_t>(size);
    }

    PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, Py_None, nullptr);
    Py_XDECREF(r); Py_DECREF(fn);
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_readinto
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_readinto(uint64_t session_id, PyObject *buf) {
    auto s = get_session(session_id);
    if (!s) return nullptr;

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    Py_buffer view;
    if (PyObject_GetBuffer(buf, &view, PyBUF_WRITABLE) < 0) {
        PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "readinto() requires a writable buffer");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    if (view.len == 0) {
        PyBuffer_Release(&view);
        PyObject *z = PyLong_FromLong(0);
        PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r  = PyObject_CallFunctionObjArgs(fn, z, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(z);
        return future;
    }

    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        int64_t rem = static_cast<int64_t>(s->cachedFileSize) - static_cast<int64_t>(s->filePos);
        if (rem <= 0) {
            PyBuffer_Release(&view);
            PyObject *z = PyLong_FromLong(0);
            PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
            PyObject *r  = PyObject_CallFunctionObjArgs(fn, z, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(z);
            return future;
        }
        readSize = std::min((size_t)view.len, (size_t)rem);
        offset = s->filePos;
        s->filePos += readSize;
    }

    IORequest *req = make_req_readinto_iocp(buf, &view, readSize, future);
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    DWORD got = 0;
    BOOL ok = ReadFile(s->handle, view.buf, (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline, delete by IOCP worker.
        // Do NOT decrement pending here; the worker handles it.
        UR_DEBUG_LOG("submit_readinto SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        PyObject *val = PyLong_FromSsize_t((Py_ssize_t)got);
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(val);
        mark_sync_done(req);
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->pending.fetch_sub(1, std::memory_order_relaxed);
            UR_DEBUG_LOG("submit_readinto FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "ReadFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            TRACKED_DELETE(req);
        } else {
            UR_DEBUG_LOG("submit_readinto ASYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        }
    }
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_close
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_close(uint64_t session_id) {
    auto s = get_session(session_id);
    if (!s) {
        UR_DEBUG_LOG("submit_close sid=%llu — session already gone", session_id);
        // Already gone — return resolved future
        PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) return nullptr;
        PyObject *cf = PyObject_GetAttr(loop, g_str_create_future);
        Py_DECREF(loop);
        PyObject *future = PyObject_CallNoArgs(cf);
        Py_DECREF(cf);
        if (future) {
            PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
            PyObject *r = PyObject_CallFunctionObjArgs(fn, Py_None, nullptr);
            Py_XDECREF(r); Py_DECREF(fn);
        }
        return future;
    }

    PyObject *future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    // Prevent new I/O
    bool expected = true;
    if (!s->running.compare_exchange_strong(expected, false)) {
        UR_DEBUG_LOG("submit_close sid=%llu — already closing", session_id);
        // Already closing — return resolved
        PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
        PyObject *r  = PyObject_CallFunctionObjArgs(fn, Py_None, nullptr);
        Py_XDECREF(r); Py_DECREF(fn);
        return future;
    }

    long close_pending = s->pending.load(std::memory_order_acquire);
    UR_DEBUG_LOG("submit_close sid=%llu h=%p pending=%ld owns_fd=%d poolPath=%s",
                 session_id, s->handle, close_pending, s->owns_fd,
                 s->poolKey.path.empty() ? "<none>" : s->poolKey.path.c_str());

    if (s->handle != INVALID_HANDLE_VALUE) {
        if (close_pending > 0) {
            UR_DEBUG_LOG("submit_close sid=%llu — CancelIoEx + wait begin (timeout=%ums)",
                         session_id, s->cached_close_timeout_ms);
            CancelIoEx(s->handle, NULL);

            unsigned timeout_ms = s->cached_close_timeout_ms;
            int w = 1;
            int elapsed = 0;
            while (elapsed < (int)timeout_ms &&
                   s->pending.load(std::memory_order_acquire) > 0) {
                Py_BEGIN_ALLOW_THREADS
                Sleep(w);
                Py_END_ALLOW_THREADS
                elapsed += w;
                w = std::min(w * 2, 32);
            }
            long final_pending = s->pending.load(std::memory_order_acquire);
            UR_DEBUG_LOG("submit_close sid=%llu — wait done elapsed=%dms final_pending=%ld",
                         session_id, elapsed, final_pending);
        }

        if (s->owns_fd) {
            // IOCP handles must NOT be pooled — the kernel IOCP association
            // outlives the handle's pool lifetime and causes
            // ERROR_INVALID_PARAMETER (err=87) when CreateIoCompletionPort
            // is called again on the recycled handle.
            if (!s->poolKey.path.empty()) {
                handle_pool_evict(s->poolKey);
            }
            UR_DEBUG_LOG("submit_close sid=%llu — CloseHandle h=%p", session_id, s->handle);
            CloseHandle(s->handle);
        }
        s->handle = INVALID_HANDLE_VALUE;
    }

    // Remove from map — in-flight completions will see "not found" and
    // clean up gracefully. Any worker holding a shared_ptr keeps the
    // Session alive until it finishes.
    remove_session(session_id);

    UR_DEBUG_LOG("submit_close sid=%llu — complete", session_id);

    PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, Py_None, nullptr);
    Py_XDECREF(r); Py_DECREF(fn);
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// close_all_sessions — emergency shutdown (Ctrl+C)
// ════════════════════════════════════════════════════════════════════════════

void IOCPContext::close_all_sessions() {
    std::vector<std::shared_ptr<Session>> snap;
    {
        std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
        snap.reserve(m_sessions.size());
        for (auto &kv : m_sessions) snap.push_back(kv.second);
    }

    UR_DEBUG_LOG("close_all_sessions: %zu sessions", snap.size());

    for (auto &s : snap) {
        bool expected = true;
        if (!s->running.compare_exchange_strong(expected, false)) continue;

        long close_pending = s->pending.load(std::memory_order_acquire);
        UR_DEBUG_LOG("close_all_sessions sid=%llu h=%p pending=%ld", s->id, s->handle, close_pending);

        if (s->handle != INVALID_HANDLE_VALUE) {
            if (close_pending > 0) {
                CancelIoEx(s->handle, NULL);
                // Release GIL if held so IOCP worker can process cancellations.
                // close_all_sessions may be called from Ctrl+C handler (no GIL).
                int w = 1;
                int elapsed = 0;
                while (elapsed < 500 &&
                       s->pending.load(std::memory_order_acquire) > 0) {
                    if (PyGILState_Check()) {
                        PyThreadState *_save = PyEval_SaveThread();
                        Sleep(w);
                        PyEval_RestoreThread(_save);
                    } else {
                        Sleep(w);
                    }
                    elapsed += w;
                    w = std::min(w * 2, 32);
                }
                UR_DEBUG_LOG("close_all_sessions sid=%llu wait done elapsed=%d final_pending=%ld",
                             s->id, elapsed, s->pending.load(std::memory_order_acquire));
            }
            if (s->owns_fd) {
                if (!s->poolKey.path.empty()) {
                    handle_pool_evict(s->poolKey);
                }
                CloseHandle(s->handle);
            }
            s->handle = INVALID_HANDLE_VALUE;
        }
    }

    std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
    m_sessions.clear();
}
