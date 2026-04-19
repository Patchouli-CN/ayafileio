#include "thread_io_backend.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <algorithm>
#include <pthread.h>
#include "../globals.hpp"
#include "./utils/file_mode.hpp"
#include <thread>

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

ThreadIOBackend::ThreadIOBackend(const std::string &path, const std::string &mode) {
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    // 使用缓存的配置
    unsigned num_workers = cfg.io_worker_count();

    // ⚠️ 延迟获取事件循环，不在构造函数中获取！

    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        throw py::value_error(e.what());
    }
    bool appendMode = mi.appendMode;

    if (mi.hasW) { flags = O_WRONLY | O_CREAT | O_TRUNC; }
    if (mi.hasA) { flags = O_WRONLY | O_CREAT | O_APPEND; }
    if (mi.hasX) { flags = O_WRONLY | O_CREAT | O_EXCL; }
    if (mi.plus) { flags = O_RDWR; }

    m_appendMode = appendMode;

    m_fd = open(path.c_str(), flags, 0644);
    if (m_fd == -1) {
        throw py::python_error();
    }

    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = st.st_size;
        }
    }


    if (num_workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 1;
        num_workers = std::max(1u, std::min(hc * 2u, 16u));
    } else if (!(num_workers >= 1 && num_workers <= 128)) {
        throw py::value_error("worker count must be 0 (auto) or 1-128");
    }
    
    for (unsigned i = 0; i < num_workers; ++i) {
        m_workers.emplace_back(&ThreadIOBackend::worker_thread, this);
    }
}

ThreadIOBackend::~ThreadIOBackend() {
    close_impl();
    if (m_loop_initialized) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
}

void ThreadIOBackend::ensure_loop_initialized() {
    if (m_loop_initialized) return;
    std::lock_guard<std::mutex> lk(m_loop_init_mtx);
    if (m_loop_initialized) return;  // 双重检查
    
    PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    refresh_loop_cache(loop);
    m_loop = loop;
    m_create_future = g_cachedFutureFn;
    Py_INCREF(m_create_future);
    m_loop_handle = g_cachedLoopHandle;
    
    m_loop_initialized = true;
}

void ThreadIOBackend::worker_thread() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_queueMtx);
            m_cv.wait(lk, [this] { return m_stop || !m_taskQueue.empty(); });
            if (m_stop && m_taskQueue.empty()) return;
            task = std::move(m_taskQueue.front());
            m_taskQueue.pop();
        }
        task();
    }
}

void ThreadIOBackend::enqueue_task(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_taskQueue.push(std::move(task));
    }
    m_cv.notify_one();
}

PyObject *ThreadIOBackend::read(int64_t size) {
    ensure_loop_initialized();
    
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    if (!m_running.load(std::memory_order_relaxed)) {
        resolve_exc(future, g_KeyboardInterrupt, 0, "interrupted");
        return future;
    }

    uint64_t offset; size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        int64_t rem = (int64_t)st.st_size - (int64_t)m_filePos;
        if (rem <= 0) { resolve_bytes(future, nullptr, 0); return future; }
        readSize = (size<0||(size_t)size>rem) ? (size_t)rem : (size_t)size;
        if (readSize == 0) { resolve_bytes(future, nullptr, 0); return future; }
        offset = m_filePos;
        m_filePos += readSize;
    }

    IORequest *req = make_req(readSize, future, ReqType::Read);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, readSize]() {
        ssize_t got = pread(m_fd, req->buf(), readSize, offset);
        if (got >= 0) {
            complete_ok(req, got);
        } else {
            complete_error(req, errno);
        }
    });

    return future;
}

PyObject *ThreadIOBackend::write(Py_buffer *view) {
    ensure_loop_initialized();
    
    size_t size = (size_t)view->len;
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    if (!m_running.load(std::memory_order_relaxed)) {
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
            struct stat st;
            if (fstat(m_fd, &st) != 0) {
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            offset = st.st_size;
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
    }

    IORequest *req = make_req(size, future, ReqType::Write);
    memcpy(req->buf(), view->buf, size);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, size]() {
        ssize_t wrote = pwrite(m_fd, req->buf(), size, offset);
        if (wrote >= 0) {
            complete_ok(req, wrote);
        } else {
            complete_error(req, errno);
        }
    });

    return future;
}

PyObject *ThreadIOBackend::seek(int64_t offset, int whence) {
    ensure_loop_initialized();
    
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (whence == 0) m_filePos = (uint64_t)offset;
        else if (whence == 1) m_filePos = (uint64_t)((int64_t)m_filePos + offset);
        else if (whence == 2) {
            struct stat st;
            if (fstat(m_fd, &st) != 0) {
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            m_filePos = (uint64_t)((int64_t)st.st_size + offset);
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }
    }
    PyObject *pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject *ThreadIOBackend::flush() {
    ensure_loop_initialized();
    
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    if (!m_running.load(std::memory_order_relaxed) || m_fd == -1) {
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    if (fsync(m_fd) != 0) {
        resolve_exc(future, g_OSError, errno, "fsync failed");
        return future;
    }
    resolve_ok(future, Py_None);
    return future;
}

PyObject *ThreadIOBackend::close() {
    // close 可能在没有初始化事件循环的情况下调用
    if (!m_loop_initialized) {
        // 直接同步关闭
        close_impl();
        
        // 尝试获取事件循环创建 future
        PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
        PyObject *future = nullptr;
        
        if (loop) {
            refresh_loop_cache(loop);
            if (g_cachedFutureFn) {
                future = PyObject_CallNoArgs(g_cachedFutureFn);
            }
            Py_DECREF(loop);
        }
        
        if (future) {
            resolve_ok(future, Py_None);
            return future;
        }
        
        // 如果无法创建 future，返回 None
        Py_RETURN_NONE;
    }
    
    ensure_loop_initialized();
    PyObject *future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    close_impl();
    resolve_ok(future, Py_None);
    return future;
}

void ThreadIOBackend::close_impl() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) return;
    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void ThreadIOBackend::complete_ok(IORequest *req, size_t bytes) {
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

void ThreadIOBackend::complete_error(IORequest *req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *exc_class = g_OSError; // map error
    PyObject *exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");

    PyObject *set_fn = req->set_exception; req->set_exception = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_result); req->set_result = nullptr;

    req->loop_handle->push(set_fn, exc);
    delete req;

    PyGILState_Release(gs);
}

IORequest *ThreadIOBackend::make_req(size_t size, PyObject *future, ReqType type) {
    auto *req = new IORequest();
    req->file = this;
    req->loop_handle = m_loop_handle;
    req->future = future; 
    Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize = size;
    req->type = type;
    
    // 使用按需分配的缓冲区池
    if (size <= m_cached_buffer_size) {
        req->poolBuf = pool_acquire_with_size(size);
    } else {
        req->heapBuf = new char[size];
    }
    return req;
}

void ThreadIOBackend::complete_error_inline(IORequest *req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject *exc_class = g_OSError;
    PyObject *exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    PyObject *set_fn = req->set_exception; req->set_exception = nullptr;
    PyObject *r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
    Py_XDECREF(r); Py_DECREF(set_fn); Py_DECREF(exc);
    delete req;
}
