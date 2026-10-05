#ifdef __APPLE__

#include "macos_gcd_backend.hpp"
#include "../globals.hpp"
#include "../config.hpp"
#include "./utils/file_mode.hpp"
#include "./utils/error_util.hpp"
#include "utils/debug_log.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// 全局缓存（线程安全）
// ════════════════════════════════════════════════════════════════════════════

static PyObject*   g_cachedLoop       = nullptr;
static PyObject*   g_cachedFutureFn   = nullptr;
static ResultBatcher* g_cachedLoopHandle = nullptr;
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
    g_cachedLoopHandle = get_or_create_batcher(loop);
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

    if (mi.hasW)      flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mi.hasA) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mi.hasX) flags = O_WRONLY | O_CREAT | O_EXCL;
    if (mi.plus)      flags = (flags & ~O_ACCMODE) | O_RDWR;

    m_appendMode = mi.appendMode;
    
    UR_DEBUG_LOG("MacOSGCDBackend: opening file with flags=%d", flags);
    {
        GilRelease gr;  // 阻塞系统调用：释放 GIL（见 globals.hpp GilRelease）
        m_fd = open(path.c_str(), flags, 0644);
    }
    if (m_fd == -1) {
        UR_DEBUG_LOG("MacOSGCDBackend: open failed, errno=%d", errno);
        throw_os_error("Failed to open file", path.c_str());
    }
    UR_DEBUG_LOG("MacOSGCDBackend: file opened, fd=%d", m_fd);

    // dup fd 隔离：channel 存续期间 fd 处于 libdispatch 控制之下（含
    // GUARD_CLOSE 守卫），应用不得触碰。dup 一份副本给 GCD，原始 fd
    // 由我们完全控制；dup 的关闭职责交给 cleanup handler（见下）。
    int gcd_fd = dup(m_fd);
    if (gcd_fd == -1) {
        UR_DEBUG_LOG("MacOSGCDBackend: dup failed, errno=%d", errno);
        int saved_errno = errno;
        ::close(m_fd);
        m_fd = -1;
        throw_os_error(saved_errno, "Failed to dup file descriptor for GCD", path.c_str());
    }
    UR_DEBUG_LOG("MacOSGCDBackend: dup'd fd=%d for GCD", gcd_fd);

    // 创建 GCD 串行队列
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0);
    m_queue = dispatch_queue_create("com.ayafileio.gcd", attr);
    UR_DEBUG_LOG("MacOSGCDBackend: GCD queue created, queue=%p", (void*)m_queue);

    // cleanup handler 独占 dup fd 的关闭：契约上 handler 入队即控制权
    // 交还（"making it safe for the application to close(2)"），handler
    // 在 close_queue 上排在 fd_entry teardown（unguard / LIST_REMOVE /
    // free）之后，此刻 close 不会踩守卫、不会与 teardown 抢 fd 号。
    // shared_ptr 捕获：handler 由 block 生命周期保活，对象析构后迟到的
    // handler 也不碰 this。
    auto fd_cleanup = std::make_shared<GcdFdCleanup>();
    fd_cleanup->fd = gcd_fd;
    fd_cleanup->done = dispatch_semaphore_create(0);

    // 创建 Dispatch I/O 通道（随机访问模式，支持 seek）
    m_channel = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        gcd_fd,
        m_queue,
        ^(int error) {
            UR_DEBUG_LOG("MacOSGCDBackend: dispatch_io_create cleanup handler, error=%d", error);
            ::close(fd_cleanup->fd);
            dispatch_semaphore_signal(fd_cleanup->done);
        }
    );

    if (!m_channel) {
        UR_DEBUG_LOG0("MacOSGCDBackend: dispatch_io_create failed");
        ::close(gcd_fd);  // GCD 未接管，自己关 dup fd
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Failed to create dispatch I/O channel");
    }
    UR_DEBUG_LOG("MacOSGCDBackend: dispatch_io_create success, channel=%p", (void*)m_channel);
    m_fd_cleanup = std::move(fd_cleanup);  // close_impl 等待 cleanup 跑完的凭据
    
    // 配置缓冲区参数
    dispatch_io_set_high_water(m_channel, m_cached_buffer_size);
    dispatch_io_set_low_water(m_channel, m_cached_buffer_size / 4);
    UR_DEBUG_LOG("MacOSGCDBackend: I/O channel configured, high_water=%zu, low_water=%zu",
                 m_cached_buffer_size, m_cached_buffer_size / 4);
    
    m_running.store(true, std::memory_order_release);
    m_pending.store(0, std::memory_order_relaxed);
    m_filePos = 0;
    
    {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_cachedFileSize = static_cast<uint64_t>(st.st_size);
            if (m_appendMode) {
                m_filePos = m_cachedFileSize;
                UR_DEBUG_LOG("MacOSGCDBackend: append mode, filePos=%llu", (unsigned long long)m_filePos);
            }
        }
    }

    // 小读 mincore 内联快车道的驻留探测映射：只喂给 mincore，从不解引用，
    // 故映射窗口之外的页不会造成 SIGBUS。映射失败仅退化为无快车道。
    if (mi.canRead && m_cachedFileSize > 0) {
        void* p;
        {
            GilRelease gr;  // 阻塞系统调用：释放 GIL（见 globals.hpp GilRelease）
            p = mmap(nullptr, static_cast<size_t>(m_cachedFileSize),
                     PROT_READ, MAP_SHARED, m_fd, 0);
        }
        if (p != MAP_FAILED) {
            m_mapBase = p;
            m_mapSize = static_cast<size_t>(m_cachedFileSize);
            UR_DEBUG_LOG("MacOSGCDBackend: mincore mapping created, size=%zu", m_mapSize);
        } else {
            UR_DEBUG_LOG("MacOSGCDBackend: mmap for mincore failed, errno=%d", errno);
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

    // dup fd 隔离：channel 存续期间 fd 处于 libdispatch 控制之下，dup 的
    // 关闭职责归 cleanup handler（同路径构造函数）
    int gcd_fd = dup(m_fd);
    if (gcd_fd == -1) {
        UR_DEBUG_LOG("MacOSGCDBackend: dup failed in fd ctor, errno=%d", errno);
        throw_os_error("Failed to dup file descriptor for GCD", "<fd>");
    }
    UR_DEBUG_LOG("MacOSGCDBackend: dup'd fd=%d for GCD", gcd_fd);

    // 创建 GCD 串行队列
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0);
    m_queue = dispatch_queue_create("com.ayafileio.gcd", attr);

    auto fd_cleanup = std::make_shared<GcdFdCleanup>();
    fd_cleanup->fd = gcd_fd;
    fd_cleanup->done = dispatch_semaphore_create(0);

    // 创建 Dispatch I/O 通道（用 dup 的 fd；cleanup handler 独占其关闭）
    m_channel = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        gcd_fd,
        m_queue,
        ^(int error) {
            UR_DEBUG_LOG("MacOSGCDBackend: fd channel cleanup, error=%d", error);
            ::close(fd_cleanup->fd);
            dispatch_semaphore_signal(fd_cleanup->done);
        }
    );

    if (!m_channel) {
        UR_DEBUG_LOG0("MacOSGCDBackend: fd dispatch_io_create failed");
        ::close(gcd_fd);  // GCD 未接管，自己关 dup fd
        throw std::runtime_error("Failed to create dispatch I/O channel from fd");
    }
    m_fd_cleanup = std::move(fd_cleanup);

    dispatch_io_set_high_water(m_channel, m_cached_buffer_size);
    dispatch_io_set_low_water(m_channel, m_cached_buffer_size / 4);
    
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

    // 小读 mincore 内联快车道的驻留探测映射（同路径构造函数）
    if (mi.canRead && m_cachedFileSize > 0) {
        void* p;
        {
            GilRelease gr;  // 阻塞系统调用：释放 GIL（见 globals.hpp GilRelease）
            p = mmap(nullptr, static_cast<size_t>(m_cachedFileSize),
                     PROT_READ, MAP_SHARED, m_fd, 0);
        }
        if (p != MAP_FAILED) {
            m_mapBase = p;
            m_mapSize = static_cast<size_t>(m_cachedFileSize);
            UR_DEBUG_LOG("MacOSGCDBackend: fd ctor mincore mapping created, size=%zu", m_mapSize);
        } else {
            UR_DEBUG_LOG("MacOSGCDBackend: fd ctor mmap for mincore failed, errno=%d", errno);
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
    m_batcher = g_cachedLoopHandle;

    // 大请求线程快速路依赖全局线程池（macOS 主路径是 GCD，
    // ThreadIOBackend 可能从不存在，线程池需自行拉起）
    auto& cfg = ayafileio::config();
    m_num_workers = cfg.io_worker_count();
    if (m_num_workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 1;
        m_num_workers = std::max(1u, std::min(hc * 2u, 16u));
    }
    GlobalThreadPool::instance().ensure_started(m_num_workers);

    m_loop_initialized = true;
    UR_DEBUG_LOG("MacOSGCDBackend::ensure_loop_initialized done, this=%p", (void*)this);
}

// ════════════════════════════════════════════════════════════════════════════
// 大请求线程快速路 — 直接 pread/pwrite 进最终缓冲，零回调零拷贝
// ════════════════════════════════════════════════════════════════════════════

void MacOSGCDBackend::submit_read_fast(IORequest* req, uint64_t offset, size_t size) {
    int fd = m_fd;
    GlobalThreadPool::instance().enqueue([this, req, fd, offset, size]() {
        ssize_t got = pread(fd, req->buf(), size, static_cast<off_t>(offset));
        if (got >= 0) complete_ok(req, static_cast<size_t>(got));
        else complete_error(req, errno);
    });
}

void MacOSGCDBackend::submit_write_fast(IORequest* req, uint64_t offset, size_t size) {
    int fd = m_fd;
    GlobalThreadPool::instance().enqueue([this, req, fd, offset, size]() {
        ssize_t wrote = pwrite(fd, req->buf(), size, static_cast<off_t>(offset));
        if (wrote >= 0) complete_ok(req, static_cast<size_t>(wrote));
        else complete_error(req, errno);
    });
}

// ── 大读并行分块填充 ─────────────────────────────────────────────────
//
// N 个分块 pread 共用一个 IORequest（各写各的位置区间，零协调）；仅最后
// 一个完成者负责 complete_*（含 batcher 记账，整请求仍是一次完成）与回收
// 聚合组。任一块失败即整请求失败——部分数据不可交付，与 read_at/write_at
// 流水线路径的错误语义一致。取消语义同其它快速路：已提交的内核/线程工作
// 不可撤销，future 取消不影响数据完整性。

namespace {

struct ParallelReadGroup {
    IORequest* req;
    std::atomic<size_t> remaining;   // 尚未完成的分块数
    std::atomic<size_t> copied{0};   // 各块成功字节累加
    std::atomic<int> first_errno{0}; // 首个失败 errno（0 = 无）
};

} // namespace

void MacOSGCDBackend::submit_read_parallel(IORequest* req, uint64_t offset, size_t size) {
    // 分块数封顶 256，超出则放大块大小（巨型文件也不撑爆任务队列）
    size_t chunk_size = PARALLEL_READ_CHUNK;
    while ((size + chunk_size - 1) / chunk_size > 256) {
        chunk_size *= 2;
    }
    const size_t nchunks = (size + chunk_size - 1) / chunk_size;

    auto* group = new ParallelReadGroup();
    group->req = req;
    group->remaining.store(nchunks, std::memory_order_relaxed);

    const int fd = m_fd;
    auto* self = this;
    for (size_t i = 0; i < nchunks; ++i) {
        const size_t off_in = i * chunk_size;
        const size_t len = std::min(chunk_size, size - off_in);
        GlobalThreadPool::instance().enqueue([group, self, fd, offset, off_in, len]() {
            ssize_t got;
            do {
                got = pread(fd, group->req->buf() + off_in, len,
                            static_cast<off_t>(offset + off_in));
            } while (got < 0 && errno == EINTR);

            if (got < 0) {
                int expected = 0;
                group->first_errno.compare_exchange_strong(expected, errno,
                                                           std::memory_order_acq_rel);
            } else {
                group->copied.fetch_add(static_cast<size_t>(got), std::memory_order_acq_rel);
            }

            if (group->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // 最后一块：聚合收尾（整请求一次完成，batcher 记账不变）
                IORequest* r = group->req;
                const size_t total = group->copied.load(std::memory_order_acquire);
                const int err = group->first_errno.load(std::memory_order_acquire);
                delete group;
                if (err) self->complete_error(r, static_cast<DWORD>(err));
                else self->complete_ok(r, total);
            }
        });
    }
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
        
        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - static_cast<int64_t>(m_filePos);
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
    
    IORequest* req = make_req_read_bytes(readSize, future);
    if (!req) [[unlikely]] { Py_DECREF(future); return nullptr; }  // MemoryError
    UR_DEBUG_LOG("MacOSGCDBackend::read req=%p, offset=%llu, size=%zu",
                 (void*)req, (unsigned long long)offset, readSize);

    m_pending.fetch_add(1, std::memory_order_relaxed);

    if (readSize >= PARALLEL_READ_THRESHOLD) {
        // 大读：并行分块填充，多 worker 并发 pread 进同一预建缓冲
        submit_read_parallel(req, offset, readSize);
        return future;
    }

    if (readSize >= LARGE_IO_THRESHOLD) {
        // 大请求：线程快速路，一发 pread 直接进 preResult PyBytes
        submit_read_fast(req, offset, readSize);
        return future;
    }

    // 小读：缓存命中走 mincore 内联快车道（就地 resolve，await 不让出
    // 事件循环）；冷页/无映射/越界时落回 dispatch_io 异步路径
    if (try_inline_read(req, offset, readSize)) {
        return future;
    }

    auto self = this;
    __block size_t total_copied = 0;
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
                size_t chunk_size = dispatch_data_get_size(data);
                UR_DEBUG_LOG("MacOSGCDBackend::read callback got data, size=%zu", chunk_size);

                dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t off, const void* buf, size_t len) {
                    UR_DEBUG_LOG("MacOSGCDBackend::read callback copying region: off=%zu, len=%zu", off, len);
                    memcpy(req->buf() + total_copied + off, buf, len);
                    return true;
                });
                total_copied += chunk_size;
            }

            if (done) {
                UR_DEBUG_LOG("MacOSGCDBackend::read callback done, total_bytes=%zu", total_copied);
                self->complete_ok(req, total_copied);
            }
        }
    );
    
    UR_DEBUG_LOG0("MacOSGCDBackend::read returning future");
    return future;
}

PyObject* MacOSGCDBackend::read_at(int64_t offset, int64_t size) {
    UR_DEBUG_LOG("MacOSGCDBackend::read_at start, this=%p, offset=%lld, size=%lld",
                 (void*)this, (long long)offset, (long long)size);

    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("MacOSGCDBackend::read_at ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }

    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) {
        UR_DEBUG_LOG0("MacOSGCDBackend::read_at failed to create future");
        return nullptr;
    }

    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
    if (closed_future) {
        UR_DEBUG_LOG0("MacOSGCDBackend::read_at file is closed");
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

        int64_t rem = static_cast<int64_t>(m_cachedFileSize) - offset;
        if (rem <= 0) {
            // offset 越界（≥ 文件大小）→ pread 语义：空 bytes
            UR_DEBUG_LOG0("MacOSGCDBackend::read_at offset beyond EOF");
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

    IORequest* req = make_req_read_bytes(readSize, future);
    if (!req) [[unlikely]] { Py_DECREF(future); return nullptr; }  // MemoryError
    UR_DEBUG_LOG("MacOSGCDBackend::read_at req=%p, offset=%lld, size=%zu",
                 (void*)req, (long long)offset, readSize);

    m_pending.fetch_add(1, std::memory_order_relaxed);

    if (readSize >= PARALLEL_READ_THRESHOLD) {
        // 大读：并行分块填充（同 read()）
        submit_read_parallel(req, static_cast<uint64_t>(offset), readSize);
        return future;
    }

    if (readSize >= LARGE_IO_THRESHOLD) {
        // 大请求：线程快速路，一发 pread 直接进 preResult PyBytes
        submit_read_fast(req, static_cast<uint64_t>(offset), readSize);
        return future;
    }

    // 小读：缓存命中走 mincore 内联快车道（同 read()）
    if (try_inline_read(req, static_cast<uint64_t>(offset), readSize)) {
        return future;
    }

    // 通道为 DISPATCH_IO_RANDOM，dispatch_io_read 带显式 offset，不动文件指针
    auto self = this;
    __block size_t total_copied = 0;
    dispatch_io_read(
        m_channel,
        offset,
        readSize,
        m_queue,
        ^(bool done, dispatch_data_t data, int error) {
            UR_DEBUG_LOG("MacOSGCDBackend::read_at callback: done=%d, data=%p, error=%d, req=%p",
                         done, (void*)data, error, (void*)req);

            if (error) {
                UR_DEBUG_LOG("MacOSGCDBackend::read_at callback error=%d", error);
                self->complete_error(req, static_cast<DWORD>(error));
                return;
            }

            if (data) {
                size_t chunk_size = dispatch_data_get_size(data);
                UR_DEBUG_LOG("MacOSGCDBackend::read_at callback got data, size=%zu", chunk_size);

                dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t off, const void* buf, size_t len) {
                    UR_DEBUG_LOG("MacOSGCDBackend::read_at callback copying region: off=%zu, len=%zu", off, len);
                    memcpy(req->buf() + total_copied + off, buf, len);
                    return true;
                });
                total_copied += chunk_size;
            }

            if (done) {
                UR_DEBUG_LOG("MacOSGCDBackend::read_at callback done, total_bytes=%zu", total_copied);
                self->complete_ok(req, total_copied);
            }
        }
    );

    UR_DEBUG_LOG0("MacOSGCDBackend::read_at returning future");
    return future;
}

PyObject* MacOSGCDBackend::write(Py_buffer* view, int64_t position) {
    UR_DEBUG_LOG("MacOSGCDBackend::write start, this=%p, size=%zd", (void*)this, view->len);
    
    try {
        ensure_loop_initialized();
    } catch (const std::runtime_error& e) {
        UR_DEBUG_LOG("MacOSGCDBackend::write ensure_loop failed: %s", e.what());
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    if (position >= 0 && (m_appendMode || (fcntl(m_fd, F_GETFL) & O_APPEND))) {
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
    
    if (size >= LARGE_IO_THRESHOLD) {
        // 大写：线程快速路，持调用方缓冲零拷贝，内核直接读用户内存
        IORequest* req = make_req_held_write(view, future);
        if (!req) { Py_DECREF(future); return nullptr; }
        UR_DEBUG_LOG("MacOSGCDBackend::write fast req=%p, offset=%llu, size=%zu",
                     (void*)req, (unsigned long long)offset, size);
        m_pending.fetch_add(1, std::memory_order_relaxed);
        submit_write_fast(req, offset, size);
        return future;
    }

    IORequest* req = make_req(size, future, ReqType::Write);
    std::memcpy(req->buf(), view->buf, size);
    UR_DEBUG_LOG("MacOSGCDBackend::write req=%p, offset=%llu, size=%zu",
                 (void*)req, (unsigned long long)offset, size);
    
    m_pending.fetch_add(1, std::memory_order_relaxed);
    
    dispatch_data_t write_data = dispatch_data_create(
        req->buf(), size, m_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

    req->release_buffer_ownership();  // 缓冲所有权已转移给 dispatch_data

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
    
    // macOS dispatch_io 使用显式 offset，不需要操作 fd
    int64_t new_pos;
    {
        std::lock_guard<std::mutex> lk(m_posMtx);

        if (whence == 0) {
            new_pos = offset;
        } else if (whence == 1) {
            new_pos = static_cast<int64_t>(m_filePos) + offset;
        } else if (whence == 2) {
            new_pos = static_cast<int64_t>(m_cachedFileSize) + offset;
        } else {
            resolve_exc(future, g_ValueError, 0, "Invalid whence value");
            return future;
        }

        if (new_pos < 0) {
            resolve_exc(future, g_ValueError, 0, "negative seek position");
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
    
    // 等待 pending I/O 排空：完成路径会 release m_close_wake，收到信号
    // 立即醒来复查，不再 sleep 指数退避轮询（close 延迟从平均 ~16ms 降到即时）
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(m_cached_close_timeout_ms);
    while (m_pending.load(std::memory_order_acquire) > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remain.count() == 0) break;
        UR_DEBUG_LOG("MacOSGCDBackend::close_impl waiting for pending I/O, pending=%ld",
                     m_pending.load());
        Py_BEGIN_ALLOW_THREADS  // Release GIL so GCD callbacks can complete
        m_close_wake.try_acquire_for(remain);
        Py_END_ALLOW_THREADS    // Reacquire GIL
    }
    
    if (m_pending.load() > 0) {
        UR_DEBUG_LOG("MacOSGCDBackend::close_impl timeout waiting for pending I/O, forcing close. pending=%ld",
                     m_pending.load());
    }

    // 解除小读快车道的驻留探测映射。pending 已排空（内联读在 pending
    // 归零前就已完成 mincore/pread 窗口），此时无读者。
    if (m_mapBase) {
        munmap(const_cast<void*>(m_mapBase), m_mapSize);
        m_mapBase = nullptr;
        m_mapSize = 0;
        UR_DEBUG_LOG0("MacOSGCDBackend: mincore mapping released");
    }

    dispatch_io_t channel = nullptr;
    if (m_channel) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl closing dispatch channel");
        // DISPATCH_IO_STOP stops I/O immediately.  The cleanup handler is NOT
        // enqueued synchronously: it reaches m_queue via a multi-hop async
        // chain (stop → channel->queue → barrier_queue → close_queue resume
        // → trampoline → m_queue), so a barrier on m_queue alone cannot
        // guarantee it has run — that was the 1.11.1 race: close_impl probed
        // and closed the dup while teardown was still in flight, the fd
        // number went back to the kernel early, and teardown's own late
        // touches (or a second close on implementations that close in
        // teardown) landed on whatever file had reused the number → the new
        // file's next write failed with EBADF (errno 9).  The cleanup
        // handler is now the sole closer (see the constructors), and we
        // *wait* for it below.
        dispatch_io_close(m_channel, DISPATCH_IO_STOP);
        channel = m_channel;  // release 之后马上做：dispose 才会释放 fd_entry
        m_channel = nullptr;
    }

    if (channel) {
        // dispatch_io_create 返回 +1 引用：只置空指针从不 release，channel
        // 对象永不销毁、cleanup handler 永不运行、dup 的 fd 永不关闭——
        // 每次 open/close 漏一个 fd（CI 实测：两千余次开关后直接 EMFILE，
        // 长跑进程会在某个月黑风高时刻集体 "too many open files"）。
        dispatch_release(channel);
    }

    if (m_fd_cleanup) {
        // 等 cleanup handler 跑完（它已在 handler 里 close 了 dup fd）。
        // 等待把 fd 号回收串行化到 libdispatch teardown 完成之后：fd_entry
        // 是 libdispatch 进程内按 fd 号全局缓存的， teardown 未跑完就复用
        // 该号会让新 channel 挂到将死的 fd_entry 上（UAF / 标志位错乱）。
        // 超时兜底：handler 仍会迟到执行（block 捕获 shared_ptr 保活），
        // fd 由它关闭，无泄漏、无双关。
        std::shared_ptr<GcdFdCleanup> cleanup = m_fd_cleanup;
        int64_t ns = static_cast<int64_t>(m_cached_close_timeout_ms) * 1000000LL;
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl waiting for channel cleanup");
        long rc;  // Py_*_ALLOW_THREADS 是块作用域宏，rc 须在块外声明
        Py_BEGIN_ALLOW_THREADS  // cleanup handler 不需要 GIL，但别占着它等
        rc = dispatch_semaphore_wait(cleanup->done,
                                     dispatch_time(DISPATCH_TIME_NOW, ns));
        Py_END_ALLOW_THREADS
        if (rc != 0) {
            UR_DEBUG_LOG0("MacOSGCDBackend::close_impl cleanup wait timed out; fd close deferred to cleanup handler");
        }
        m_fd_cleanup.reset();
    }

    if (m_queue) {
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl waiting for queue drain");
        // cleanup handler 已在 m_queue 上跑完，此处仅排空残余块再 release
        Py_BEGIN_ALLOW_THREADS
        dispatch_barrier_sync(m_queue, ^{});
        Py_END_ALLOW_THREADS
        UR_DEBUG_LOG0("MacOSGCDBackend::close_impl queue drained, releasing");
        dispatch_release(m_queue);
        m_queue = nullptr;
    }

    if (m_owns_fd && m_fd != -1) {
        ::close(m_fd);
    }
    m_fd = -1;

    UR_DEBUG_LOG0("MacOSGCDBackend::close_impl done");
}

PyObject* MacOSGCDBackend::tell() {
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

PyObject* MacOSGCDBackend::truncate(int64_t size) {
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
        m_cachedFileSize = static_cast<uint64_t>(size);
        if (static_cast<uint64_t>(size) < m_filePos) {
            m_filePos = static_cast<uint64_t>(size);
        }
    }
    
    resolve_ok(future, Py_None);
    return future;
}

PyObject* MacOSGCDBackend::readinto(PyObject* buf) {
    UR_DEBUG_LOG("MacOSGCDBackend::readinto start, this=%p", (void*)this);
    
    try { ensure_loop_initialized(); }
    catch (const std::runtime_error&) {
        return create_rejected_future(nullptr, g_ValueError, "No running event loop", 0);
    }
    
    PyObject* future = PyObject_CallNoArgs(m_create_future);
    if (!future) return nullptr;
    
    PyObject* closed_future = check_closed_and_return_future(
        m_running.load(std::memory_order_acquire), m_fd, m_create_future, m_loop);
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

    if (readSize >= PARALLEL_READ_THRESHOLD) {
        // 大读：并行分块填充（直接进用户缓冲区）
        submit_read_parallel(req, offset, readSize);
        return future;
    }

    if (readSize >= LARGE_IO_THRESHOLD) {
        // 大请求：线程快速路，一发 pread 直接进用户缓冲区
        submit_read_fast(req, offset, readSize);
        return future;
    }

    auto self = this;
    __block size_t total_copied = 0;
    dispatch_io_read(m_channel, offset, readSize, m_queue,
        ^(bool done, dispatch_data_t data, int error) {
            if (error) {
                self->complete_error(req, static_cast<DWORD>(error));
                return;
            }
            if (data) {
                size_t chunk_size = dispatch_data_get_size(data);
                dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t off, const void* src, size_t len) {
                    memcpy(req->buf() + total_copied + off, src, len);
                    return true;
                });
                total_copied += chunk_size;
            }
            if (done) self->complete_ok(req, total_copied);
        });
    
    return future;
}

// ── 小读 mincore 内联快车道 ─────────────────────────────────────────────
//
// dispatch_io 每 op 都需过队列交付（handler hop + 唤醒），缓存命中的
// 小读由本方法就地完成：mincore 判定整个请求范围常驻页缓存后，在调用
// 线程（持 GIL）直接 pread 进预建的 PyBytes，并以 complete_inline 即刻
// resolve——无 GCD 回调、无线程池、无 batch flush 一跳。这是 macOS 侧
// 对应 Windows FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 与 io_uring
// COOP_TASKRUN 内联收割的快车道，补上三平台热路径的最后一块。
//
// 竞态与语义（与 turbofile 同款赌注）：mincore 判定与 pread 之间页可能
// 被换出，此时 pread 在当前线程阻塞。接受——热路径赌命中率；冷数据/大
// 请求本就走 dispatch_io/线程池，不受影响。短读由 complete_inline 收缩
// preResult；pread 失败亦由 complete_inline 转成异常 future。
bool MacOSGCDBackend::try_inline_read(IORequest* req, uint64_t offset, size_t size) {
    const char* base = static_cast<const char*>(m_mapBase);
    const size_t map_size = m_mapSize;
    if (!base || map_size == 0) [[unlikely]] return false;

    // 请求范围必须完整落在 open 时的映射窗口内；文件此后增长的区域
    // 不走快车道（落回 dispatch_io 异步路径）。
    if (offset > map_size || size > map_size - offset) [[unlikely]] return false;

    static const size_t s_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t first_page = offset / s_page;
    const size_t npages = (offset + size - 1) / s_page - first_page + 1;
    if (npages > 64) [[unlikely]] return false;  // 上限保护（正常有 LARGE_IO_THRESHOLD 兜底，远小于此）

    // mincore 在部分新 SDK 被标记 deprecated；调用本身有效，压掉告警
    char vec[64] = {0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const int mc = mincore(const_cast<void*>(
        static_cast<const void*>(base + first_page * s_page)), npages * s_page, vec);
#pragma clang diagnostic pop
    if (mc != 0) [[unlikely]] return false;
    for (size_t i = 0; i < npages; ++i) {
        if (!(vec[i] & 1)) [[unlikely]] return false;  // 存在未驻留页 → 冷路径
    }

    // 全命中：当前线程（持 GIL）直接 pread 进预建 PyBytes。承诺内联后
    // 必须在此完成（含错误），不可落回——否则同一 future 会被双投递。
    // EINTR 透明重试，与 CPython 的 pread 封装一致（dispatch 路径内部
    // 同样重试，避免信号把一次正常的缓存命中读变成 OSError）。
    ssize_t got;
    do {
        got = pread(m_fd, req->buf(), size, static_cast<off_t>(offset));
    } while (got < 0 && errno == EINTR);
    complete_inline(req, got);
    return true;
}

} // namespace ayafileio

#endif // __APPLE__