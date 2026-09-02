#ifdef HAVE_IO_URING

#include "io_uring_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
#include "../uring_pool.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include "utils/debug_log.hpp"
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
static ResultBatcher* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) return;
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop = loop;
    g_cachedFutureFn = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) Py_INCREF(g_cachedFutureFn);
    g_cachedLoopHandle = get_or_create_batcher(loop);
}

// ════════════════════════════════════════════════════════════════════════════
// 构造函数：独立 ring OPENAT，零竞争
// ════════════════════════════════════════════════════════════════════════════

IOUringBackend::IOUringBackend(const std::string& path, const std::string& mode) {
    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        throw py::value_error(e.what());
    }

    if (mi.hasW)      flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mi.hasA) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mi.hasX) flags = O_WRONLY | O_CREAT | O_EXCL;
    if (mi.plus)      flags = (flags & ~O_ACCMODE) | O_RDWR;

    m_appendMode = mi.appendMode;
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    // ── 第一步：用独立的本地 ring 做 OPENAT ──
    m_fd = -1;
    {
        struct io_uring local_ring;
        if (io_uring_queue_init(8, &local_ring, 0) == 0) {
            char* path_copy = strdup(path.c_str());
            if (path_copy) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(&local_ring);
                if (sqe) {
                    io_uring_prep_openat(sqe, AT_FDCWD, path_copy, flags, 0644);
                    io_uring_submit(&local_ring);
                    
                    struct io_uring_cqe* cqe = nullptr;
                    int ret = io_uring_wait_cqe(&local_ring, &cqe);
                    if (ret >= 0 && cqe) {
                        m_fd = cqe->res;
                        io_uring_cqe_seen(&local_ring, cqe);
                    }
                }
                free(path_copy);
            }
            io_uring_queue_exit(&local_ring);
        }
    }
    
    // ── 第二步：OPENAT 失败 → 回退同步 ──
    if (m_fd < 0) {
        m_fd = open(path.c_str(), flags, 0644);
    }
    
    if (m_fd == -1) {
        throw_os_error("Failed to open file", path.c_str());
    }
    
    UR_LOG("IOUringBackend created: this=%p, fd=%d, path=%s", 
           (void*)this, m_fd, path.c_str());
    
    // ── 第三步：初始化共享 io_uring（供 read/write 使用）──
    // 这里延迟到第一次真正 I/O 时才初始化，避免 OPENAT 和 reaper 竞争
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;

    {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_cachedFileSize = static_cast<uint64_t>(st.st_size);
            if (m_appendMode) {
                m_filePos = m_cachedFileSize;
            }
        }
    }
}

IOUringBackend::IOUringBackend(int fd, const std::string& mode, bool owns_fd) {
    m_fd = fd;
    m_owns_fd = owns_fd;
    
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        throw py::value_error(e.what());
    }
    
    m_appendMode = mi.appendMode;
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;

    {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_cachedFileSize = static_cast<uint64_t>(st.st_size);
            if (m_appendMode) {
                m_filePos = m_cachedFileSize;
            }
        }
    }

    UR_LOG("IOUringBackend created from fd: this=%p, fd=%d, owns_fd=%d",
           (void*)this, m_fd, owns_fd);
}

IOUringBackend::~IOUringBackend() {
    close_impl();
    if (m_loop_initialized) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// 延迟初始化：第一次 read/write 时才创建共享 ring + reaper
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
    m_batcher = g_cachedLoopHandle;

    auto& cfg = ayafileio::config();
    m_uring = uring_manager().acquire(
        m_loop,
        &IOUringBackend::reaper_loop_entry,
        cfg.io_uring_queue_depth(),
        cfg.io_uring_flags(),
        cfg.io_uring_sqpoll()
    );

    if (!m_uring) {
        throw std::runtime_error("Failed to acquire io_uring instance");
    }

    uring_manager().start_reaper(m_uring);
    m_loop_initialized = true;
}

// ════════════════════════════════════════════════════════════════════════════
// Reaper 循环 — 只处理 IORequest*，不再遇到 char*
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::reaper_loop_entry(UringInstance* inst) {
    struct io_uring_cqe* cqe = nullptr;
    
    while (!inst->reaper_stop.load(std::memory_order_acquire)) {
        int ret = io_uring_wait_cqe(&inst->ring, &cqe);
        if (ret == -EEXIST || ret == -EINTR) continue;
        if (ret < 0) break;

        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&inst->ring, head, cqe) {
            IORequest* req = static_cast<IORequest*>(io_uring_cqe_get_data(cqe));
            if (req) {
                auto* file = static_cast<IOUringBackend*>(req->file);
                if (cqe->res >= 0) {
                    file->complete_ok(req, static_cast<size_t>(cqe->res));
                } else {
                    file->complete_error(req, static_cast<DWORD>(-cqe->res));
                }
            } else {
                // eventfd / timeout wakeup — re-submit poll so shutdown can wake us
                bool stop = inst->reaper_stop.load(std::memory_order_acquire);
                if (!stop) {
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, inst->event_fd, POLLIN);
                        io_uring_sqe_set_data(sqe, nullptr);
                    } else {
                        // Ring full: fallback to a 1 s timeout so io_uring_wait_cqe
                        // cannot block forever even if the poll SQE was lost.
                        struct __kernel_timespec ts = {1, 0};
                        sqe = io_uring_get_sqe(&inst->ring);
                        if (sqe) {
                            io_uring_prep_timeout(sqe, &ts, 0, 0);
                            io_uring_sqe_set_data(sqe, nullptr);
                        }
                    }
                    io_uring_submit(&inst->ring);
                }
            }
            count++;
        }

        if (count > 0) io_uring_cq_advance(&inst->ring, count);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// I/O 提交
// ════════════════════════════════════════════════════════════════════════════

void IOUringBackend::submit_io(IORequest* req, int op, int fd,
                                std::span<const std::byte> data, off_t offset) {
    if (!m_uring) { complete_error(req, EINVAL); return; }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_uring->ring);
    if (!sqe) { complete_error(req, EBUSY); return; }

    void* wbuf = const_cast<std::byte*>(data.data());
    auto len = static_cast<unsigned>(data.size());
    if (op == IORING_OP_READ) [[likely]]
        io_uring_prep_read(sqe, fd, wbuf, len, offset);
    else if (op == IORING_OP_WRITE) [[likely]]
        io_uring_prep_write(sqe, fd, wbuf, len, offset);
    else if (op == IORING_OP_FSYNC)
        io_uring_prep_fsync(sqe, fd, 0);
    else { complete_error(req, EINVAL); return; }

    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&m_uring->ring);
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject* IOUringBackend::read(int64_t size) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }

    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) [[unlikely]] return nullptr;

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) [[unlikely]] { Py_DECREF(future); return closed_future; }

    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) [[unlikely]] { resolve_bytes(future, nullptr, 0); return future; }
        readSize = (size < 0) ? static_cast<size_t>(rem)
                 : std::min(static_cast<size_t>(size), static_cast<size_t>(rem));
        if (readSize == 0) [[unlikely]] { resolve_bytes(future, nullptr, 0); return future; }
        offset = m_filePos;
        m_filePos += readSize;
    }

    IORequest* req = make_req(readSize, future, ReqType::Read);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_READ, m_fd,
              std::as_bytes(std::span{req->buf(), readSize}), static_cast<off_t>(offset));
    return future;
}

PyObject* IOUringBackend::read_at(int64_t offset, int64_t size) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }

    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) [[unlikely]] return nullptr;

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) [[unlikely]] { Py_DECREF(future); return closed_future; }

    if (offset < 0) [[unlikely]] {
        resolve_exc(future, g_ValueError, 0, "negative offset not allowed");
        return future;
    }

    size_t readSize;
    {
        // m_posMtx 仅为与 write/truncate 的 cachedFileSize 更新保持一致；
        // 不读取也不修改 m_filePos —— 位置读与并发 read()/seek() 无竞争
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - offset;
        if (rem <= 0) [[unlikely]] { resolve_bytes(future, nullptr, 0); return future; }
        readSize = (size < 0) ? static_cast<size_t>(rem)
                 : std::min(static_cast<size_t>(size), static_cast<size_t>(rem));
        if (readSize == 0) [[unlikely]] { resolve_bytes(future, nullptr, 0); return future; }
    }

    IORequest* req = make_req(readSize, future, ReqType::Read);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    // io_uring_prep_read 显式偏移，不动文件指针
    submit_io(req, IORING_OP_READ, m_fd,
              std::as_bytes(std::span{req->buf(), readSize}), static_cast<off_t>(offset));
    return future;
}

PyObject* IOUringBackend::write(Py_buffer* view) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }

    size_t size = static_cast<size_t>(view->len);
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) [[unlikely]] return nullptr;

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) [[unlikely]] { Py_DECREF(future); return closed_future; }

    if (size == 0) [[unlikely]] {
        PyObject* z = PyLong_FromLong(0);
        resolve_ok(future, z); Py_DECREF(z);
        return future;
    }
    
    uint64_t offset;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (m_appendMode) {
            offset = m_cachedFileSize;
        } else {
            offset = m_filePos;
        }
        m_filePos = offset + size;
        if (m_filePos > m_cachedFileSize) {
            m_cachedFileSize = m_filePos;  // optimistic: assume write succeeds
        }
    }
    
    IORequest* req = make_req(size, future, ReqType::Write);
    std::memcpy(req->buf(), view->buf, size);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_WRITE, m_fd,
              std::as_bytes(std::span{req->buf(), size}), static_cast<off_t>(offset));
    return future;
}

PyObject* IOUringBackend::seek(int64_t offset, int whence) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (whence == 0) m_filePos = static_cast<uint64_t>(offset);
        else if (whence == 1) m_filePos = static_cast<uint64_t>(static_cast<int64_t>(m_filePos) + offset);
        else if (whence == 2) {
            m_filePos = static_cast<uint64_t>(static_cast<int64_t>(m_cachedFileSize) + offset);
        } else { resolve_exc(future, g_ValueError, 0, "Invalid whence value"); return future; }
    }
    
    PyObject* pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject* IOUringBackend::flush() {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
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
    submit_io(req, IORING_OP_FSYNC, m_fd, std::span<const std::byte>{}, 0);
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

    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(m_cached_close_timeout_ms);

    while (true) {
        long pending = m_pending.load(std::memory_order_acquire);
        if (pending == 0) break;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remain.count() == 0) break;
        Py_BEGIN_ALLOW_THREADS
        m_close_wake.try_acquire_for(std::chrono::milliseconds(remain.count()));
        Py_END_ALLOW_THREADS
    }
    
    if (m_uring) m_uring.reset();
    if (m_owns_fd && m_fd != -1) {
        ::close(m_fd);
    }
}

PyObject* IOUringBackend::tell() {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    uint64_t pos;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        pos = m_filePos;
    }
    
    PyObject* py_pos = PyLong_FromUnsignedLongLong(pos);
    resolve_ok(future, py_pos);
    Py_DECREF(py_pos);
    return future;
}

PyObject* IOUringBackend::truncate(int64_t size) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (size < 0) {
        resolve_exc(future, g_ValueError, 0, "negative size not allowed");
        return future;
    }
    
    if (ftruncate(m_fd, static_cast<off_t>(size)) != 0) {
        resolve_exc(future, g_OSError, errno, "truncate failed");
        return future;
    }

    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (static_cast<uint64_t>(size) > m_cachedFileSize) {
            m_cachedFileSize = static_cast<uint64_t>(size);  // extending
        } else if (static_cast<uint64_t>(size) < m_cachedFileSize) {
            m_cachedFileSize = static_cast<uint64_t>(size);  // shrinking
        }
        if (static_cast<uint64_t>(size) < m_filePos) {
            m_filePos = static_cast<uint64_t>(size);
        }
    }
    
    resolve_ok(future, Py_None);
    return future;
}

PyObject* IOUringBackend::readinto(PyObject* buf) {
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) { Py_DECREF(future); return closed_future; }
    
    // 获取用户缓冲区
    Py_buffer view;
    if (PyObject_GetBuffer(buf, &view, PyBUF_WRITABLE) < 0) {
        resolve_exc(future, g_ValueError, 0, "readinto() requires a writable buffer");
        return future;
    }
    
    if (view.len == 0) {
        PyBuffer_Release(&view);
        PyObject* z = PyLong_FromLong(0);
        resolve_ok(future, z); Py_DECREF(z);
        return future;
    }
    
    // 计算偏移和读取大小
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) {
            PyBuffer_Release(&view);
            PyObject* z = PyLong_FromLong(0);
            resolve_ok(future, z); Py_DECREF(z);
            return future;
        }
        readSize = std::min(static_cast<size_t>(view.len), static_cast<size_t>(rem));
        offset = m_filePos;
        m_filePos += readSize;
    }

    IORequest* req = make_req_readinto(buf, &view, readSize, future);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(req, IORING_OP_READ, m_fd,
              std::as_bytes(std::span{
                  static_cast<const char*>(view.buf), readSize}),
              static_cast<off_t>(offset));
    return future;
}

#endif // HAVE_IO_URING