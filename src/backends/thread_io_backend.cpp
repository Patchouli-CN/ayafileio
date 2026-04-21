#include "thread_io_backend.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include "../globals.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include "../debug_log.hpp"
#include <thread>

// ════════════════════════════════════════════════════════════════════════════
// 全局缓存（线程安全）
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static LoopHandle* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    UR_DEBUG_LOG("ThreadIOBackend: refresh_loop_cache start, loop=%p", (void*)loop);
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) {
        UR_DEBUG_LOG("ThreadIOBackend: refresh_loop_cache cache hit");
        return;
    }
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop       = loop;
    g_cachedFutureFn   = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) {
        Py_INCREF(g_cachedFutureFn);
        UR_DEBUG_LOG("ThreadIOBackend: refresh_loop_cache got create_future");
    } else {
        UR_DEBUG_LOG("ThreadIOBackend: refresh_loop_cache FAILED to get create_future");
    }
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
    UR_DEBUG_LOG("ThreadIOBackend: refresh_loop_cache done");
}

// ════════════════════════════════════════════════════════════════════════════
// 构造函数 / 析构函数
// ════════════════════════════════════════════════════════════════════════════

ThreadIOBackend::ThreadIOBackend(const std::string &path, const std::string &mode) {
    UR_DEBUG_LOG("ThreadIOBackend: constructor start, path=%s, mode=%s", path.c_str(), mode.c_str());
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    m_num_workers = cfg.io_worker_count();
    if (m_num_workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 1;
        m_num_workers = std::max(1u, std::min(hc * 2u, 16u));
        UR_DEBUG_LOG("ThreadIOBackend: auto worker count = %u (hc=%u)", m_num_workers, hc);
    } else {
        UR_DEBUG_LOG("ThreadIOBackend: configured worker count = %u", m_num_workers);
    }

    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        UR_DEBUG_LOG("ThreadIOBackend: parse_mode failed: %s", e.what());
        throw py::value_error(e.what());
    }
    bool appendMode = mi.appendMode;

    if (mi.hasW) { flags = O_WRONLY | O_CREAT | O_TRUNC; }
    else if (mi.hasA) { flags = O_WRONLY | O_CREAT | O_APPEND; }
    else if (mi.hasX) { flags = O_WRONLY | O_CREAT | O_EXCL; }
    if (mi.plus) { flags = O_RDWR; }

    m_appendMode = appendMode;

    UR_DEBUG_LOG("ThreadIOBackend: opening file with flags=%d", flags);
    m_fd = open(path.c_str(), flags, 0644);
    if (m_fd == -1) {
        UR_DEBUG_LOG("ThreadIOBackend: open failed, errno=%d", errno);
        throw_os_error("Failed to open file", path.c_str());
    }
    UR_DEBUG_LOG("ThreadIOBackend: file opened, fd=%d", m_fd);

    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = st.st_size;
            UR_DEBUG_LOG("ThreadIOBackend: append mode, filePos=%llu", (unsigned long long)m_filePos);
        }
    }
    
    UR_DEBUG_LOG("ThreadIOBackend: constructor done, this=%p", (void*)this);
}

ThreadIOBackend::~ThreadIOBackend() {
    UR_DEBUG_LOG("ThreadIOBackend: destructor start, this=%p", (void*)this);
    close_impl();
    if (m_loop_initialized.load(std::memory_order_acquire)) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
    UR_DEBUG_LOG("ThreadIOBackend: destructor done");
}

// ════════════════════════════════════════════════════════════════════════════
// 初始化方法
// ════════════════════════════════════════════════════════════════════════════

void ThreadIOBackend::start_workers() {
    UR_DEBUG_LOG("ThreadIOBackend: start_workers called, this=%p", (void*)this);
    
    bool expected = false;
    if (!m_workers_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        UR_DEBUG_LOG("ThreadIOBackend: workers already started, skipping");
        return;
    }
    
    UR_DEBUG_LOG("ThreadIOBackend: starting %u workers", m_num_workers);
    m_workers.reserve(m_num_workers);
    for (unsigned i = 0; i < m_num_workers; ++i) {
        UR_DEBUG_LOG("ThreadIOBackend: creating worker %u", i);
        m_workers.emplace_back(&ThreadIOBackend::worker_thread, this);
    }
    UR_DEBUG_LOG("ThreadIOBackend: all workers created");
}

void ThreadIOBackend::ensure_loop_initialized() {
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized start, this=%p, already_init=%d", 
                 (void*)this, m_loop_initialized.load());
    
    if (m_loop_initialized.load(std::memory_order_acquire)) {
        UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized already initialized, returning");
        return;
    }
    
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized calling get_running_loop...");
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) {
        PyErr_Clear();
        UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized NO RUNNING LOOP");
        throw std::runtime_error("No running event loop");
    }
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized got loop=%p", (void*)loop);
    
    {
        std::lock_guard<std::mutex> lk(m_loop_init_mtx);
        UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized inside lock");
        
        if (m_loop_initialized.load(std::memory_order_relaxed)) {
            UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized another thread initialized");
            Py_DECREF(loop);
            return;
        }
        
        UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized refreshing cache...");
        refresh_loop_cache(loop);
        m_loop = loop;
        Py_INCREF(m_loop);
        m_create_future = g_cachedFutureFn;
        Py_INCREF(m_create_future);
        m_loop_handle = g_cachedLoopHandle;
        
        m_loop_initialized.store(true, std::memory_order_release);
        UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized marked as initialized");
    }
    
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized releasing GIL before starting workers...");
    PyThreadState* _save = PyEval_SaveThread();
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized GIL released, starting workers...");
    start_workers();
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized workers started, restoring GIL...");
    PyEval_RestoreThread(_save);
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized done, this=%p", (void*)this);
}

void ThreadIOBackend::worker_thread() {
    UR_DEBUG_LOG("ThreadIOBackend: worker_thread started");
    
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_queueMtx);
            UR_DEBUG_LOG("ThreadIOBackend: worker waiting for task...");
            m_cv.wait(lk, [this] { 
                return m_stop.load(std::memory_order_acquire) || !m_taskQueue.empty(); 
            });
            
            if (m_stop.load(std::memory_order_acquire) && m_taskQueue.empty()) {
                UR_DEBUG_LOG("ThreadIOBackend: worker stopping");
                return;
            }
            
            task = std::move(m_taskQueue.front());
            m_taskQueue.pop();
            UR_DEBUG_LOG("ThreadIOBackend: worker got task, queue_size=%zu", m_taskQueue.size());
        }
        
        if (task) {
            UR_DEBUG_LOG("ThreadIOBackend: worker executing task");
            task();
            UR_DEBUG_LOG("ThreadIOBackend: worker task completed");
        }
    }
}

void ThreadIOBackend::enqueue_task(std::function<void()> task) {
    UR_DEBUG_LOG("ThreadIOBackend: enqueue_task, this=%p", (void*)this);
    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_taskQueue.push(std::move(task));
        UR_DEBUG_LOG("ThreadIOBackend: task enqueued, queue_size=%zu", m_taskQueue.size());
    }
    m_cv.notify_one();
    UR_DEBUG_LOG("ThreadIOBackend: notified one worker");
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject *ThreadIOBackend::read(int64_t size) {
    UR_DEBUG_LOG("ThreadIOBackend::read start, this=%p, size=%ld", (void*)this, size);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("ThreadIOBackend::read ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) {
        UR_DEBUG_LOG("ThreadIOBackend::read failed to create future");
        return nullptr;
    }
    UR_DEBUG_LOG("ThreadIOBackend::read future created");

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) {
        UR_DEBUG_LOG("ThreadIOBackend::read file is closed");
        Py_DECREF(future);
        return closed_future;
    }

    uint64_t offset; size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            UR_DEBUG_LOG("ThreadIOBackend::read fstat failed, errno=%d", errno);
            set_os_error("fstat failed");
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        int64_t rem = (int64_t)st.st_size - (int64_t)m_filePos;
        if (rem <= 0) { 
            UR_DEBUG_LOG("ThreadIOBackend::read EOF");
            resolve_bytes(future, nullptr, 0); 
            return future; 
        }
        
        if (size < 0) {
            readSize = static_cast<size_t>(rem);
        } else {
            size_t sz = static_cast<size_t>(size);
            size_t r = static_cast<size_t>(rem);
            readSize = (sz > r) ? r : sz;
        }
        
        if (readSize == 0) { 
            resolve_bytes(future, nullptr, 0); 
            return future; 
        }
        offset = m_filePos;
        m_filePos += readSize;
    }

    IORequest *req = make_req(readSize, future, ReqType::Read);
    UR_DEBUG_LOG("ThreadIOBackend::read req=%p, offset=%llu, size=%zu", 
                 (void*)req, (unsigned long long)offset, readSize);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, readSize]() {
        UR_DEBUG_LOG("ThreadIOBackend::read task executing, fd=%d, offset=%llu, size=%zu",
                     m_fd, (unsigned long long)offset, readSize);
        ssize_t got = pread(m_fd, req->buf(), readSize, offset);
        UR_DEBUG_LOG("ThreadIOBackend::read task done, got=%zd", got);
        if (got >= 0) {
            complete_ok(req, static_cast<size_t>(got));
        } else {
            UR_DEBUG_LOG("ThreadIOBackend::read task failed, errno=%d", errno);
            complete_error(req, errno);
        }
    });

    UR_DEBUG_LOG("ThreadIOBackend::read returning future");
    return future;
}

PyObject *ThreadIOBackend::write(Py_buffer *view) {
    UR_DEBUG_LOG("ThreadIOBackend::write start, this=%p, size=%zd", (void*)this, view->len);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("ThreadIOBackend::write ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    size_t size = static_cast<size_t>(view->len);
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) {
        Py_DECREF(future);
        return closed_future;
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
                set_os_error("fstat failed");
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            offset = static_cast<uint64_t>(st.st_size);
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
    }

    IORequest *req = make_req(size, future, ReqType::Write);
    memcpy(req->buf(), view->buf, size);
    UR_DEBUG_LOG("ThreadIOBackend::write req=%p, offset=%llu, size=%zu", 
                 (void*)req, (unsigned long long)offset, size);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, size]() {
        UR_DEBUG_LOG("ThreadIOBackend::write task executing, fd=%d, offset=%llu, size=%zu",
                     m_fd, (unsigned long long)offset, size);
        ssize_t wrote = pwrite(m_fd, req->buf(), size, static_cast<off_t>(offset));
        UR_DEBUG_LOG("ThreadIOBackend::write task done, wrote=%zd", wrote);
        if (wrote >= 0) {
            complete_ok(req, static_cast<size_t>(wrote));
        } else {
            UR_DEBUG_LOG("ThreadIOBackend::write task failed, errno=%d", errno);
            complete_error(req, errno);
        }
    });

    return future;
}

PyObject *ThreadIOBackend::seek(int64_t offset, int whence) {
    UR_DEBUG_LOG("ThreadIOBackend::seek start, offset=%ld, whence=%d", offset, whence);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (whence == 0) {
            m_filePos = static_cast<uint64_t>(offset);
        } else if (whence == 1) {
            m_filePos = static_cast<uint64_t>(static_cast<int64_t>(m_filePos) + offset);
        } else if (whence == 2) {
            struct stat st;
            if (fstat(m_fd, &st) != 0) {
                set_os_error("fstat failed");
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            m_filePos = static_cast<uint64_t>(static_cast<int64_t>(st.st_size) + offset);
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }
    }
    UR_DEBUG_LOG("ThreadIOBackend::seek new pos=%llu", (unsigned long long)m_filePos);
    PyObject *pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject *ThreadIOBackend::flush() {
    UR_DEBUG_LOG("ThreadIOBackend::flush start, this=%p", (void*)this);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_acquire) || m_fd == -1) {
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    
    if (fsync(m_fd) != 0) {
        UR_DEBUG_LOG("ThreadIOBackend::flush fsync failed, errno=%d", errno);
        set_os_error("fsync failed");
        resolve_exc(future, g_OSError, errno, "fsync failed");
        return future;
    }
    UR_DEBUG_LOG("ThreadIOBackend::flush done");
    resolve_ok(future, Py_None);
    return future;
}

PyObject *ThreadIOBackend::close() {
    UR_DEBUG_LOG("ThreadIOBackend::close start, this=%p, initialized=%d", 
                 (void*)this, m_loop_initialized.load());
    
    if (!m_loop_initialized.load(std::memory_order_acquire)) {
        UR_DEBUG_LOG("ThreadIOBackend::close not initialized, closing directly");
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) {
            PyErr_Clear();
            close_impl();
            Py_RETURN_NONE;
        }
        
        PyObject* future = create_resolved_future(loop, Py_None);
        Py_DECREF(loop);
        
        if (!future) {
            close_impl();
            return nullptr;
        }
        
        close_impl();
        return future;
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    close_impl();
    resolve_ok(future, Py_None);
    return future;
}

void ThreadIOBackend::close_impl() {
    UR_DEBUG_LOG("ThreadIOBackend::close_impl start, this=%p, fd=%d", (void*)this, m_fd);
    
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        UR_DEBUG_LOG("ThreadIOBackend::close_impl already closed");
        return;
    }
    
    UR_DEBUG_LOG("ThreadIOBackend::close_impl stopping workers...");
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();
    
    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workers.clear();
    UR_DEBUG_LOG("ThreadIOBackend::close_impl workers stopped");
    
    int elapsed = 0;
    int wait_time = 1;
    while (elapsed < static_cast<int>(m_cached_close_timeout_ms) && 
           m_pending.load(std::memory_order_acquire) > 0) {
        UR_DEBUG_LOG("ThreadIOBackend::close_impl waiting for pending I/O, elapsed=%d, pending=%ld",
                     elapsed, m_pending.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));
        elapsed += wait_time;
        wait_time = std::min(wait_time * 2, 32);
    }
    
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
        UR_DEBUG_LOG("ThreadIOBackend::close_impl fd closed");
    }
    UR_DEBUG_LOG("ThreadIOBackend::close_impl done");
}

// ════════════════════════════════════════════════════════════════════════════
// 完成处理（工作线程中调用）
// ════════════════════════════════════════════════════════════════════════════

void ThreadIOBackend::complete_ok(IORequest *req, size_t bytes) {
    UR_DEBUG_LOG("ThreadIOBackend::complete_ok req=%p, bytes=%zu", (void*)req, bytes);
    m_pending.fetch_sub(1, std::memory_order_release);
    
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *val = nullptr;
    if (req->type == ReqType::Read) {
        val = PyBytes_FromStringAndSize(req->buf(), static_cast<Py_ssize_t>(bytes));
    } else if (req->type == ReqType::Write) {
        val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
    } else {
        val = Py_None;
        Py_INCREF(Py_None);
    }

    PyObject *set_fn = req->set_result; 
    req->set_result = nullptr;
    Py_DECREF(req->future); 
    req->future = nullptr;
    Py_XDECREF(req->set_exception); 
    req->set_exception = nullptr;

    req->loop_handle->push(set_fn, val);
    delete req;

    PyGILState_Release(gs);
    UR_DEBUG_LOG("ThreadIOBackend::complete_ok done");
}

void ThreadIOBackend::complete_error(IORequest *req, DWORD err) {
    UR_DEBUG_LOG("ThreadIOBackend::complete_error req=%p, err=%u", (void*)req, err);
    m_pending.fetch_sub(1, std::memory_order_release);
    
    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *exc_class = map_posix_error(static_cast<int>(err));
    PyObject *exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");

    PyObject *set_fn = req->set_exception; 
    req->set_exception = nullptr;
    Py_DECREF(req->future); 
    req->future = nullptr;
    Py_XDECREF(req->set_result); 
    req->set_result = nullptr;

    req->loop_handle->push(set_fn, exc);
    delete req;

    PyGILState_Release(gs);
    UR_DEBUG_LOG("ThreadIOBackend::complete_error done");
}

// ════════════════════════════════════════════════════════════════════════════
// 辅助方法
// ════════════════════════════════════════════════════════════════════════════

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
    
    if (size <= m_cached_buffer_size) {
        req->poolBuf = pool_acquire_with_size(size);
    } else {
        req->heapBuf = new char[size];
    }
    return req;
}

void ThreadIOBackend::complete_error_inline(IORequest *req, DWORD err) {
    UR_DEBUG_LOG("ThreadIOBackend::complete_error_inline req=%p, err=%u", (void*)req, err);
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject *exc_class = map_posix_error(static_cast<int>(err));
    PyObject *exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");
    PyObject *set_fn = req->set_exception; 
    req->set_exception = nullptr;
    PyObject *r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
    Py_XDECREF(r); 
    Py_DECREF(set_fn); 
    Py_DECREF(exc);
    delete req;
}
