#ifdef __APPLE__

#include "macos_gcd_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
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
// 全局缓存（线程安全）
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static LoopHandle* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    UR_DEBUG_LOG0("MacOSGCDBackend: refresh_loop_cache start");
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) {
        UR_DEBUG_LOG0("MacOSGCDBackend: refresh_loop_cache cache hit");
        return;
    }
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop = loop;
    g_cachedFutureFn = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) {
        Py_INCREF(g_cachedFutureFn);
        UR_DEBUG_LOG0("MacOSGCDBackend: refresh_loop_cache got create_future");
    } else {
        UR_DEBUG_LOG0("MacOSGCDBackend: refresh_loop_cache FAILED to get create_future");
    }
    g_cachedLoopHandle = get_or_create_loop_handle(loop);
    UR_DEBUG_LOG0("MacOSGCDBackend: refresh_loop_cache done");
}

// ════════════════════════════════════════════════════════════════════════════
// 构造函数 / 析构函数
// ════════════════════════════════════════════════════════════════════════════

MacOSGCDBackend::MacOSGCDBackend(const std::string& path, const std::string& mode) 
    : m_path(path) {
    UR_DEBUG_LOG("MacOSGCDBackend: constructor start, path=%s, mode=%s", path.c_str(), mode.c_str());
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    int flags = O_RDONLY;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        UR_DEBUG_LOG("MacOSGCDBackend: parse_mode failed: %s", e.what());
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
    
    UR_DEBUG_LOG("MacOSGCDBackend: opening file with flags=%d", flags);
    m_fd = open(path.c_str(), flags, 0644);
    if (m_fd == -1) {
        UR_DEBUG_LOG("MacOSGCDBackend: open failed, errno=%d", errno);
        throw_os_error("Failed to open file", path.c_str());
    }
    UR_DEBUG_LOG("MacOSGCDBackend: file opened, fd=%d", m_fd);
    
    // 创建 GCD 串行队列
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0);
    m_queue = dispatch_queue_create("com.ayafileio.gcd", attr);
    UR_DEBUG_LOG("MacOSGCDBackend: GCD queue created, queue=%p", (void*)m_queue);
    
    // 创建 Dispatch I/O 通道（随机访问模式，支持 seek）
    m_channel = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        m_fd,
        m_queue,
        ^(int error) {
            UR_DEBUG_LOG("MacOSGCDBackend: dispatch_io_create cleanup handler, error=%d", error);
        }
    );
    
    if (!m_channel) {
        UR_DEBUG_LOG0("MacOSGCDBackend: dispatch_io_create failed");
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Failed to create dispatch I/O channel");
    }
    UR_DEBUG_LOG("MacOSGCDBackend: dispatch_io_create success, channel=%p", (void*)m_channel);
    
    // 配置缓冲区参数
    dispatch_io_set_high_water(m_channel, m_cached_buffer_size);
    dispatch_io_set_low_water(m_channel, m_cached_buffer_size / 4);
    UR_DEBUG_LOG("MacOSGCDBackend: I/O channel configured, high_water=%zu, low_water=%zu",
                 m_cached_buffer_size, m_cached_buffer_size / 4);
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = static_cast<uint64_t>(st.st_size);
            UR_DEBUG_LOG("MacOSGCDBackend: append mode, filePos=%llu", (unsigned long long)m_filePos);
        }
    }
    
    UR_DEBUG_LOG("MacOSGCDBackend: constructor done, this=%p", (void*)this);
}

MacOSGCDBackend::MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd) 
    : m_path("<fd>") {
    
    UR_DEBUG_LOG("MacOSGCDBackend: fd constructor start, fd=%d", fd);
    
    m_fd = fd;
    m_owns_fd = owns_fd;
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument& e) {
        throw py::value_error(e.what());
    }
    
    m_appendMode = mi.appendMode;
    
    // 创建 GCD 串行队列
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0);
    m_queue = dispatch_queue_create("com.ayafileio.gcd", attr);
    
    // 创建 Dispatch I/O 通道（用现有 fd）
    m_channel = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        m_fd,
        m_queue,
        ^(int error) {
            UR_DEBUG_LOG("MacOSGCDBackend: fd channel cleanup, error=%d", error);
        }
    );
    
    if (!m_channel) {
        throw std::runtime_error("Failed to create dispatch I/O channel from fd");
    }
    
    dispatch_io_set_high_water(m_channel, m_cached_buffer_size);
    dispatch_io_set_low_water(m_channel, m_cached_buffer_size / 4);
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    if (m_appendMode) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_filePos = static_cast<uint64_t>(st.st_size);
        }
    }
    
    UR_DEBUG_LOG("MacOSGCDBackend: fd constructor done, this=%p", (void*)this);
}

MacOSGCDBackend::~MacOSGCDBackend() {
    UR_DEBUG_LOG("MacOSGCDBackend: destructor start, this=%p", (void*)this);
    close_impl();
    if (m_loop_initialized) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
    UR_DEBUG_LOG0("MacOSGCDBackend: destructor done");
}

// ════════════════════════════════════════════════════════════════════════════
// 初始化
// ════════════════════════════════════════════════════════════════════════════

void MacOSGCDBackend::ensure_loop_initialized() {
    UR_DEBUG_LOG("MacOSGCDBackend::ensure_loop_initialized start, this=%p, already_init=%d",
                 (void*)this, m_loop_initialized);
    
    if (m_loop_initialized) {
        UR_DEBUG_LOG0("MacOSGCDBackend::ensure_loop_initialized already initialized, returning");
        return;
    }
    
    UR_DEBUG_LOG0("MacOSGCDBackend::ensure_loop_initialized calling get_running_loop...");
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) {
        PyErr_Clear();
        UR_DEBUG_LOG0("MacOSGCDBackend::ensure_loop_initialized NO RUNNING LOOP");
        throw std::runtime_error("No running event loop");
    }
    UR_DEBUG_LOG("MacOSGCDBackend::ensure_loop_initialized got loop=%p", (void*)loop);
    
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
    
    m_loop_initialized = true;
    UR_DEBUG_LOG("MacOSGCDBackend::ensure_loop_initialized done, this=%p", (void*)this);
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject* MacOSGCDBackend::read(int64_t size) {
    UR_DEBUG_LOG("MacOSGCDBackend::read start, this=%p, size=%lld", (void*)this, (long long)size);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("MacOSGCDBackend::read ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) {
        UR_DEBUG_LOG0("MacOSGCDBackend::read failed to create future");
        return nullptr;
    }
    UR_DEBUG_LOG0("MacOSGCDBackend::read future created");
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) {
        UR_DEBUG_LOG0("MacOSGCDBackend::read file is closed");
        Py_DECREF(future);
        return closed_future;
    }
    
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        
        struct stat st;
        if (fstat(m_fd, &st) != 0) {
            UR_DEBUG_LOG("MacOSGCDBackend::read fstat failed, errno=%d", errno);
            set_os_error("fstat failed");
            resolve_exc(future, g_OSError, errno, "fstat failed");
            return future;
        }
        
        int64_t rem = static_cast<int64_t>(st.st_size) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) {
            UR_DEBUG_LOG0("MacOSGCDBackend::read EOF");
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
    
    IORequest* req = make_req(readSize, future, ReqType::Read);
    UR_DEBUG_LOG("MacOSGCDBackend::read req=%p, offset=%llu, size=%zu",
                 (void*)req, (unsigned long long)offset, readSize);
    
    m_pending.fetch_add(1, std::memory_order_relaxed);
    
    auto self = this;
    dispatch_io_read(
        m_channel,
        offset,
        readSize,
        m_queue,
        ^(bool done, dispatch_data_t data, int error) {
            UR_DEBUG_LOG("MacOSGCDBackend::read callback: done=%d, data=%p, error=%d, req=%p",
                         done, (void*)data, error, (void*)req);
            
            if (error) {
                UR_DEBUG_LOG("MacOSGCDBackend::read callback error=%d", error);
                self->complete_error(req, static_cast<DWORD>(error));
                return;
            }
            
            if (data) {
                size_t total_bytes = dispatch_data_get_size(data);
                UR_DEBUG_LOG("MacOSGCDBackend::read callback got data, size=%zu", total_bytes);
                
                __block size_t copied = 0;
                dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t off, const void* buf, size_t len) {
                    UR_DEBUG_LOG("MacOSGCDBackend::read callback copying region: off=%zu, len=%zu", off, len);
                    memcpy(req->buf() + off, buf, len);
                    copied += len;
                    return true;
                });
                
                if (done) {
                    UR_DEBUG_LOG("MacOSGCDBackend::read callback done, total_bytes=%zu", total_bytes);
                    self->complete_ok(req, total_bytes);
                }
            } else if (done) {
                UR_DEBUG_LOG0("MacOSGCDBackend::read callback EOF");
                self->complete_ok(req, 0);
            }
        }
    );
    
    UR_DEBUG_LOG0("MacOSGCDBackend::read returning future");
    return future;
}

PyObject* MacOSGCDBackend::write(Py_buffer* view) {
    UR_DEBUG_LOG("MacOSGCDBackend::write start, this=%p, size=%zd", (void*)this, view->len);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("MacOSGCDBackend::write ensure_loop failed: %s", e.what());
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
    UR_DEBUG_LOG("MacOSGCDBackend::write req=%p, offset=%llu, size=%zu",
                 (void*)req, (unsigned long long)offset, size);
    
    m_pending.fetch_add(1, std::memory_order_relaxed);
    
    dispatch_data_t write_data = dispatch_data_create(
        req->buf(), size, m_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    
    auto self = this;
    dispatch_io_write(
        m_channel,
        offset,
        write_data,
        m_queue,
        ^(bool done, dispatch_data_t data, int error) {
            UR_DEBUG_LOG("MacOSGCDBackend::write callback: done=%d, error=%d, req=%p",
                         done, error, (void*)req);
            
            if (error) {
                UR_DEBUG_LOG("MacOSGCDBackend::write callback error=%d", error);
                self->complete_error(req, static_cast<DWORD>(error));
                return;
            }
            
            if (done) {
                UR_DEBUG_LOG("MacOSGCDBackend::write callback done, bytes=%zu", size);
                self->complete_ok(req, size);
            }
        }
    );
    
    return future;
}

PyObject* MacOSGCDBackend::seek(int64_t offset, int whence) {
    UR_DEBUG_LOG("MacOSGCDBackend::seek start, offset=%lld, whence=%d", (long long)offset, whence);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    // ✅ 直接在主线程执行 lseek，不使用 GCD barrier
    off_t new_pos;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        
        if (whence == 0) {
            new_pos = lseek(m_fd, offset, SEEK_SET);
        } else if (whence == 1) {
            new_pos = lseek(m_fd, m_filePos + offset, SEEK_SET);
        } else if (whence == 2) {
            struct stat st;
            if (fstat(m_fd, &st) != 0) {
                resolve_exc(future, g_OSError, errno, "fstat failed");
                return future;
            }
            new_pos = lseek(m_fd, st.st_size + offset, SEEK_SET);
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }
        
        if (new_pos == -1) {
            resolve_exc(future, g_OSError, errno, "lseek failed");
            return future;
        }
        
        m_filePos = static_cast<uint64_t>(new_pos);
        UR_DEBUG_LOG("MacOSGCDBackend::seek new pos=%lld", (long long)new_pos);
    }
    
    PyObject* pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos);
    Py_DECREF(pos);
    
    return future;
}

PyObject* MacOSGCDBackend::flush() {
    UR_DEBUG_LOG0("MacOSGCDBackend::flush start");
    
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
    
    // ✅ 同步执行 fsync，不走 GCD barrier
    if (fsync(m_fd) != 0) {
        UR_DEBUG_LOG("MacOSGCDBackend::flush fsync failed, errno=%d", errno);
        set_os_error("fsync failed");
        resolve_exc(future, g_OSError, errno, "fsync failed");
        return future;
    }
    
    UR_DEBUG_LOG0("MacOSGCDBackend::flush done");
    resolve_ok(future, Py_None);
    return future;
}

PyObject* MacOSGCDBackend::close() {
    UR_DEBUG_LOG("MacOSGCDBackend::close start, this=%p, initialized=%d",
                 (void*)this, m_loop_initialized);
    
    if (!m_loop_initialized) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close not initialized, closing directly");
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

void MacOSGCDBackend::close_impl() {
    UR_DEBUG_LOG("MacOSGCDBackend::close_impl start, this=%p, fd=%d", (void*)this, m_fd);
    
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl already closed");
        return;
    }
    
    int elapsed = 0;
    int wait_time = 1;
    while (elapsed < static_cast<int>(m_cached_close_timeout_ms) &&
           m_pending.load(std::memory_order_acquire) > 0) {
        UR_DEBUG_LOG("MacOSGCDBackend::close_impl waiting for pending I/O, elapsed=%d, pending=%ld",
                     elapsed, m_pending.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));
        elapsed += wait_time;
        wait_time = std::min(wait_time * 2, 32);
    }
    
    if (m_pending.load() > 0) {
        UR_DEBUG_LOG("MacOSGCDBackend::close_impl timeout waiting for pending I/O, forcing close. pending=%ld",
                     m_pending.load());
    }
    
    if (m_channel) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl closing dispatch channel");
        dispatch_io_close(m_channel, DISPATCH_IO_STOP);
        m_channel = nullptr;
    }
    
    if (m_queue) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl releasing queue");
        dispatch_release(m_queue);
        m_queue = nullptr;
    }

    if (m_owns_fd && m_fd != -1) {
        ::close(m_fd);
    }
    
    m_fd = -1;
    
    UR_DEBUG_LOG0("MacOSGCDBackend::close_impl done");
}

// ════════════════════════════════════════════════════════════════════════════
// 完成处理
// ════════════════════════════════════════════════════════════════════════════

void MacOSGCDBackend::complete_ok(IORequest* req, size_t bytes) {
    UR_DEBUG_LOG("MacOSGCDBackend::complete_ok req=%p, bytes=%zu", (void*)req, bytes);
    m_pending.fetch_sub(1, std::memory_order_release);
    
    // 在 GCD 队列的回调中，我们已经持有 GIL？不，GCD 队列不一定持有 GIL
    // 所以需要安全地获取 GIL
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
    
    if (set_fn && val) {
        req->loop_handle->push(set_fn, val);
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
    }
    delete req;
    
    PyGILState_Release(gs);
    UR_DEBUG_LOG0("MacOSGCDBackend::complete_ok done");
}

void MacOSGCDBackend::complete_error(IORequest* req, DWORD err) {
    UR_DEBUG_LOG("MacOSGCDBackend::complete_error req=%p, err=%u", (void*)req, err);
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
    
    if (set_fn && exc) {
        req->loop_handle->push(set_fn, exc);
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(exc);
    }
    delete req;
    
    PyGILState_Release(gs);
    UR_DEBUG_LOG0("MacOSGCDBackend::complete_error done");
}

// ════════════════════════════════════════════════════════════════════════════
// 辅助方法
// ════════════════════════════════════════════════════════════════════════════

IORequest* MacOSGCDBackend::make_req(size_t size, PyObject* future, ReqType type) {
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
    return req;
}

void MacOSGCDBackend::complete_error_inline(IORequest* req, DWORD err) {
    UR_DEBUG_LOG("MacOSGCDBackend::complete_error_inline req=%p, err=%u", (void*)req, err);
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    
    PyObject* exc_class = map_posix_error(static_cast<int>(err));
    PyObject* exc = PyObject_CallFunction(exc_class, "is", static_cast<int>(err), "I/O operation failed");
    PyObject* set_fn = req->set_exception;
    req->set_exception = nullptr;
    if (set_fn && exc) {
        PyObject* r = PyObject_CallFunctionObjArgs(set_fn, exc, nullptr);
        Py_XDECREF(r);
    }
    Py_XDECREF(set_fn);
    Py_XDECREF(exc);
    delete req;
}

#endif // __APPLE__