#ifdef HAVE_IO_URING

#include "io_uring_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
#include "../uring_pool.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include "../debug_log.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>

// ════════════════════════════════════════════════════════════════════════════
// 全局缓存
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static LoopHandle* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) return;
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop = loop;
    g_cachedFutureFn = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) Py_INCREF(g_cachedFutureFn);
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
}

// ════════════════════════════════════════════════════════════════════════════
// 构造函数 / 析构函数
// ════════════════════════════════════════════════════════════════════════════

IOUringBackend::IOUringBackend(const std::string& path, const std::string& mode) {
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
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    // ── 打开文件：优先异步 (IORING_OP_OPENAT) ──
    m_fd = -1;
    char* path_copy = strdup(path.c_str());

    try {
        ensure_loop_initialized();
        
        if (m_uring && path_copy) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&m_uring->ring);
            if (sqe) {
                io_uring_prep_openat(sqe, AT_FDCWD, path_copy, flags, 0644);
                io_uring_sqe_set_data(sqe, path_copy);  // char* 标记这是 OPENAT
                
                int ret = io_uring_submit(&m_uring->ring);
                if (ret >= 0) {
                    struct io_uring_cqe* cqe = nullptr;
                    ret = io_uring_wait_cqe(&m_uring->ring, &cqe);
                    if (ret >= 0 && cqe) {
                        m_fd = cqe->res;
                        io_uring_cqe_seen(&m_uring->ring, cqe);
                        if (m_fd >= 0) {
                            UR_LOG("IOUringBackend: async open success via io_uring, fd=%d", m_fd);
                        }
                    }
                }
            }
        }
    } catch (...) {
        // ensure_loop_initialized 可能抛异常
    }
    
    free(path_copy);
    
    // ── 异步打开失败 → 回退同步 ──
    if (m_fd < 0) {
        m_fd = open(path.c_str(), flags, 0644);
        UR_LOG("IOUringBackend: sync open fallback, fd=%d", m_fd);
    }
    
    if (m_fd == -1) {
        UR_LOG("IOUringBackend open failed: path=%s, mode=%s, errno=%d", 
               path.c_str(), mode.c_str(), errno);
        throw_os_error("Failed to open file", path.c_str());
    }
    
    UR_LOG("IOUringBackend created: this=%p, fd=%d, path=%s, mode=%s", 
           (void*)this, m_fd, path.c_str(), mode.c_str());
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = static_cast<uint64_t>(st.st_size);
        }
    }
}

IOUringBackend::~IOUringBackend() {
    UR_LOG("IOUringBackend destructor: this=%p, fd=%d", (void*)this, m_fd);
    close_impl();
    if (m_loop_initialized) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// 初始化
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::ensure_loop_initialized() {
    if (m_loop_initialized) return;

    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) {
        PyErr_Clear();
        throw std::runtime_error("No running event loop");
    }

    std::lock_guard<std::mutex> lk(m_loop_init_mtx);
    if (m_loop_initialized) {
        Py_DECREF(loop);
        return;
    }

    refresh_loop_cache(loop);
    m_loop = loop;
    Py_INCREF(m_loop);
    m_create_future = g_cachedFutureFn;
    Py_INCREF(m_create_future);
    m_loop_handle = g_cachedLoopHandle;

    auto& cfg = ayafileio::config();
    m_uring = uring_manager().acquire(
        m_loop,
        &IOUringBackend::reaper_loop_entry,
        cfg.io_uring_queue_depth(),
        cfg.io_uring_flags(),
        cfg.io_uring_sqpoll()
    );

    if (!m_uring) {
        UR_LOG("ensure_loop_initialized: failed to acquire uring instance");
        throw std::runtime_error("Failed to acquire io_uring instance");
    }

    uring_manager().start_reaper(m_uring);
    
    UR_LOG("ensure_loop_initialized: acquired uring instance=%p, queue_depth=%u",
           (void*)m_uring.get(), cfg.io_uring_queue_depth());

    m_loop_initialized = true;
    UR_LOG("ensure_loop_initialized done: this=%p", (void*)this);
}

// ════════════════════════════════════════════════════════════════════════════
// Reaper 循环
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::reaper_loop_entry(UringInstance* inst) {
    UR_LOG("reaper_loop_entry start: inst=%p, ring_fd=%d, event_fd=%d", 
           (void*)inst, inst->ring.ring_fd, inst->event_fd);
    
    struct io_uring_cqe* cqe = nullptr;
    int idle_count = 0;
    
    while (!inst->reaper_stop.load(std::memory_order_acquire)) {
        int ret = io_uring_wait_cqe(&inst->ring, &cqe);

        if (ret == -EEXIST) { continue; }
        if (ret == -EINTR) { continue; }
        if (ret < 0) {
            UR_LOG("reaper_loop: io_uring_wait_cqe error ret=%d, exiting", ret);
            break;
        }
        
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&inst->ring, head, cqe) {
            void* data = io_uring_cqe_get_data(cqe);
            IORequest* req = static_cast<IORequest*>(data);
            
            // 关键：检查 user_data 是否指向堆内存而不是 IORequest*
            // IORequest 对象的地址一定大于某个阈值（堆/栈），而 char* 的地址通常是堆分配的路径字符串
            // 简单判断：如果 data 不为空且看起来像有效指针，尝试当作 IORequest 处理
            if (req && data != nullptr && cqe->user_data != 0) {
                // 进一步检查：IORequest 的第一个成员是 OVERLAPPED (Windows) 或直接是 file 指针
                // 最简单的方法：检查是否在构造函数的 path_copy 范围内（不可行）
                // 改用：所有通过 submit_io 提交的 SQE，user_data 都是 IORequest*
                // 只有构造函数的 OPENAT 用 char* 作为 user_data
                // 
                // 判断方法：如果 cqe->res 返回的是 fd（>= 0 且很小），则可能是 OPENAT
                // 但这不可靠。最安全的方式：让 OPENAT 的 SQE 在构造函数中 wait_cqe 自己处理，
                // reaper 不应该看到 OPENAT 的 CQE —— 因为构造函数在 reaper 启动之前就 wait 了
                
                auto* file = static_cast<IOUringBackend*>(req->file);
                if (file && file->m_fd >= 0) {
                    UR_LOG("reaper got cqe: req=%p, file=%p, fd=%d, res=%d", 
                           (void*)req, (void*)file, file->m_fd, cqe->res);
                    if (cqe->res >= 0) {
                        file->complete_ok(req, static_cast<size_t>(cqe->res));
                    } else {
                        file->complete_error(req, static_cast<DWORD>(-cqe->res));
                    }
                } else {
                    // 可能是 OPENAT 的 CQE 或其他
                    UR_LOG("reaper got non-IORequest cqe (likely OPENAT char*), skipping");
                }
            } else {
                UR_LOG("reaper got eventfd wakeup or stale CQE");
                if (!inst->reaper_stop.load(std::memory_order_acquire)) {
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, inst->event_fd, POLLIN);
                        io_uring_sqe_set_data(sqe, nullptr);
                        ret = io_uring_submit(&inst->ring);
                        if (ret < 0) {
                            UR_LOG("reaper_loop: failed to resubmit eventfd poll, ret=%d", ret);
                        }
                    }
                }
            }
            count++;
        }
        
        if (count > 0) {
            io_uring_cq_advance(&inst->ring, count);
            UR_LOG("reaper processed %u cqes", count);
        } else {
            idle_count++;
            if (idle_count % 1000 == 0) {
                UR_LOG("reaper idle, count=%d", idle_count);
            }
        }
    }
    
    UR_LOG("reaper_loop_entry exit: inst=%p", (void*)inst);
}

// ════════════════════════════════════════════════════════════════════════════
// I/O 提交
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::submit_io(IORequest* req, int op, int fd, 
                                const void* buf, size_t len, off_t offset) {
    if (!m_uring) {
        complete_error(req, EINVAL);
        return;
    }
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_uring->ring);
    if (!sqe) {
        complete_error(req, EBUSY);
        return;
    }
    
    void* writeable_buf = const_cast<void*>(buf);
    
    if (op == IORING_OP_READ)
        io_uring_prep_read(sqe, fd, writeable_buf, static_cast<unsigned>(len), offset);
    else if (op == IORING_OP_WRITE)
        io_uring_prep_write(sqe, fd, writeable_buf, static_cast<unsigned>(len), offset);
    else if (op == IORING_OP_FSYNC)
        io_uring_prep_fsync(sqe, fd, 0);
    else {
        complete_error(req, EINVAL);
        return;
    }
    
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&m_uring->ring);
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject* IOUringBackend::read(int64_t size) {
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) { Py_DECREF(future); return closed_future; }
    
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            set_os_error("fstat failed");
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        int64_t rem = static_cast<int64_t>(st.st_size) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) { resolve_bytes(future, nullptr, 0); return future; }
        readSize = (size < 0) ? static_cast<size_t>(rem) 
                 : std::min(static_cast<size_t>(size), static_cast<size_t>(rem));
        if (readSize == 0) { resolve_bytes(future, nullptr, 0); return future; }
        offset = m_filePos;
        m_filePos += readSize;
    }
    
    IORequest* req = make_req(readSize, future, ReqType::Read);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_READ, m_fd, req->buf(), readSize, static_cast<off_t>(offset));
    return future;
}

PyObject* IOUringBackend::write(Py_buffer* view) {
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    size_t size = static_cast<size_t>(view->len);
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) { Py_DECREF(future); return closed_future; }
    
    if (size == 0) {
        PyObject* z = PyLong_FromLong(0);
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
    
    IORequest* req = make_req(size, future, ReqType::Write);
    std::memcpy(req->buf(), view->buf, size);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_WRITE, m_fd, req->buf(), size, static_cast<off_t>(offset));
    return future;
}

PyObject* IOUringBackend::seek(int64_t offset, int whence) {
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (whence == 0) m_filePos = static_cast<uint64_t>(offset);
        else if (whence == 1) m_filePos = static_cast<uint64_t>(static_cast<int64_t>(m_filePos) + offset);
        else if (whence == 2) {
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
    
    PyObject* pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject* IOUringBackend::flush() {
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
    
    IORequest* req = make_req(0, future, ReqType::Other);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_FSYNC, m_fd, nullptr, 0, 0);
    return future;
}

PyObject* IOUringBackend::close() {
    if (!m_loop_initialized) {
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) { PyErr_Clear(); close_impl(); Py_RETURN_NONE; }
        PyObject* future = create_resolved_future(loop, Py_None);
        Py_DECREF(loop);
        if (!future) { close_impl(); return nullptr; }
        close_impl();
        return future;
    }
    
    ensure_loop_initialized();
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    close_impl();
    resolve_ok(future, Py_None);
    return future;
}

void IOUringBackend::close_impl() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;
    
    int elapsed = 0, wait_time = 1;
    while (elapsed < static_cast<int>(m_cached_close_timeout_ms) && 
           m_pending.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));
        elapsed += wait_time;
        wait_time = std::min(wait_time * 2, 32);
    }
    
    if (m_uring) m_uring.reset();
    if (m_fd != -1) { ::close(m_fd); m_fd = -1; }
}

// ════════════════════════════════════════════════════════════════════════════
// 完成处理
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::complete_ok(IORequest* req, size_t bytes) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* val = nullptr;
    if (req->type == ReqType::Read)
        val = PyBytes_FromStringAndSize(req->buf(), static_cast<Py_ssize_t>(bytes));
    else if (req->type == ReqType::Write)
        val = PyLong_FromSsize_t(static_cast<Py_ssize_t>(bytes));
    else { val = Py_None; Py_INCREF(Py_None); }
    
    PyObject* set_fn = req->set_result; req->set_result = nullptr;
    Py_DECREF(req->future); req->future = nullptr;
    Py_XDECREF(req->set_exception); req->set_exception = nullptr;
    
    if (set_fn && val) req->loop_handle->push(set_fn, val);
    else { Py_XDECREF(set_fn); Py_XDECREF(val); }
    delete req;
    PyGILState_Release(gs);
}

void IOUringBackend::complete_error(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_release);
    PyGILState_STATE gs = PyGILState_Ensure();
    
    PyObject* exc_class = map_posix_error(static_cast<int>(err));
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
// 辅助方法
// ════════════════════════════════════════════════════════════════════════════

IORequest* IOUringBackend::make_req(size_t size, PyObject* future, ReqType type) {
    auto* req = new IORequest();
    req->file = this;
    req->loop_handle = m_loop_handle;
    req->future = future; Py_INCREF(future);
    req->set_result = PyObject_GetAttr(future, g_str_set_result);
    req->set_exception = PyObject_GetAttr(future, g_str_set_exception);
    req->reqSize = size;
    req->type = type;
    if (size <= m_cached_buffer_size) req->poolBuf = pool_acquire_with_size(size);
    else req->heapBuf = new char[size];
    return req;
}

void IOUringBackend::complete_error_inline(IORequest* req, DWORD err) {
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    PyObject* exc_class = map_posix_error(static_cast<int>(err));
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");
    PyObject* set_fn = req->set_exception; req->set_exception = nullptr;
    if (set_fn && exc) { PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr); Py_XDECREF(r); }
    Py_XDECREF(set_fn); Py_DECREF(exc);
    delete req;
}

#endif // HAVE_IO_URING