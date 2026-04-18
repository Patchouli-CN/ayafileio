#ifdef HAVE_IO_URING

#include "io_uring_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
#include "./utils/file_mode.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

// 缓存事件循环 (与其他后端相同)
static PyObject* g_cachedLoop = nullptr;
static PyObject* g_cachedFutureFn = nullptr;
static LoopHandle* g_cachedLoopHandle = nullptr;

static void refresh_loop_cache(PyObject* loop) {
    if (loop == g_cachedLoop) return;
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop = loop;
    g_cachedFutureFn = PyObject_GetAttr(loop, g_str_create_future);
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
}

IOUringBackend::IOUringBackend(const std::string& path, const std::string& mode) {
    // 获取事件循环
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    refresh_loop_cache(loop);
    m_loop = loop;
    m_create_future = g_cachedFutureFn;
    Py_INCREF(m_create_future);
    m_loop_handle = g_cachedLoopHandle;

    // 解析模式
    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        throw py::value_error(e.what());
    }
    
    if (mi.hasW) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mi.hasA) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mi.hasX) flags = O_WRONLY | O_CREAT | O_EXCL;
    if (mi.plus) flags = O_RDWR;
    
    m_appendMode = mi.appendMode;
    
    // 打开文件
    m_fd = open(path.c_str(), flags, 0644);
    if (m_fd == -1) {
        throw py::python_error();
    }
    
    // 缓存配置值（性能优化）
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    m_cached_io_uring_queue_depth = cfg.io_uring_queue_depth();
    m_cached_io_uring_flags = cfg.io_uring_flags();
    m_cached_io_uring_sqpoll = cfg.io_uring_sqpoll();
    
    // 设置 io_uring
    if (!setup_uring()) {
        ::close(m_fd);
        throw std::runtime_error("Failed to setup io_uring");
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
}

IOUringBackend::~IOUringBackend() {
    close_impl();
    Py_XDECREF(m_create_future);
    Py_XDECREF(m_loop);
}

bool IOUringBackend::setup_uring() {
    // 使用缓存的配置值
    unsigned flags = m_cached_io_uring_flags;
    
    // 如果启用 SQPOLL
    if (m_cached_io_uring_sqpoll) {
        flags |= IORING_SETUP_SQPOLL;
    }
    
    int ret = io_uring_queue_init(m_cached_io_uring_queue_depth, &m_ring, flags);
    if (ret < 0) return false;
    
    m_reaper_stop.store(false);
    m_reaper_thread = std::thread(&IOUringBackend::reaper_loop, this);
    return true;
}

void IOUringBackend::teardown_uring() {
    m_reaper_stop.store(true);
    if (m_reaper_thread.joinable()) {
        m_reaper_thread.join();
    }
    io_uring_queue_exit(&m_ring);
}

void IOUringBackend::reaper_loop() {
    while (!m_reaper_stop.load(std::memory_order_relaxed)) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&m_ring, &cqe);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!cqe) continue;
        
        IORequest* req = (IORequest*)io_uring_cqe_get_data(cqe);
        if (req) {
            if (cqe->res >= 0) {
                complete_ok(req, cqe->res);
            } else {
                complete_error(req, -cqe->res);
            }
        }
        io_uring_cqe_seen(&m_ring, cqe);
    }
}

void IOUringBackend::submit_io(IORequest* req, int op, int fd, 
                                const void* buf, size_t len, off_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        complete_error(req, EBUSY);
        return;
    }
    
    io_uring_prep_rw(op, sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&m_ring);
}

PyObject* IOUringBackend::read(int64_t size) {
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_relaxed) || m_fd == -1) {
        resolve_exc(future, g_ValueError, 0, "I/O operation on closed file.");
        return future;
    }
    
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        int64_t rem = (int64_t)st.st_size - (int64_t)m_filePos;
        if (rem <= 0) {
            resolve_bytes(future, nullptr, 0);
            return future;
        }
        readSize = (size < 0 || (size_t)size > (size_t)rem) ? (size_t)rem : (size_t)size;
        if (readSize == 0) {
            resolve_bytes(future, nullptr, 0);
            return future;
        }
        offset = m_filePos;
        m_filePos += readSize;
    }
    
    IORequest* req = make_req(readSize, future, ReqType::Read);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_READ, m_fd, req->buf(), readSize, offset);
    
    return future;
}

PyObject* IOUringBackend::write(Py_buffer* view) {
    size_t size = (size_t)view->len;
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_relaxed) || m_fd == -1) {
        resolve_exc(future, g_ValueError, 0, "I/O operation on closed file.");
        return future;
    }
    
    if (size == 0) {
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
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            offset = st.st_size;
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
    }
    
    IORequest* req = make_req(size, future, ReqType::Write);
    memcpy(req->buf(), view->buf, size);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_WRITE, m_fd, req->buf(), size, offset);
    
    return future;
}

PyObject* IOUringBackend::seek(int64_t offset, int whence) {
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (whence == 0) {
            m_filePos = (uint64_t)offset;
        } else if (whence == 1) {
            m_filePos = (uint64_t)((int64_t)m_filePos + offset);
        } else if (whence == 2) {
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
    
    PyObject* pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos);
    Py_DECREF(pos);
    return future;
}

PyObject* IOUringBackend::flush() {
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_relaxed) || m_fd == -1) {
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    
    if (m_fd != -1) {
        IORequest* req = make_req(0, future, ReqType::Other);
        m_pending.fetch_add(1, std::memory_order_relaxed);
        submit_io(req, IORING_OP_FSYNC, m_fd, nullptr, 0, 0);
    } else {
        resolve_ok(future, Py_None);
    }
    
    return future;
}

PyObject* IOUringBackend::close() {
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    close_impl();
    resolve_ok(future, Py_None);
    return future;
}

void IOUringBackend::close_impl() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) return;
    
    teardown_uring();
    
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void IOUringBackend::complete_ok(IORequest* req, size_t bytes) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* val = nullptr;
    if (req->type == ReqType::Read) {
        val = PyBytes_FromStringAndSize(req->buf(), bytes);
    } else if (req->type == ReqType::Write) {
        val = PyLong_FromSsize_t((Py_ssize_t)bytes);
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
}

void IOUringBackend::complete_error(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* exc_class = g_OSError;
    PyObject* exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    
    PyObject* set_fn = req->set_exception;
    req->set_exception = nullptr;
    Py_DECREF(req->future);
    req->future = nullptr;
    Py_XDECREF(req->set_result);
    req->set_result = nullptr;
    
    req->loop_handle->push(set_fn, exc);
    delete req;
    
    PyGILState_Release(gs);
}

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
    
    // 使用基类缓存的缓冲区大小（无锁访问）
    if (size <= m_cached_buffer_size) {
        req->poolBuf = pool_acquire();
    } else {
        req->heapBuf = new char[size];
    }
    return req;
}

void IOUringBackend::complete_error_inline(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject* exc_class = g_OSError;
    PyObject* exc = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    PyObject* set_fn = req->set_exception;
    req->set_exception = nullptr;
    PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
    Py_XDECREF(r);
    Py_DECREF(set_fn);
    Py_DECREF(exc);
    delete req;
}

#endif // HAVE_IO_URING