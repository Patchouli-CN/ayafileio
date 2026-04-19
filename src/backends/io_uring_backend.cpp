#ifdef HAVE_IO_URING

#include "io_uring_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <sys/eventfd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdio>

// ════════════════════════════════════════════════════════════════════════════
// 调试宏
// ════════════════════════════════════════════════════════════════════════════

#ifdef AYAFILEIO_VERBOSE_LOGGING
#define LOG(fmt, ...) printf("[IOUringBackend] " fmt "\n", ##__VA_ARGS__); fflush(stdout)
#else
#define LOG(fmt, ...) ((void)0)
#endif

// ════════════════════════════════════════════════════════════════════════════
// 全局缓存（线程安全）
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static LoopHandle* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) return;
    
    LOG("refresh_loop_cache: new loop %p", (void*)loop);
    
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop = loop;
    
    g_cachedFutureFn = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) {
        Py_INCREF(g_cachedFutureFn);
    }
    
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
}

// ════════════════════════════════════════════════════════════════════════════
// 构造函数 / 析构函数
// ════════════════════════════════════════════════════════════════════════════

IOUringBackend::IOUringBackend(const std::string& path, const std::string& mode) {
    LOG("Constructor START: path=%s, mode=%s", path.c_str(), mode.c_str());
    
    // 解析模式
    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        LOG("Constructor ERROR: invalid mode - %s", e.what());
        throw py::value_error(e.what());
    }
    
    if (mi.hasW) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mi.hasA) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (mi.hasX) {
        flags = O_WRONLY | O_CREAT | O_EXCL;
    }
    if (mi.plus) {
        flags = O_RDWR;
    }
    
    m_appendMode = mi.appendMode;
    
    LOG("Constructor: opening file with flags=0x%x", flags);
    m_fd = open(path.c_str(), flags, 0644);
    if (m_fd == -1) {
        LOG("Constructor ERROR: open failed, errno=%d", errno);
        throw_os_error("Failed to open file", path.c_str());
    }
    LOG("Constructor: file opened, fd=%d", m_fd);
    
    // 缓存配置值
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    m_cached_io_uring_queue_depth = cfg.io_uring_queue_depth();
    m_cached_io_uring_flags = cfg.io_uring_flags();
    m_cached_io_uring_sqpoll = cfg.io_uring_sqpoll();
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = static_cast<uint64_t>(st.st_size);
        }
    }
    
    LOG("Constructor END: fd=%d, appendMode=%d", m_fd, m_appendMode);
}

IOUringBackend::~IOUringBackend() {
    LOG("Destructor START: fd=%d", m_fd);
    close_impl();
    if (m_loop_initialized) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
    LOG("Destructor END");
}

// ════════════════════════════════════════════════════════════════════════════
// 初始化方法
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::ensure_loop_initialized() {
    LOG("ensure_loop_initialized: m_loop_initialized=%d", m_loop_initialized);
    
    if (m_loop_initialized) return;
    
    LOG("ensure_loop_initialized: calling get_running_loop");
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) {
        PyErr_Clear();
        LOG("ensure_loop_initialized ERROR: No running event loop");
        throw std::runtime_error("No running event loop");
    }
    LOG("ensure_loop_initialized: got loop %p", (void*)loop);
    
    bool need_start_uring = false;
    {
        std::lock_guard<std::mutex> lk(m_loop_init_mtx);
        if (m_loop_initialized) {
            Py_DECREF(loop);
            LOG("ensure_loop_initialized: already initialized by another thread");
            return;
        }
        
        refresh_loop_cache(loop);
        m_loop = loop;
        Py_INCREF(m_loop);
        m_create_future = g_cachedFutureFn;
        Py_INCREF(m_create_future);
        m_loop_handle = g_cachedLoopHandle;
        
        if (!m_uring_started) {
            need_start_uring = true;
        }
        
        m_loop_initialized = true;
    }
    
    // 在锁外启动 io_uring，避免死锁
    if (need_start_uring) {
        LOG("ensure_loop_initialized: starting io_uring (outside lock)");
        start_uring();
    }
    
    LOG("ensure_loop_initialized: DONE");
}

void IOUringBackend::start_uring() {
    LOG("start_uring START: m_uring_started=%d", m_uring_started);
    
    std::lock_guard<std::mutex> lk(m_loop_init_mtx);
    if (m_uring_started) {
        LOG("start_uring: already started");
        return;
    }
    
    LOG("start_uring: creating eventfd");
    m_event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_event_fd == -1) {
        LOG("start_uring ERROR: eventfd failed, errno=%d", errno);
        throw_os_error("Failed to create eventfd");
    }
    LOG("start_uring: eventfd created, fd=%d", m_event_fd);
    
    if (!setup_uring()) {
        LOG("start_uring ERROR: setup_uring failed");
        ::close(m_event_fd);
        m_event_fd = -1;
        throw std::runtime_error("Failed to setup io_uring");
    }
    
    m_uring_started = true;
    LOG("start_uring END");
}

bool IOUringBackend::setup_uring() {
    LOG("setup_uring START: queue_depth=%u", m_cached_io_uring_queue_depth);
    
    unsigned flags = m_cached_io_uring_flags;
    if (m_cached_io_uring_sqpoll) {
        flags |= IORING_SETUP_SQPOLL;
        LOG("setup_uring: SQPOLL enabled");
    }
    
#ifdef IORING_SETUP_SINGLE_ISSUER
    flags |= IORING_SETUP_SINGLE_ISSUER;
    LOG("setup_uring: SINGLE_ISSUER enabled");
#endif
    
#ifdef IORING_SETUP_DEFER_TASKRUN
    flags |= IORING_SETUP_DEFER_TASKRUN;
    LOG("setup_uring: DEFER_TASKRUN enabled");
#endif
    
    int ret = io_uring_queue_init(m_cached_io_uring_queue_depth, &m_ring, flags);
    if (ret < 0) {
        LOG("setup_uring ERROR: io_uring_queue_init failed, ret=%d, errno=%d", ret, errno);
        return false;
    }
    LOG("setup_uring: io_uring_queue_init success");
    
    m_reaper_stop.store(false);
    m_reaper_thread = std::thread(&IOUringBackend::reaper_loop, this);
    LOG("setup_uring: reaper thread started");
    
    return true;
}

void IOUringBackend::teardown_uring() {
    LOG("teardown_uring START: m_uring_started=%d", m_uring_started);
    if (!m_uring_started) return;
    
    m_reaper_stop.store(true);
    LOG("teardown_uring: m_reaper_stop set to true");
    
    if (m_event_fd != -1) {
        uint64_t val = 1;
        ssize_t written = ::write(m_event_fd, &val, sizeof(val));
        LOG("teardown_uring: wrote to eventfd, written=%zd", written);
    }
    
    if (m_reaper_thread.joinable()) {
        LOG("teardown_uring: joining reaper thread");
        m_reaper_thread.join();
        LOG("teardown_uring: reaper thread joined");
    }
    
    io_uring_queue_exit(&m_ring);
    LOG("teardown_uring: io_uring_queue_exit done");
    
    if (m_event_fd != -1) {
        ::close(m_event_fd);
        m_event_fd = -1;
    }
    
    m_uring_started = false;
    LOG("teardown_uring END");
}

// ════════════════════════════════════════════════════════════════════════════
// Reaper 线程
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::reaper_loop() {
    LOG("reaper_loop START");
    
    uint64_t event_val;
    struct io_uring_sqe* sqe;
    struct io_uring_cqe* cqe;
    
    sqe = io_uring_get_sqe(&m_ring);
    if (sqe) {
        io_uring_prep_read(sqe, m_event_fd, &event_val, sizeof(event_val), 0);
        io_uring_sqe_set_data(sqe, nullptr);
        io_uring_submit(&m_ring);
        LOG("reaper_loop: submitted eventfd read");
    } else {
        LOG("reaper_loop ERROR: failed to get SQE for eventfd");
    }
    
    while (!m_reaper_stop.load(std::memory_order_relaxed)) {
        LOG("reaper_loop: waiting for CQE");
        int ret = io_uring_wait_cqe(&m_ring, &cqe);
        if (ret < 0) {
            LOG("reaper_loop: io_uring_wait_cqe returned %d, errno=%d", ret, errno);
            if (errno == EINTR) continue;
            break;
        }
        LOG("reaper_loop: got CQE");
        
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&m_ring, head, cqe) {
            IORequest* req = static_cast<IORequest*>(io_uring_cqe_get_data(cqe));
            if (req) {
                LOG("reaper_loop: processing request %p, res=%d", (void*)req, cqe->res);
                if (cqe->res >= 0) {
                    complete_ok(req, static_cast<size_t>(cqe->res));
                } else {
                    complete_error(req, static_cast<DWORD>(-cqe->res));
                }
            } else {
                LOG("reaper_loop: eventfd wakeup");
                if (!m_reaper_stop.load(std::memory_order_relaxed)) {
                    sqe = io_uring_get_sqe(&m_ring);
                    if (sqe) {
                        io_uring_prep_read(sqe, m_event_fd, &event_val, sizeof(event_val), 0);
                        io_uring_sqe_set_data(sqe, nullptr);
                        io_uring_submit(&m_ring);
                        LOG("reaper_loop: resubmitted eventfd read");
                    }
                }
            }
            count++;
        }
        
        if (count > 0) {
            io_uring_cq_advance(&m_ring, count);
            LOG("reaper_loop: advanced %u CQEs", count);
        }
    }
    
    LOG("reaper_loop END");
}

// ════════════════════════════════════════════════════════════════════════════
// I/O 提交
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::submit_io(IORequest* req, int op, int fd, 
                                const void* buf, size_t len, off_t offset) {
    LOG("submit_io: req=%p, op=%d, fd=%d, len=%zu, offset=%ld", 
        (void*)req, op, fd, len, (long)offset);
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        LOG("submit_io ERROR: failed to get SQE");
        complete_error(req, EBUSY);
        return;
    }
    
    void* writeable_buf = const_cast<void*>(buf);
    
    if (op == IORING_OP_READ) {
        io_uring_prep_read(sqe, fd, writeable_buf, static_cast<unsigned>(len), offset);
    } else if (op == IORING_OP_WRITE) {
        io_uring_prep_write(sqe, fd, writeable_buf, static_cast<unsigned>(len), offset);
    } else if (op == IORING_OP_FSYNC) {
        io_uring_prep_fsync(sqe, fd, 0);
    } else {
        LOG("submit_io ERROR: invalid op %d", op);
        complete_error(req, EINVAL);
        return;
    }
    
    io_uring_sqe_set_data(sqe, req);
    int ret = io_uring_submit(&m_ring);
    LOG("submit_io: submitted, ret=%d", ret);
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject* IOUringBackend::read(int64_t size) {
    LOG("read START: size=%ld", (long)size);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        LOG("read ERROR: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, 
                                      "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_relaxed), m_fd, m_create_future, m_loop);
    if (closed_future) {
        LOG("read: file closed, returning rejected future");
        Py_DECREF(future);
        return closed_future;
    }
    
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            LOG("read ERROR: fstat failed, errno=%d", errno);
            set_os_error("fstat failed");
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        
        int64_t rem = static_cast<int64_t>(st.st_size) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) {
            LOG("read: EOF, rem=%ld", (long)rem);
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
            LOG("read: readSize=0");
            resolve_bytes(future, nullptr, 0);
            return future;
        }
        
        offset = m_filePos;
        m_filePos += readSize;
        LOG("read: offset=%llu, readSize=%zu", (unsigned long long)offset, readSize);
    }
    
    IORequest* req = make_req(readSize, future, ReqType::Read);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_READ, m_fd, req->buf(), readSize, 
              static_cast<off_t>(offset));
    
    LOG("read END: future=%p", (void*)future);
    return future;
}

PyObject* IOUringBackend::write(Py_buffer* view) {
    LOG("write START: len=%zd", view->len);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        LOG("write ERROR: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, 
                                      "No running event loop", 0);
    }
    
    size_t size = static_cast<size_t>(view->len);
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_relaxed), m_fd, m_create_future, m_loop);
    if (closed_future) {
        LOG("write: file closed, returning rejected future");
        Py_DECREF(future);
        return closed_future;
    }
    
    if (size == 0) {
        LOG("write: size=0");
        PyObject* z = PyLong_FromLong(0);
        resolve_ok(future, z);
        Py_DECREF(z);
        return future;
    }
    
    uint64_t offset;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        
        if (m_appendMode) {
            struct stat st;
            if (fstat(m_fd, &st) != 0) {
                LOG("write ERROR: fstat failed, errno=%d", errno);
                set_os_error("fstat failed");
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            offset = static_cast<uint64_t>(st.st_size);
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
        LOG("write: offset=%llu, size=%zu", (unsigned long long)offset, size);
    }
    
    IORequest* req = make_req(size, future, ReqType::Write);
    std::memcpy(req->buf(), view->buf, size);
    
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_WRITE, m_fd, req->buf(), size, 
              static_cast<off_t>(offset));
    
    LOG("write END: future=%p", (void*)future);
    return future;
}

PyObject* IOUringBackend::seek(int64_t offset, int whence) {
    LOG("seek START: offset=%ld, whence=%d", (long)offset, whence);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        LOG("seek ERROR: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, 
                                      "No running event loop", 0);
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
                LOG("seek ERROR: fstat failed, errno=%d", errno);
                set_os_error("fstat failed");
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            m_filePos = static_cast<uint64_t>(static_cast<int64_t>(st.st_size) + offset);
        } else {
            LOG("seek ERROR: invalid whence=%d", whence);
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }
    }
    
    PyObject* pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos);
    Py_DECREF(pos);
    
    LOG("seek END: new_pos=%llu", (unsigned long long)m_filePos);
    return future;
}

PyObject* IOUringBackend::flush() {
    LOG("flush START");
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        LOG("flush ERROR: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, 
                                      "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_relaxed) || m_fd == -1) {
        LOG("flush ERROR: file closed");
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    
    IORequest* req = make_req(0, future, ReqType::Other);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_FSYNC, m_fd, nullptr, 0, 0);
    
    LOG("flush END");
    return future;
}

PyObject* IOUringBackend::close() {
    LOG("close START: m_loop_initialized=%d", m_loop_initialized);
    
    if (!m_loop_initialized) {
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) {
            PyErr_Clear();
            LOG("close: no event loop, closing directly");
            close_impl();
            Py_RETURN_NONE;
        }
        
        LOG("close: creating resolved future for uninitialized backend");
        PyObject* future = create_resolved_future(loop, Py_None);
        Py_DECREF(loop);
        
        if (!future) {
            close_impl();
            return nullptr;
        }
        
        close_impl();
        return future;
    }
    
    ensure_loop_initialized();
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    close_impl();
    resolve_ok(future, Py_None);
    
    LOG("close END");
    return future;
}

void IOUringBackend::close_impl() {
    LOG("close_impl START");
    
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) {
        LOG("close_impl: already closed");
        return;
    }
    
    LOG("close_impl: waiting for pending I/O (pending=%ld)", m_pending.load());
    int elapsed = 0;
    int wait_time = 1;
    while (elapsed < static_cast<int>(m_cached_close_timeout_ms) && 
           m_pending.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));
        elapsed += wait_time;
        wait_time = std::min(wait_time * 2, 32);
    }
    LOG("close_impl: wait complete, elapsed=%dms", elapsed);
    
    teardown_uring();
    
    if (m_fd != -1) {
        ::close(m_fd);
        LOG("close_impl: closed fd=%d", m_fd);
        m_fd = -1;
    }
    
    LOG("close_impl END");
}

// ════════════════════════════════════════════════════════════════════════════
// 完成处理
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::complete_ok(IORequest* req, size_t bytes) {
    LOG("complete_ok: req=%p, bytes=%zu", (void*)req, bytes);
    m_pending.fetch_sub(1, std::memory_order_release);
    
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* val = nullptr;
    if (req->type == ReqType::Read) {
        val = PyBytes_FromStringAndSize(req->buf(), static_cast<Py_ssize_t>(bytes));
    } else if (req->type == ReqType::Write) {
        val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
    } else {
        val = Py_None;
        Py_INCREF(Py_None);
    }
    
    PyObject* set_fn = req->set_result;
    req->set_result = nullptr;
    Py_DECREF(req->future);
    req->future = nullptr;
    Py_XDECREF(req->set_exception);
    req->set_exception = nullptr;
    
    req->loop_handle->push(set_fn, val);
    delete req;
    
    PyGILState_Release(gs);
    LOG("complete_ok: DONE");
}

void IOUringBackend::complete_error(IORequest* req, DWORD err) {
    LOG("complete_error: req=%p, err=%u", (void*)req, (unsigned)err);
    m_pending.fetch_sub(1, std::memory_order_release);
    
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* exc_class = map_posix_error(static_cast<int>(err));
    PyObject* exc = PyObject_CallFunction(exc_class, "is", 
                                           static_cast<int>(err), 
                                           "I/O operation failed");
    
    PyObject* set_fn = req->set_exception;
    req->set_exception = nullptr;
    Py_DECREF(req->future);
    req->future = nullptr;
    Py_XDECREF(req->set_result);
    req->set_result = nullptr;
    
    req->loop_handle->push(set_fn, exc);
    delete req;
    
    PyGILState_Release(gs);
    LOG("complete_error: DONE");
}

// ════════════════════════════════════════════════════════════════════════════
// 辅助方法
// ════════════════════════════════════════════════════════════════════════════

IORequest* IOUringBackend::make_req(size_t size, PyObject* future, ReqType type) {
    auto* req = new IORequest();
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
    
    LOG("make_req: req=%p, size=%zu, type=%d", (void*)req, size, (int)type);
    return req;
}

void IOUringBackend::complete_error_inline(IORequest* req, DWORD err) {
    LOG("complete_error_inline: req=%p, err=%u", (void*)req, (unsigned)err);
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    
    PyObject* exc_class = map_posix_error(static_cast<int>(err));
    PyObject* exc = PyObject_CallFunction(exc_class, "is", 
                                           static_cast<int>(err), 
                                           "I/O operation failed");
    PyObject* set_fn = req->set_exception;
    req->set_exception = nullptr;
    PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
    Py_XDECREF(r);
    Py_DECREF(set_fn);
    Py_DECREF(exc);
    delete req;
    LOG("complete_error_inline: DONE");
}

#endif // HAVE_IO_URING