#include "thread_io_backend.hpp"
#include <algorithm>
#include <cstring>
#include <chrono>
#include "../globals.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include "utils/debug_log.hpp"
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace ayafileio {

#ifdef _WIN32
namespace {
// CRT 的 invalid-parameter 默认 handler 是 _invoke_watson → __fastfail（进程直接
// 0xC0000409）。_get_osfhandle()/_close() 对**已被关掉**的 fd 正好会走这条校验，
// 而 fileno() 暴露的描述符允许调用方自己关掉。换成只返回的 handler 后这些调用
// 退化成返回 -1 / errno=EBADF。thread_local 版本，不影响其它线程。
void __cdecl ignore_invalid_parameter(const wchar_t *, const wchar_t *,
                                      const wchar_t *, unsigned int, uintptr_t) {}
} // namespace
#endif

// ════════════════════════════════════════════════════════════════════════════
// 全局缓存（线程安全）
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static ResultBatcher* g_cachedLoopHandle = nullptr;
static std::mutex  g_cacheMtx;

static void refresh_loop_cache(PyObject* loop) {
    UR_DEBUG_LOG0("ThreadIOBackend: refresh_loop_cache start");
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (loop == g_cachedLoop) {
        UR_DEBUG_LOG0("ThreadIOBackend: refresh_loop_cache cache hit");
        return;
    }
    Py_XDECREF(g_cachedFutureFn);
    g_cachedLoop       = loop;
    g_cachedFutureFn   = PyObject_GetAttr(loop, g_str_create_future);
    if (g_cachedFutureFn) {
        Py_INCREF(g_cachedFutureFn);
        UR_DEBUG_LOG0("ThreadIOBackend: refresh_loop_cache got create_future");
    } else {
        UR_DEBUG_LOG0("ThreadIOBackend: refresh_loop_cache FAILED to get create_future");
    }
    g_cachedLoopHandle = get_or_create_batcher(loop);
    UR_DEBUG_LOG0("ThreadIOBackend: refresh_loop_cache done");
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

    int flags = kOpenRead;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        UR_DEBUG_LOG("ThreadIOBackend: parse_mode failed: %s", e.what());
        throw py::value_error(e.what());
    }
    bool appendMode = mi.appendMode;

    if (mi.hasW)      flags = kOpenWrite | kOpenCreate | kOpenTruncate;
    else if (mi.hasA) flags = kOpenWrite | kOpenCreate | kOpenAppend;
    else if (mi.hasX) flags = kOpenWrite | kOpenCreate | kOpenExclusive;
    if (mi.plus)      flags = kOpenRead | kOpenWrite
                            | (flags & ~(kOpenRead | kOpenWrite));

    m_appendMode = appendMode;

    UR_DEBUG_LOG("ThreadIOBackend: opening file with flags=%d", flags);
    {
        GilRelease gr;  // 阻塞系统调用：释放 GIL（见 globals.hpp GilRelease）
        m_fd = file_open(path.c_str(), flags);
    }
    if (m_fd == kFileInvalid) {
        const int err = native_file_last_error();
        UR_DEBUG_LOG("ThreadIOBackend: open failed, native error=%d", err);
        PyObject *exc = make_native_os_error(err, "Failed to open file", path.c_str());
        if (!exc) throw py::python_error();
        PyErr_SetObject(reinterpret_cast<PyObject *>(Py_TYPE(exc)), exc);
        Py_DECREF(exc);
        throw py::python_error();
    }
    UR_DEBUG_LOG("ThreadIOBackend: file opened, handle=%lld",
                 (long long)m_fd);

    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    {
        unsigned long long size = 0;
        if (file_size(m_fd, &size)) {
            m_cachedFileSize = static_cast<uint64_t>(size);
            if (m_appendMode) {
                m_filePos = m_cachedFileSize;
                UR_DEBUG_LOG("ThreadIOBackend: append mode, filePos=%llu", (unsigned long long)m_filePos);
            }
        }
    }
    
    UR_DEBUG_LOG("ThreadIOBackend: constructor done, this=%p", (void*)this);
}

ThreadIOBackend::ThreadIOBackend(int fd, const std::string& mode, bool owns_fd) {
#ifdef _WIN32
    // Windows：fd 里的句柄不一定是 FILE_FLAG_OVERLAPPED 打开的，用它做定位 I/O
    // 会**静默忽略偏移**（多 worker 并发读互相踩）。所以这条路径在 Windows 上
    // 不实现——file_handle.cpp 会把 wrap_file 固定交给 IOCP 后端。
    (void)fd; (void)mode; (void)owns_fd;
    throw py::value_error(
        "ThreadIOBackend does not support wrapping an existing fd on Windows; "
        "use backend=auto (IOCP) for wrap_file()");
#else
    m_fd = fd;
    m_owns_fd = owns_fd;
    
    auto& cfg = ayafileio::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    
    m_num_workers = cfg.io_worker_count();
    if (m_num_workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 1;
        m_num_workers = std::max(1u, std::min(hc * 2u, 16u));
    }
    
    ModeInfo mi = parse_mode(mode);
    m_appendMode = mi.appendMode;
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    {
        unsigned long long size = 0;
        if (file_size(m_fd, &size)) {
            m_cachedFileSize = static_cast<uint64_t>(size);
            if (m_appendMode) {
                m_filePos = m_cachedFileSize;
            }
        }
    }
#endif
}

ThreadIOBackend::~ThreadIOBackend() {
    UR_DEBUG_LOG("ThreadIOBackend: destructor start, this=%p", (void*)this);
    close_impl();
    if (m_loop_initialized.load(std::memory_order_acquire)) {
        Py_XDECREF(m_create_future);
        Py_XDECREF(m_loop);
    }
    UR_DEBUG_LOG0("ThreadIOBackend: destructor done");
}

// ════════════════════════════════════════════════════════════════════════════
// 缓存文件大小的刷新（见头文件里的说明）
// ════════════════════════════════════════════════════════════════════════════

void ThreadIOBackend::refresh_cached_size_locked() {
    unsigned long long size = 0;
    if (file_size(m_fd, &size)) {
        m_cachedFileSize = static_cast<uint64_t>(size);
    }
}

void ThreadIOBackend::refresh_cached_size() {
    std::lock_guard<std::mutex> lk(m_posMtx);
    refresh_cached_size_locked();
}

// ════════════════════════════════════════════════════════════════════════════
// 初始化方法
// ════════════════════════════════════════════════════════════════════════════

void ThreadIOBackend::ensure_loop_initialized() {
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized start, this=%p, already_init=%d",
                 (void*)this, m_loop_initialized.load());

    if (m_loop_initialized.load(std::memory_order_acquire)) {
        UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized already initialized, returning");
        return;
    }

    UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized calling get_running_loop...");
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) {
        PyErr_Clear();
        UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized NO RUNNING LOOP");
        throw std::runtime_error("No running event loop");
    }
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized got loop=%p", (void*)loop);

    {
        std::lock_guard<std::mutex> lk(m_loop_init_mtx);
        UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized inside lock");

        if (m_loop_initialized.load(std::memory_order_relaxed)) {
            UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized another thread initialized");
            Py_DECREF(loop);
            return;
        }

        UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized refreshing cache...");
        refresh_loop_cache(loop);
        m_loop = loop;
        Py_INCREF(m_loop);
        m_create_future = g_cachedFutureFn;
        Py_INCREF(m_create_future);
        m_batcher = g_cachedLoopHandle;

        m_loop_initialized.store(true, std::memory_order_release);
        UR_DEBUG_LOG0("ThreadIOBackend::ensure_loop_initialized marked as initialized");
    }

    // 使用全局共享线程池，避免 per-file open/close 反复创建线程
    GlobalThreadPool::instance().ensure_started(m_num_workers);
    UR_DEBUG_LOG("ThreadIOBackend::ensure_loop_initialized done, this=%p", (void*)this);
}

void ThreadIOBackend::enqueue_task(std::function<void()> task) {
    UR_DEBUG_LOG("ThreadIOBackend: enqueue_task, this=%p", (void*)this);
    GlobalThreadPool::instance().enqueue(std::move(task));
}

// ════════════════════════════════════════════════════════════════════════════
// 公共 I/O 接口
// ════════════════════════════════════════════════════════════════════════════

PyObject *ThreadIOBackend::read(int64_t size) {
    UR_DEBUG_LOG("ThreadIOBackend::read start, this=%p, size=%lld", (void*)this, (long long)size);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("ThreadIOBackend::read ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) {
        UR_DEBUG_LOG0("ThreadIOBackend::read failed to create future");
        return nullptr;
    }
    UR_DEBUG_LOG0("ThreadIOBackend::read future created");

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire),
        (m_fd == kFileInvalid) ? -1 : 0, m_create_future, m_loop);
    if (closed_future) {
        UR_DEBUG_LOG0("ThreadIOBackend::read file is closed");
        Py_DECREF(future);
        return closed_future;
    }

    uint64_t offset; size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = (int64_t)m_cachedFileSize - (int64_t)m_filePos;
        if (rem <= 0) {
            // 可能只是缓存长度过期（外部追加）：判 EOF 前先问一次内核
            refresh_cached_size_locked();
            rem = (int64_t)m_cachedFileSize - (int64_t)m_filePos;
        }
        if (rem <= 0) { 
            UR_DEBUG_LOG0("ThreadIOBackend::read EOF");
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

    IORequest *req = make_req_read_bytes(readSize, future);
    if (!req) [[unlikely]] { Py_DECREF(future); return nullptr; }  // MemoryError
    UR_DEBUG_LOG("ThreadIOBackend::read req=%p, offset=%llu, size=%zu", 
                 (void*)req, (unsigned long long)offset, readSize);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, readSize]() {
        UR_DEBUG_LOG("ThreadIOBackend::read task executing, handle=%lld, offset=%llu, size=%zu",
                     (long long)m_fd, (unsigned long long)offset, readSize);
        long long got = file_pread(m_fd, req->buf(), readSize,
                                   static_cast<long long>(offset));
        UR_DEBUG_LOG("ThreadIOBackend::read task done, got=%lld", got);
        if (got >= 0) {
            // 短读 = 真碰到 EOF（文件被外部截短 / 读穿了缓存长度）：校正缓存
            if (static_cast<size_t>(got) < readSize) refresh_cached_size();
            complete_ok(req, static_cast<size_t>(got));
        } else {
            UR_DEBUG_LOG("ThreadIOBackend::read task failed, native error=%d",
                         native_file_last_error());
            complete_error(req, native_file_last_error());
        }
    });

    UR_DEBUG_LOG0("ThreadIOBackend::read returning future");
    return future;
}

PyObject *ThreadIOBackend::read_at(int64_t offset, int64_t size) {
    UR_DEBUG_LOG("ThreadIOBackend::read_at start, this=%p, offset=%lld, size=%lld",
                 (void*)this, (long long)offset, (long long)size);

    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("ThreadIOBackend::read_at ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }

    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) {
        UR_DEBUG_LOG0("ThreadIOBackend::read_at failed to create future");
        return nullptr;
    }

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire),
        (m_fd == kFileInvalid) ? -1 : 0, m_create_future, m_loop);
    if (closed_future) {
        UR_DEBUG_LOG0("ThreadIOBackend::read_at file is closed");
        Py_DECREF(future);
        return closed_future;
    }

    if (offset < 0) {
        resolve_exc(future, g_ValueError, 0, "negative offset not allowed");
        return future;
    }

    size_t readSize;
    {
        // m_posMtx 仅为与写/截断路径的 cachedFileSize 更新保持一致；不读取也不修改 m_filePos
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = (int64_t)m_cachedFileSize - offset;
        if (rem <= 0) {
            // 缓存长度可能过期（外部追加）：offset 看着越界时先问内核
            refresh_cached_size_locked();
            rem = (int64_t)m_cachedFileSize - offset;
        }
        if (rem <= 0) {
            // offset 越界（≥ 文件大小）→ pread 语义：空 bytes
            UR_DEBUG_LOG0("ThreadIOBackend::read_at offset beyond EOF");
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
    }

    IORequest *req = make_req_read_bytes(readSize, future);
    if (!req) [[unlikely]] { Py_DECREF(future); return nullptr; }  // MemoryError
    UR_DEBUG_LOG("ThreadIOBackend::read_at req=%p, offset=%lld, size=%zu",
                 (void*)req, (long long)offset, readSize);

    // pread 显式偏移，不动文件指针 —— 与并发 read()/seek() 无竞争
    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, readSize]() {
        UR_DEBUG_LOG("ThreadIOBackend::read_at task executing, handle=%lld, offset=%lld, size=%zu",
                     (long long)m_fd, (long long)offset, readSize);
        long long got = file_pread(m_fd, req->buf(), readSize,
                                   static_cast<long long>(offset));
        UR_DEBUG_LOG("ThreadIOBackend::read_at task done, got=%lld", got);
        if (got >= 0) {
            if (static_cast<size_t>(got) < readSize) refresh_cached_size();  // 短读 → 校正缓存
            complete_ok(req, static_cast<size_t>(got));
        } else {
            UR_DEBUG_LOG("ThreadIOBackend::read_at task failed, native error=%d",
                         native_file_last_error());
            complete_error(req, native_file_last_error());
        }
    });

    UR_DEBUG_LOG0("ThreadIOBackend::read_at returning future");
    return future;
}

PyObject *ThreadIOBackend::write(Py_buffer *view, int64_t position) {
    UR_DEBUG_LOG("ThreadIOBackend::write start, this=%p, size=%zd", (void*)this, view->len);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("ThreadIOBackend::write ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    if (position >= 0 && (m_appendMode || file_is_append(m_fd))) {
        PyErr_SetString(PyExc_ValueError, "write_at() does not support append mode");
        return nullptr;
    }
    // Native completion counts and submission lengths are 32-bit.
    if (static_cast<uint64_t>(view->len) > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "write buffer exceeds 4 GiB - 1");
        return nullptr;
    }

    size_t size = static_cast<size_t>(view->len);
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire),
        (m_fd == kFileInvalid) ? -1 : 0, m_create_future, m_loop);
    if (closed_future) {
        Py_DECREF(future);
        return closed_future;
    }
    
    if (size == 0) {
        PyObject *z = PyLong_FromLong(0);
        resolve_ok(future, z); Py_DECREF(z);
        return future;
    }

    uint64_t offset = static_cast<uint64_t>(position);
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        if (position < 0) {
            offset = m_appendMode ? m_cachedFileSize : m_filePos;
            m_filePos = offset + size;
        }
        if (offset + size > m_cachedFileSize)
            m_cachedFileSize = offset + size;  // 乐观更新：假设写成功
    }

    IORequest *req = make_req_held_write(view, future);
    if (!req) { Py_DECREF(future); return nullptr; }
    UR_DEBUG_LOG("ThreadIOBackend::write req=%p, offset=%llu, size=%zu", 
                 (void*)req, (unsigned long long)offset, size);

    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, req, offset, size]() {
        UR_DEBUG_LOG("ThreadIOBackend::write task executing, handle=%lld, offset=%llu, size=%zu",
                     (long long)m_fd, (unsigned long long)offset, size);
        long long wrote = file_pwrite(m_fd, req->buf(), size,
                                      static_cast<long long>(offset));
        UR_DEBUG_LOG("ThreadIOBackend::write task done, wrote=%lld", wrote);
        if (wrote >= 0) {
            complete_ok(req, static_cast<size_t>(wrote));
        } else {
            UR_DEBUG_LOG("ThreadIOBackend::write task failed, native error=%d",
                         native_file_last_error());
            complete_error(req, native_file_last_error());
        }
    });

    return future;
}

PyObject *ThreadIOBackend::seek(int64_t offset, int whence) {
    UR_DEBUG_LOG("ThreadIOBackend::seek start, offset=%lld, whence=%d", (long long)offset, whence);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t new_pos;
        if (whence == 0) {
            new_pos = offset;
        } else if (whence == 1) {
            new_pos = static_cast<int64_t>(m_filePos) + offset;
        } else if (whence == 2) {
            // SEEK_END 问的就是「现在多长」：缓存值可能过期（外部追加）
            refresh_cached_size_locked();
            new_pos = static_cast<int64_t>(m_cachedFileSize) + offset;
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }

        // 负位置必须当场拒绝。直接 static_cast<uint64_t> 会把 -1 变成
        // 2^64-1：位置被静默污染，之后每次 read() 只报一个和原因毫不相干的
        // 错误（Windows 上实测 `ReadFile failed, errno 87`）。语义与
        // macos_gcd_backend 的 seek 保持一致。
        if (new_pos < 0) {
            resolve_exc(future, g_ValueError, 0, "negative seek position");
            return future;
        }

        m_filePos = static_cast<uint64_t>(new_pos);
    }
    UR_DEBUG_LOG("ThreadIOBackend::seek new pos=%llu", (unsigned long long)m_filePos);
    PyObject *pos = PyLong_FromUnsignedLongLong(m_filePos);
    resolve_ok(future, pos); Py_DECREF(pos);
    return future;
}

PyObject *ThreadIOBackend::flush() {
    UR_DEBUG_LOG0("ThreadIOBackend::flush start");
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    if (!m_running.load(std::memory_order_acquire) || m_fd == kFileInvalid) {
        resolve_exc(future, g_OSError, 0, "flush on closed file");
        return future;
    }
    
    if (file_fsync(m_fd) != 0) {
        const int err = native_file_last_error();
        UR_DEBUG_LOG("ThreadIOBackend::flush fsync failed, native error=%d", err);
        // 不调用 set_os_error()：它会把异常挂在 PyErr 上，而下面 resolve_exc
        // 还要做 Python 调用——留着 pending 异常只会污染后续操作。
        resolve_exc(future, g_OSError, err, "fsync failed");
        return future;
    }
    UR_DEBUG_LOG0("ThreadIOBackend::flush done");
    resolve_ok(future, Py_None);
    return future;
}

PyObject *ThreadIOBackend::close() {
    UR_DEBUG_LOG("ThreadIOBackend::close start, this=%p, initialized=%d", 
                 (void*)this, m_loop_initialized.load());
    
    if (!m_loop_initialized.load(std::memory_order_acquire)) {
        UR_DEBUG_LOG0("ThreadIOBackend::close not initialized, closing directly");
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

// POSIX 直接返回真实 fd（与内置 io 一致）。Windows 上返回一份**复制句柄**包装
// 出来的 CRT 描述符：同一个对象总是同一个 fd，由本对象在 close 时释放，且调用
// 方关掉它不会牵连文件本身（与 IOCP 后端的 fileno() 语义一致）。
int ThreadIOBackend::fileno() const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(m_filenoMtx);
    if (m_cachedFileno >= 0) return m_cachedFileno;
    if (m_fd == kFileInvalid) return -1;

    HANDLE dup = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(m_fd),
                         GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }

    // 访问权限在句柄上，CRT 侧用宽松的读写标志即可（越权访问到 OS 层才会失败）
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(dup), _O_RDWR | _O_BINARY);
    if (fd == -1) {
        CloseHandle(dup);
        return -1;
    }
    m_cachedFileno = fd;
    m_cachedDupHandle = dup;
    return fd;
#else
    return (m_fd == kFileInvalid) ? -1 : static_cast<int>(m_fd);
#endif
}

void ThreadIOBackend::close_impl() {
    UR_DEBUG_LOG("ThreadIOBackend::close_impl start, this=%p, handle=%lld",
                 (void*)this, (long long)m_fd);

#ifdef _WIN32
    // 放掉 fileno() 暴露的复制句柄。只在「槽位里仍是我们的句柄」时才关：
    // 调用方可能自己 close 过，fd 号会被别人复用，凭「有效」判断会误伤。
    {
        std::lock_guard<std::mutex> lk(m_filenoMtx);
        if (m_cachedFileno >= 0) {
            auto old = _set_thread_local_invalid_parameter_handler(ignore_invalid_parameter);
            const intptr_t slot = _get_osfhandle(m_cachedFileno);
            if (slot != -1 &&
                reinterpret_cast<HANDLE>(slot) == m_cachedDupHandle) {
                _close(m_cachedFileno);
            }
            _set_thread_local_invalid_parameter_handler(old);
            m_cachedFileno = -1;
            m_cachedDupHandle = INVALID_HANDLE_VALUE;
        }
    }
#endif

    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        UR_DEBUG_LOG0("ThreadIOBackend::close_impl already closed");
        return;
    }

    // 等待 pending I/O 完成（任务在全局共享线程池中执行，不需要停掉线程）。
    // 完成路径会 release m_close_wake，收到信号立即醒来复查，不再 sleep 轮询。
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(m_cached_close_timeout_ms);
    while (m_pending.load(std::memory_order_acquire) > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remain.count() == 0) break;
        UR_DEBUG_LOG("ThreadIOBackend::close_impl waiting for pending I/O, pending=%ld",
                     m_pending.load());
        Py_BEGIN_ALLOW_THREADS  // Release GIL so thread pool workers can complete callbacks
        m_close_wake.try_acquire_for(remain);
        Py_END_ALLOW_THREADS    // Reacquire GIL
    }

    if (m_pending.load() > 0) {
        UR_DEBUG_LOG("ThreadIOBackend::close_impl timeout waiting for pending I/O, forcing close. pending=%ld",
                     m_pending.load());
    }

    if (m_owns_fd && m_fd != kFileInvalid) {
        UR_DEBUG_LOG("ThreadIOBackend::close_impl closing handle=%lld", (long long)m_fd);
        file_close(m_fd);
        m_fd = kFileInvalid;
    }
    UR_DEBUG_LOG0("ThreadIOBackend::close_impl done");
}

PyObject* ThreadIOBackend::tell() {
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

PyObject* ThreadIOBackend::truncate(int64_t size) {
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
    
    if (file_truncate(m_fd, static_cast<unsigned long long>(size)) != 0) {
        resolve_exc(future, g_OSError, native_file_last_error(), "truncate failed");
        return future;
    }
    
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        m_cachedFileSize = static_cast<uint64_t>(size);
        if (static_cast<uint64_t>(size) < m_filePos) {
            m_filePos = static_cast<uint64_t>(size);
        }
    }
    
    resolve_ok(future, Py_None);
    return future;
}

PyObject* ThreadIOBackend::readinto(PyObject* buf) {
    UR_DEBUG_LOG("ThreadIOBackend::readinto start, this=%p", (void*)this);
    
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire),
        (m_fd == kFileInvalid) ? -1 : 0, m_create_future, m_loop);
    if (closed_future) { Py_DECREF(future); return closed_future; }
    
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
    
    uint64_t offset;
    size_t readSize;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);
        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - static_cast<int64_t>(m_filePos);
        if (rem <= 0) {
            // 缓存长度可能过期（外部追加）：判 EOF 前先问内核
            refresh_cached_size_locked();
            rem = static_cast<int64_t>(m_cachedFileSize) - static_cast<int64_t>(m_filePos);
        }
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
    enqueue_task([this, req, offset, readSize]() {
        long long got = file_pread(m_fd, req->buf(), readSize,
                                   static_cast<long long>(offset));
        if (got >= 0) {
            if (static_cast<size_t>(got) < readSize) refresh_cached_size();  // 短读 → 校正缓存
            complete_ok(req, static_cast<size_t>(got));
        }
        else complete_error(req, static_cast<DWORD>(native_file_last_error()));
    });
    
    return future;
}

} // namespace ayafileio
