#include "iocp_context.hpp"
#include "pool.hpp"
#include "config.hpp"
#include "global_thread_pool.hpp"
#include "utils/debug_log.hpp"
#include "utils/yuyuko_memlife.hpp"
#include <algorithm>
#include <chrono>
#include <deque>

namespace ayafileio {

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

// Quarantine list — IORequest objects are NOT freed immediately to prevent
// address-reuse from defeating the IOCP double-delivery CAS guard when
// Windows re-delivers a completion across multiple GQCSEx calls.
// Each entry is held for at least QUARANTINE_MS before being freed.
//
// FIFO by construction: entries are appended with a non-decreasing
// steady_clock timestamp, so the expired ones are always a *prefix*. A deque
// therefore lets drain pop that prefix in amortised O(1) per entry. The
// previous std::vector version did `erase(it)` for every expired entry — an
// O(depth) memmove each — and scanned the whole list on every call, so each
// completed op paid O(queue depth): at 30k ops/s the queue holds ~30k entries
// (one QUARANTINE_MS of traffic) and the drain inspected tens of thousands of
// entries per op. That is what made sustained small-write throughput collapse
// to a third of its post-idle value while burning 2.3x CPU per op.
static constexpr unsigned QUARANTINE_MS = 1000;
struct QuarantineEntry { IORequest* req; std::chrono::steady_clock::time_point when; };
static thread_local std::deque<QuarantineEntry> t_quarantine;

// ── instrumentation (relaxed atomics, see iocp_context.hpp) ─────────────────
namespace {
std::atomic<std::uint64_t> g_q_live{0};
std::atomic<std::uint64_t> g_q_pushes{0};
std::atomic<std::uint64_t> g_q_scanned{0};
std::atomic<std::uint64_t> g_q_drains{0};
std::atomic<std::uint64_t> g_q_peak{0};

inline void q_note_push() {
    std::uint64_t live = g_q_live.fetch_add(1, std::memory_order_relaxed) + 1;
    g_q_pushes.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t peak = g_q_peak.load(std::memory_order_relaxed);
    while (live > peak &&
           !g_q_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}
inline void q_note_free() { g_q_live.fetch_sub(1, std::memory_order_relaxed); }
} // namespace

static void quarantine_drain() {
    const auto window = std::chrono::milliseconds(QUARANTINE_MS);
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t expired = 0;
    while (!t_quarantine.empty() && now - t_quarantine.front().when >= window) {
        REQ_FREE(t_quarantine.front().req);
        t_quarantine.pop_front();
        q_note_free();
        ++expired;
    }
    g_q_scanned.fetch_add(expired, std::memory_order_relaxed);
    g_q_drains.fetch_add(1, std::memory_order_relaxed);
}

static void quarantine_push(IORequest* req) {
    t_quarantine.push_back({req, std::chrono::steady_clock::now()});
    q_note_push();
    if (t_quarantine.size() >= 256) quarantine_drain();
}

// 本线程 quarantine 队首的剩余保留时间（毫秒）；队列空返回 INFINITE。
// 供 worker 的等待超时使用：没有新 I/O 时 GetQueuedCompletionStatusEx 会
// 一直阻塞（没有 batcher 待投递时超时是 INFINITE），于是最后一批完成的请求
// 永远等不到下一次 drain —— 每个 IORequest 还牵着它的 future（以及未来
// 事件循环对象的引用），那批内存就留在进程里直到下次有流量。
// 把队首到期时间并进等待超时，空闲时也能把它放掉。
static DWORD quarantine_timeout_ms() {
    if (t_quarantine.empty()) return INFINITE;
    const auto deadline = t_quarantine.front().when
        + std::chrono::milliseconds(QUARANTINE_MS);
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (ms < 1) ms = 1;
    // 留 1ms 余量，避免刚好卡在边界上反复空转
    return static_cast<DWORD>(ms + 1);
}

QuarantineStats quarantine_stats() {
    QuarantineStats s;
    s.live    = g_q_live.load(std::memory_order_relaxed);
    s.pushes  = g_q_pushes.load(std::memory_order_relaxed);
    s.scanned = g_q_scanned.load(std::memory_order_relaxed);
    s.drains  = g_q_drains.load(std::memory_order_relaxed);
    s.peak    = g_q_peak.load(std::memory_order_relaxed);
    return s;
}

// 递减本 loop 的在飞 IOCP 计数；若这是最后一个在飞 op 且 batcher 里
// 有滞留结果，立即 flush —— 串行/小并发负载不再为空闲超时买单。
// 须持 GIL 调用（flush 需要）。
static inline void complete_tracked_op(ResultBatcher *b) {
    if (b && b->op_completed() && b->has_pending()) b->flush();
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
// 缓存的文件大小
//
// Session::cachedFileSize 是 open 时的快照，之后只由本会话的 write/truncate
// 更新——别的进程（或同一进程的另一个句柄）追加/截断的数据，这里看不到。
// 把旧长度当成真相会让 read()/read_at() 静默少读（典型场景：tail 一个正在被
// 写的日志、等生产者落盘后再读）。
//
// 策略：只在**已经到 EOF 判决点**或**发生了短读**时问一次内核。这两处本来
// 就不是热路径（正常读满载而归，不碰缓存值），所以热路径的 syscall 数不变，
// 而「文件长大了」这件事在下一次操作就可见，不用调用方介入。
// ════════════════════════════════════════════════════════════════════════════

// 调用方须持有 s.posMtx
static inline void refresh_cached_size_locked(Session &s) {
    LARGE_INTEGER fs{};
    if (GetFileSizeEx(s.handle, &fs) && fs.QuadPart >= 0)
        s.cachedFileSize = static_cast<uint64_t>(fs.QuadPart);
}

// 自行加锁的版本，供不持 posMtx 的短读/失败路径使用
static inline void refresh_cached_size(Session &s) {
    std::lock_guard<std::mutex> lk(s.posMtx);
    refresh_cached_size_locked(s);
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

    // 6. Clean up batchers（连同退役的那些）
    //    析构里会 Py_DECREF（弱引用 / 回调），所以挪到锁外删——锁内析构可能
    //    级联执行任意 Python 代码并重取 GIL。
    {
        std::vector<std::unique_ptr<ResultBatcher>> doomed;
        {
            std::lock_guard<std::mutex> lk(m_batchersMtx);
            for (auto &kv : m_batchers) doomed.push_back(std::move(kv.second));
            m_batchers.clear();
            for (auto &b : m_retiredBatchers) doomed.push_back(std::move(b));
            m_retiredBatchers.clear();
        }
    }   // doomed 在锁外析构

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
                                     DWORD access, bool owns_fd) {
    auto &cfg = ayafileio::config();
    auto s = std::make_shared<Session>();
    s->id                     = m_nextSessionId.fetch_add(1, std::memory_order_relaxed);
    s->handle                 = h;
    s->loop                   = loop;
    Py_XINCREF(s->loop);   // ~Session 负责释放；构造函数那边不再替它保管
    s->batcher                = get_batcher(loop);
    s->create_future          = create_future;
    Py_INCREF(create_future);
    s->appendMode             = appendMode;
    s->access                 = access;
    s->owns_fd                = owns_fd;
    s->cached_buffer_size     = cfg.buffer_size();
    s->cached_buffer_pool_max = cfg.buffer_pool_max();
    s->cached_close_timeout_ms = cfg.close_timeout_ms();

    // Associate handle with the appropriate IOCP using sessionId as key
    HANDLE iocp = (access & GENERIC_WRITE) && !(access & GENERIC_READ)
                      ? m_writeIocp : m_readIocp;  // write-only → write IOCP; all others → read IOCP
    if (!CreateIoCompletionPort(h, iocp, (ULONG_PTR)s->id, 0)) {
        DWORD err = GetLastError();
        UR_DEBUG_LOG("create_session: CreateIoCompletionPort FAILED id=%llu h=%p err=%lu", s->id, h, err);
        CloseHandle(h);
        Py_DECREF(create_future);
        win_throw_os_error(err, "Failed to associate handle with IOCP");
    }
    s->skipCPOnSuccess = SetFileCompletionNotificationModes(h,
        FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0;

    // 文件大小**惰性**获取：这里不再无条件 GetFileSizeEx（每次 open 一次内核调用）。
    // 读取路径的 EOF 判决点与 seek(SEEK_END) 都会先 refresh_cached_size_locked()
    // （见 refresh_cached_size_locked / submit_read / submit_seek），第一次真正
    // 需要长度时会自动补上；而 append 模式必须立刻知道起始位置，所以那种情况
    // 仍然当场取。
    s->cachedFileSize = 0;
    if (s->appendMode) {
        LARGE_INTEGER fs{};
        if (GetFileSizeEx(h, &fs) && fs.QuadPart >= 0) {
            s->cachedFileSize = static_cast<uint64_t>(fs.QuadPart);
            s->filePos = s->cachedFileSize;
        }
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
    // 把 shared_ptr 挪到锁外再析构：~Session 会 Py_DECREF(create_future)，
    // 可能级联触发任意 Python 代码（如 loop.__del__），期间 GIL 可能被
    // 释放重取 —— 不能发生在持 m_sessionsMtx 时。
    std::shared_ptr<Session> victim;
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        auto it = m_sessions.find(id);
        if (it != m_sessions.end()) {
            victim = std::move(it->second);
            m_sessions.erase(it);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Batcher management
// ════════════════════════════════════════════════════════════════════════════

ResultBatcher *IOCPContext::get_batcher(PyObject *loop) {
    {
        std::lock_guard<std::mutex> lk(m_batchersMtx);
        auto it = m_batchers.find(loop);
        if (it != m_batchers.end()) {
            if (it->second->loop_is(loop)) return it->second.get();
            // 缓存键是裸 PyObject*：旧 loop 已死、新 loop 复用了同一个地址。
            // 旧 batcher 必须先退役——它的弱引用已经失效，继续用会把新 loop
            // 的完成结果全部静默丢掉。只退役不销毁：Session::batcher 与
            // snapshot_batchers() 可能仍持有这个裸指针。
            m_retiredBatchers.push_back(std::move(it->second));
            m_batchers.erase(it);
        }
    }
    // Create new batcher outside lock to avoid nested issues
    auto &cfg = ayafileio::config();
    std::unique_ptr<ResultBatcher> mine(
        new ResultBatcher(loop, cfg.iocp_batch_size(), 5));  // max_threshold, max_idle_timeout_ms
    mine->set_adaptive(cfg.adaptive_batch());
    mine->set_target_latency_us(cfg.adaptive_target_latency_us());

    std::lock_guard<std::mutex> lk(m_batchersMtx);
    auto it = m_batchers.find(loop);
    if (it != m_batchers.end()) return it->second.get();   // 竞态：别的线程先建好了
    m_batchers.emplace(loop, std::move(mine));
    return m_batchers.find(loop)->second.get();
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

std::vector<ResultBatcher*> IOCPContext::snapshot_batchers() {
    std::vector<ResultBatcher*> out;
    std::lock_guard<std::mutex> lk(m_batchersMtx);
    out.reserve(m_batchers.size());
    for (auto &kv : m_batchers) out.push_back(kv.second.get());
    return out;
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
        // quarantine 队首的到期时间也要算进等待超时，否则空闲时这个线程会
        // INFINITE 阻塞，最后一批完成的请求就一直挂在队里
        //（见 quarantine_timeout_ms 的说明）。
        const DWORD q_timeout = quarantine_timeout_ms();
        if (q_timeout < timeout) timeout = q_timeout;
        ULONG count = 0;

        BOOL ok = GetQueuedCompletionStatusEx(
            iocp, batch.data(), (ULONG)batch.size(), &count, timeout, FALSE);

        if (count == 0) {
            // Timeout or error — flush idle batchers, then check for shutdown
            {
                PyGILState_STATE gs = PyGILState_Ensure();
                // 空闲同样是 drain 的时机：放掉保留期已满的请求（连带它们的
                // future 以及 future 里对事件循环的引用）。只做 flush 会让
                // 内存停在上一次峰值，直到下一次有流量才回落。
                quarantine_drain();
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
        // Drain quarantine — IORequest objects that have been held
        // for QUARANTINE_MS are now safe to free.
        quarantine_drain();
        flush_batchers();
        PyGILState_Release(gs);
    }
    UR_DEBUG_LOG("worker_proc: exit iocp=%p", iocp);
}

void IOCPContext::process_one(uint64_t sessionId, IORequest *req,
                              DWORD bytes, DWORD err) {
    // ── Double‑delivery guard ────────────────────────────────────────────
    // MUST come first — if this is a duplicate IOCP delivery of an already-
    // freed IORequest, the CAS will fail (state is RESOLVED or REJECTED, not
    // PENDING) and we bail out without touching any of req's other fields.
    IOState expected = IOState::PENDING;
    if (!req->state.compare_exchange_strong(expected, IOState::RESOLVED)) {
        UR_DEBUG_LOG("process_one DOUBLE-DELIVERY req=%p sid=%llu state=%d — skipping",
                     (void*)req, sessionId, (int)expected);
        return;
    }
    // CAS succeeded — we own this completion exclusively, req is guaranteed alive
    if (!Yuyuko::check_access(req, __FILE__, __LINE__, __FUNCTION__)) return;

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
            long prev = session->complete_request();
            UR_DEBUG_LOG("process_one SYNC req=%p sid=%llu pending %ld→%ld — deleting",
                         (void*)req, sessionId, prev, prev - 1);
        } else {
            UR_DEBUG_LOG("process_one SYNC req=%p sid=%llu SESSION NOT FOUND — deleting req anyway",
                         (void*)req, sessionId);
        }
        complete_tracked_op(req->batcher);
        quarantine_push(req);
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
            complete_tracked_op(req->batcher);
            quarantine_push(req);
            return;
        }
        session = it->second;
    }

    long prev_pending = session->complete_request();
    UR_DEBUG_LOG("process_one ASYNC req=%p sid=%llu pending %ld→%ld err=%lu",
                 (void*)req, sessionId, prev_pending, prev_pending - 1, (unsigned long)err);

    ResultBatcher *batcher = session->batcher;
    if (!batcher) [[unlikely]] batcher = get_batcher(session->loop);

    PyObject *set_fn  = nullptr;
    PyObject *val     = nullptr;

    if (err == 0) {
        // ── success ───────────────────────────────────────────────────────
        set_fn = req->set_result;
        req->set_result = nullptr;

        switch (req->type) {
        case ReqType::Read:
            if (req->isReadinto) {
                val = PyLong_FromSsize_t((Py_ssize_t)bytes);
            } else if (req->preResult && (size_t)bytes == req->reqSize) {
                // 零拷贝：预建的 PyBytes 直接作为结果
                val = req->preResult;
                req->preResult = nullptr;
            } else {
                // 短读：拷贝实际字节数，preResult 随 req 释放
                val = PyBytes_FromStringAndSize(req->buf(), (Py_ssize_t)bytes);
            }
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
        // set_exception 未在提交时预取（罕见路径），此处持 GIL 按需获取
        set_fn = req->set_exception;
        req->set_exception = nullptr;
        if (!set_fn) {
            set_fn = PyObject_GetAttr(req->future, g_str_set_exception);
            if (!set_fn) PyErr_Clear();
        }

        PyObject *exc_class = map_win_error(err);
        val = PyObject_CallFunction(exc_class, "is", (int)err, "I/O operation failed");
    }

    if (set_fn && val) {
        bool threshold = batcher->push(set_fn, val);
        // 这是本 loop 最后一个在飞 op 时立即 flush，不再等空闲超时
        bool last = req->batcher ? req->batcher->op_completed() : false;
        if (threshold || last) [[unlikely]] batcher->flush();
    } else {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
        complete_tracked_op(req->batcher);
    }
    UR_DEBUG_LOG("process_one ASYNC req=%p sid=%llu — deferred free after push", (void*)req, sessionId);
    quarantine_push(req);
}

void IOCPContext::flush_batchers() {
    // Called with GIL held from worker thread.
    //
    // 关键：不能持着 m_batchersMtx 调 flush_if_idle() —— 它内部会调
    // call_soon_threadsafe，而 asyncio 的实现走 socket send()，期间会
    // 临时释放并重新获取 GIL。如果此时另一个线程正持着 GIL 等
    // m_batchersMtx（另一个 worker 的 flush_batchers，或主线程 open()
    // 里的 get_batcher），就会互等死锁（CI 上偶发的整进程卡死）。
    // 因此先在锁内拷出指针，锁外再调用。batcher 只在 shutdown 时销毁，
    // 而 shutdown 会先 join 掉本线程，所以锁外使用这些指针是安全的。
    std::vector<ResultBatcher*> local;
    {
        std::lock_guard<std::mutex> lk(m_batchersMtx);
        local.reserve(m_batchers.size());
        for (auto &kv : m_batchers) local.push_back(kv.second.get());
    }
    for (auto *b : local) b->flush_if_idle();
}

// ════════════════════════════════════════════════════════════════════════════
// IORequest factory (replaces IOBackendBase::make_req for the IOCP path)
// ════════════════════════════════════════════════════════════════════════════

namespace {

IORequest *make_req_iocp(size_t size, PyObject *future, ReqType type,
                         size_t buf_size, size_t /*pool_max*/) {
    auto *req = REQ_ALLOC();
    req->file        = nullptr;   // not used — completion routed via sessionId
    req->batcher = nullptr;   // not used — batcher handles dispatch
    req->future      = future;
    Py_INCREF(future);
    req->set_result    = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取：错误是罕见路径，process_one 出错时按需获取，
    // 成功路径每个 I/O 省一次属性查找
    req->reqSize = size;
    req->type    = type;

    if (type == ReqType::Read) {
        // 零拷贝读：直接读进预建的 PyBytes，完成时它就是结果对象，
        // 省一次完整数据拷贝。OOM 返回 nullptr（MemoryError 已设置）。
        req->preResult = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)size);
        if (!req->preResult) { REQ_FREE(req); return nullptr; }
    } else if (size <= buf_size)
        req->poolBuf = pool_acquire_with_size(size);
    else
        req->heapBuf = static_cast<char *>(std::malloc(size));

    UR_DEBUG_LOG("make_req_iocp req=%p future=%p type=%d size=%zu", (void*)req, (void*)future, (int)type, size);
    return req;
}

IORequest *make_req_readinto_iocp(PyObject *buf, Py_buffer *view, size_t size,
                                  PyObject *future) {
    auto *req = REQ_ALLOC();
    req->file        = nullptr;
    req->batcher = nullptr;
    req->future      = future;
    Py_INCREF(future);
    req->set_result    = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取，同 make_req_iocp
    req->reqSize   = size;
    req->type      = ReqType::Read;
    req->isReadinto = true;
    req->userBuf   = buf;
    Py_INCREF(buf);
    req->userBufView = *view;

    UR_DEBUG_LOG("make_req_readinto_iocp req=%p future=%p size=%zu", (void*)req, (void*)future, size);
    return req;
}

// 零拷贝写：自行 GetBuffer 持有调用方缓冲区视图（调用方返回后释放自己
// 的视图不影响我们），WriteFile 直接读用户内存，省一次完整拷贝。
// 失败返回 nullptr（Python 异常已设置）。
IORequest *make_req_write_iocp_held(Py_buffer *view, PyObject *future) {
    auto *req = REQ_ALLOC();
    req->file        = nullptr;
    req->batcher     = nullptr;
    req->future      = future;
    Py_INCREF(future);
    req->set_result  = PyObject_GetAttr(future, g_str_set_result);
    // set_exception 不预取，同 make_req_iocp
    req->reqSize     = static_cast<size_t>(view->len);
    req->type        = ReqType::Write;
    req->holdsWriteBuf = true;

    if (PyObject_GetBuffer(view->obj, &req->userBufView, PyBUF_SIMPLE) < 0) {
        REQ_FREE(req);
        return nullptr;
    }
    req->userBuf = view->obj;
    Py_INCREF(view->obj);

    UR_DEBUG_LOG("make_req_write_iocp_held req=%p future=%p size=%zu", (void*)req, (void*)future, req->reqSize);
    return req;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════════════
// I/O submission helpers
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::make_failed_future_no_session() {
    // Session gone — return an already-failed future so the caller never
    // sees a bare nullptr without an active exception. If loop/future
    // creation itself fails, a Python exception IS set, so returning nullptr
    // there is correct (the binding's py::python_error() will pick it up).
    PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) return nullptr;
    PyObject *cf = PyObject_GetAttr(loop, g_str_create_future);
    Py_DECREF(loop);
    if (!cf) return nullptr;
    PyObject *future = PyObject_CallNoArgs(cf);
    Py_DECREF(cf);
    if (!future) return nullptr;

    PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "File not open.");
    PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
    PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
    Py_XDECREF(r); Py_XDECREF(fn); Py_XDECREF(exc);
    return future;
}

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
    return (s->access & GENERIC_WRITE) &&
                   !(s->access & GENERIC_READ)
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
            // 可能只是缓存长度过期（外部追加），先问一次内核再判 EOF
            refresh_cached_size_locked(*s);
            rem = static_cast<int64_t>(s->cachedFileSize) - static_cast<int64_t>(s->filePos);
        }
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
    if (!req) { Py_DECREF(future); return nullptr; }  // MemoryError
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    req->batcher = s->batcher;
    req->batcher->op_submitted();
    DWORD got = 0;
    BOOL ok = ReadFile(s->handle, req->buf(), (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline.
        // skipCPOnSuccess 启用时内核不投完成包，由提交线程全权收尾；
        // 否则完成包到达后由 worker 递减 pending 并回收 req。
        UR_DEBUG_LOG("submit_read SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        PyObject *val;
        if ((size_t)got == readSize) {
            // 零拷贝：预建的 PyBytes 直接作为结果
            val = req->preResult;
            req->preResult = nullptr;
        } else {
            // 短读 = 真碰到 EOF（文件被外部截短/读穿了缓存长度）：
            // 把缓存长度拉回内核真值，下一次操作立刻正确
            refresh_cached_size(*s);
            // 短读：拷贝实际字节数，preResult 随 req 由 worker 释放
            val = PyBytes_FromStringAndSize(req->buf(), got);
        }
        PyObject *fn  = req->set_result;  // 提交时已预取，复用
        req->set_result = nullptr;
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_XDECREF(fn); Py_DECREF(val);
        if (s->skipCPOnSuccess) {
            // 内核不再投递完成包：pending 由提交线程递减，req 直接回收
            //（同步完成意味着内核已结束该 op，worker 无需参与）。
            // batcher 的 op_submitted 也必须在此配平——worker 见不到
            // 完成包，否则 m_outstanding 只增不减，"最后一个在飞 op 完成
            // 立即 flush"永久失效，异步完成的 op 全部退化到等空闲清扫
            // 周期（CI windows-2022 上实测 x1 随机读因此只有 144 ops/s）
            s->complete_request();
            complete_tracked_op(req->batcher);  // 配平记账；归零且有滞留结果时顺手 flush
            mark_sync_done(req);
            REQ_FREE(req);
        } else {
            // 回退路径：完成包仍会到达，worker 递减 pending 并回收 req
            mark_sync_done(req);
        }
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->complete_request();
            req->batcher->op_completed();
            UR_DEBUG_LOG("submit_read FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "ReadFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            REQ_FREE(req);
        } else {
            UR_DEBUG_LOG("submit_read ASYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        }
    }
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_read_at — 位置读（pread 语义）
//
// 与 submit_read 的唯一区别：offset 来自参数而非 s->filePos，
// 且绝不触碰 s->filePos —— 与并发的 read()/seek() 无竞争。
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_read_at(uint64_t session_id, int64_t offset, int64_t size) {
    auto s = get_session(session_id);
    if (!s) return make_failed_future_no_session();

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    if (offset < 0) {
        PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "negative offset not allowed");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    if (is_ctrlc_triggered() ||
        !s->running.load(std::memory_order_relaxed)) {
        PyObject *exc = PyObject_CallFunction(g_KeyboardInterrupt, "s", "interrupted");
        PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
        Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
        return future;
    }

    uint64_t roffset = static_cast<uint64_t>(offset);
    size_t   readSize;
    {
        // posMtx 仅为与 write/truncate 的 cachedFileSize 更新保持一致而持有；
        // 不读取也不修改 s->filePos。
        std::lock_guard<std::mutex> lk(s->posMtx);
        int64_t rem = static_cast<int64_t>(s->cachedFileSize) - offset;
        if (rem <= 0) {
            // 缓存长度可能过期（外部追加）：offset 看上去越界时先问内核
            refresh_cached_size_locked(*s);
            rem = static_cast<int64_t>(s->cachedFileSize) - offset;
        }
        if (rem <= 0) {
            // offset 越界（≥ 文件大小）→ pread 语义：空 bytes
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
    }

    IORequest *req = make_req_iocp(readSize, future, ReqType::Read,
                                   s->cached_buffer_size, s->cached_buffer_pool_max);
    if (!req) { Py_DECREF(future); return nullptr; }  // MemoryError
    req->ov.Offset     = (DWORD)(roffset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(roffset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    req->batcher = s->batcher;
    req->batcher->op_submitted();
    DWORD got = 0;
    BOOL ok = ReadFile(s->handle, req->buf(), (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline.
        // skipCPOnSuccess 启用时内核不投完成包，由提交线程全权收尾；
        // 否则完成包到达后由 worker 递减 pending 并回收 req。
        UR_DEBUG_LOG("submit_read_at SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        PyObject *val;
        if ((size_t)got == readSize) {
            // 零拷贝：预建的 PyBytes 直接作为结果
            val = req->preResult;
            req->preResult = nullptr;
        } else {
            // 短读 = 真碰到 EOF：校正缓存长度（见 refresh_cached_size）
            refresh_cached_size(*s);
            // 短读：拷贝实际字节数，preResult 随 req 由 worker 释放
            val = PyBytes_FromStringAndSize(req->buf(), got);
        }
        PyObject *fn  = req->set_result;  // 提交时已预取，复用
        req->set_result = nullptr;
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_XDECREF(fn); Py_DECREF(val);
        if (s->skipCPOnSuccess) {
            // 内核不再投递完成包：pending 由提交线程递减，req 直接回收
            //（同步完成意味着内核已结束该 op，worker 无需参与）。
            // batcher 的 op_submitted 也必须在此配平——worker 见不到
            // 完成包，否则 m_outstanding 只增不减，"最后一个在飞 op 完成
            // 立即 flush"永久失效，异步完成的 op 全部退化到等空闲清扫
            // 周期（CI windows-2022 上实测 x1 随机读因此只有 144 ops/s）
            s->complete_request();
            complete_tracked_op(req->batcher);  // 配平记账；归零且有滞留结果时顺手 flush
            mark_sync_done(req);
            REQ_FREE(req);
        } else {
            // 回退路径：完成包仍会到达，worker 递减 pending 并回收 req
            mark_sync_done(req);
        }
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->complete_request();
            req->batcher->op_completed();
            UR_DEBUG_LOG("submit_read_at FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "ReadFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            REQ_FREE(req);
        } else {
            UR_DEBUG_LOG("submit_read_at ASYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        }
    }
    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_write
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_write(uint64_t session_id, Py_buffer *view, int64_t position) {
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

    if (position >= 0 && s->appendMode) {
        PyErr_SetString(PyExc_ValueError, "write_at() does not support append mode");
        return nullptr;
    }
    // Native completion counts and submission lengths are 32-bit.
    if (static_cast<uint64_t>(view->len) > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "write buffer exceeds 4 GiB - 1");
        return nullptr;
    }

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

    uint64_t offset = static_cast<uint64_t>(position);
    if (position < 0) {
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

    if (position >= 0) {
        std::lock_guard<std::mutex> lk(s->posMtx);
        if (offset + wsize > s->cachedFileSize) s->cachedFileSize = offset + wsize;
    }

    IORequest *req = make_req_write_iocp_held(view, future);
    if (!req) { Py_DECREF(future); return nullptr; }
    req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    req->ov.OffsetHigh = (DWORD)(offset >> 32);

    long prev = s->pending.fetch_add(1, std::memory_order_relaxed);
    req->batcher = s->batcher;
    req->batcher->op_submitted();
    DWORD wrote = 0;
    BOOL ok = WriteFile(s->handle, req->buf(), (DWORD)wsize, &wrote, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline.
        // skipCPOnSuccess 启用时内核不投完成包，由提交线程全权收尾；
        // 否则完成包到达后由 worker 递减 pending 并回收 req。
        UR_DEBUG_LOG("submit_write SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        // 短写：上面的乐观 cachedFileSize 更新会高估真实长度，拉回内核真值
        if (static_cast<size_t>(wrote) != wsize) refresh_cached_size(*s);
        PyObject *val = PyLong_FromSsize_t((Py_ssize_t)wrote);
        PyObject *fn  = req->set_result;  // 提交时已预取，复用
        req->set_result = nullptr;
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_XDECREF(fn); Py_DECREF(val);
        if (s->skipCPOnSuccess) {
            // 内核不再投递完成包：pending 由提交线程递减，req 直接回收
            //（同步完成意味着内核已结束该 op，worker 无需参与）。
            // batcher 的 op_submitted 也必须在此配平——worker 见不到
            // 完成包，否则 m_outstanding 只增不减，"最后一个在飞 op 完成
            // 立即 flush"永久失效，异步完成的 op 全部退化到等空闲清扫
            // 周期（CI windows-2022 上实测 x1 随机读因此只有 144 ops/s）
            s->complete_request();
            complete_tracked_op(req->batcher);  // 配平记账；归零且有滞留结果时顺手 flush
            mark_sync_done(req);
            REQ_FREE(req);
        } else {
            // 回退路径：完成包仍会到达，worker 递减 pending 并回收 req
            mark_sync_done(req);
        }
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            // 写失败：提交前的乐观长度更新必须撤回，否则后续 read 会读到假 EOF 之后
            refresh_cached_size(*s);
            s->complete_request();
            req->batcher->op_completed();
            UR_DEBUG_LOG("submit_write FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "WriteFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            REQ_FREE(req);
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
    if (!s) return make_failed_future_no_session();

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    {
        std::lock_guard<std::mutex> lk(s->posMtx);
        int64_t new_pos;
        if (whence == 0)
            new_pos = offset;
        else if (whence == 1)
            new_pos = static_cast<int64_t>(s->filePos) + offset;
        else if (whence == 2) {
            // SEEK_END 问的就是「现在多长」：缓存值可能过期（外部追加）
            refresh_cached_size_locked(*s);
            new_pos = static_cast<int64_t>(s->cachedFileSize) + offset;
        } else {
            PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "Invalid whence value");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            return future;
        }

        // 负位置必须当场拒绝。原来的 `(uint64_t)offset` 会把 -1 变成 2^64-1：
        // 位置被静默污染（tell() 返回 18446744073709551615），之后每次 read()
        // 只报 `ReadFile failed, errno 87`——错误信息和真正的原因毫无关系。
        // 语义与 macos_gcd_backend 的 seek 保持一致（那边早就有这个校验）。
        if (new_pos < 0) {
            PyObject *exc = PyObject_CallFunction(g_ValueError, "s", "negative seek position");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            return future;
        }

        s->filePos = static_cast<uint64_t>(new_pos);
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
    if (!s) return make_failed_future_no_session();

    PyObject *future = check_session_closed(s);
    if (future) return future;

    future = PyObject_CallNoArgs(s->create_future);
    if (!future) return nullptr;

    PyObject *set_result = PyObject_GetAttr(future, g_str_set_result);
    if (!set_result) {
        Py_DECREF(future);
        return nullptr;
    }

    ResultBatcher *batcher = s->batcher;
    HANDLE handle = s->handle;

    // 与 submit_write 同一套记账：pending 让 close() 等在本 op 之后，
    // op_submitted 让 batcher 知道本 loop 还有一个在飞 op
    s->pending.fetch_add(1, std::memory_order_relaxed);
    batcher->op_submitted();

    GlobalThreadPool &pool = GlobalThreadPool::instance();
    pool.ensure_started(effective_io_worker_count());

    // FlushFileBuffers 是同步调用（慢盘实测 40–50 ms），不能留在事件循环
    // 线程上。丢给全局线程池，结果经 ResultBatcher 回投 —— 与 open_async
    // 同一条链路。此前已提交给驱动的写入由 FlushFileBuffers 一并落盘，
    // 因此这里不需要再等 pending 归零。
    std::shared_ptr<Session> kept = s;
    pool.enqueue([kept, handle, batcher, set_result]() mutable {
        BOOL ok = FlushFileBuffers(handle);
        DWORD err = ok ? 0 : GetLastError();

        PyGILState_STATE gs = PyGILState_Ensure();
        PyObject *val = nullptr;
        if (ok) {
            val = Py_None;
            Py_INCREF(val);
        } else {
            val = PyObject_CallFunction(g_OSError, "is", static_cast<int>(err),
                                        "FlushFileBuffers failed");
        }
        kept->complete_request();
        bool last = batcher->op_completed();
        if (val) {
            bool threshold = batcher->push(set_result, val);
            // push 接管 set_result 与 val 的所有权
            if (threshold || last) batcher->flush();
        } else {
            Py_DECREF(set_result);
            PyErr_Clear();
        }
        // Session 若在此刻是最后一个引用，其析构里的 DECREF 必须在持 GIL 下发生
        kept.reset();
        PyGILState_Release(gs);
    });

    return future;
}

// ════════════════════════════════════════════════════════════════════════════
// submit_tell
// ════════════════════════════════════════════════════════════════════════════

PyObject *IOCPContext::submit_tell(uint64_t session_id) {
    auto s = get_session(session_id);
    if (!s) return make_failed_future_no_session();

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
    if (!s) return make_failed_future_no_session();

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
    if (!s) return make_failed_future_no_session();

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
            // 缓存长度可能过期（外部追加）：判 EOF 前先问内核
            refresh_cached_size_locked(*s);
            rem = static_cast<int64_t>(s->cachedFileSize) - static_cast<int64_t>(s->filePos);
        }
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
    req->batcher = s->batcher;
    req->batcher->op_submitted();
    DWORD got = 0;
    BOOL ok = ReadFile(s->handle, view.buf, (DWORD)readSize, &got, &req->ov);
    if (ok) {
        // Synchronous completion — resolve inline.
        // skipCPOnSuccess 启用时内核不投完成包，由提交线程全权收尾；
        // 否则完成包到达后由 worker 递减 pending 并回收 req。
        UR_DEBUG_LOG("submit_readinto SYNC sid=%llu req=%p pending %ld→%ld", session_id, (void*)req, prev, prev+1);
        if ((size_t)got != readSize) refresh_cached_size(*s);  // 短读 → 校正缓存长度
        PyObject *val = PyLong_FromSsize_t((Py_ssize_t)got);
        PyObject *fn  = req->set_result;  // 提交时已预取，复用
        req->set_result = nullptr;
        PyObject *r   = PyObject_CallFunctionObjArgs(fn, val, nullptr);
        Py_XDECREF(r); Py_XDECREF(fn); Py_DECREF(val);
        if (s->skipCPOnSuccess) {
            // 内核不再投递完成包：pending 由提交线程递减，req 直接回收
            //（同步完成意味着内核已结束该 op，worker 无需参与）。
            // batcher 的 op_submitted 也必须在此配平——worker 见不到
            // 完成包，否则 m_outstanding 只增不减，"最后一个在飞 op 完成
            // 立即 flush"永久失效，异步完成的 op 全部退化到等空闲清扫
            // 周期（CI windows-2022 上实测 x1 随机读因此只有 144 ops/s）
            s->complete_request();
            complete_tracked_op(req->batcher);  // 配平记账；归零且有滞留结果时顺手 flush
            mark_sync_done(req);
            REQ_FREE(req);
        } else {
            // 回退路径：完成包仍会到达，worker 递减 pending 并回收 req
            mark_sync_done(req);
        }
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            s->complete_request();
            req->batcher->op_completed();
            UR_DEBUG_LOG("submit_readinto FAIL sid=%llu req=%p err=%lu — deleting", session_id, (void*)req, err);
            PyObject *exc = PyObject_CallFunction(g_OSError, "is", (int)err, "ReadFile failed");
            PyObject *fn  = PyObject_GetAttr(future, g_str_set_exception);
            PyObject *r   = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
            Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
            req->state.store(IOState::REJECTED, std::memory_order_relaxed);
            REQ_FREE(req);
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
    UR_DEBUG_LOG("submit_close sid=%llu h=%p pending=%ld owns_fd=%d",
                 session_id, s->handle, close_pending, s->owns_fd);

    if (s->handle != INVALID_HANDLE_VALUE) {
        if (close_pending > 0) {
            UR_DEBUG_LOG("submit_close sid=%llu — CancelIoEx + wait begin (timeout=%ums)",
                         session_id, s->cached_close_timeout_ms);
            CancelIoEx(s->handle, NULL);

            // 完成路径会释放 close_wake；收到信号立即复查，不再 Sleep 轮询。
            auto wait_started = std::chrono::steady_clock::now();
            auto deadline = wait_started
                + std::chrono::milliseconds(s->cached_close_timeout_ms);
            while (s->pending.load(std::memory_order_acquire) > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
                if (remain.count() <= 0) break;
                Py_BEGIN_ALLOW_THREADS
                [[maybe_unused]] bool acquired = s->close_wake.try_acquire_for(remain);
                Py_END_ALLOW_THREADS
            }
            [[maybe_unused]] long final_pending = s->pending.load(std::memory_order_acquire);
            [[maybe_unused]] auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wait_started).count();
            UR_DEBUG_LOG("submit_close sid=%llu — wait done elapsed=%lldms final_pending=%ld",
                         session_id, static_cast<long long>(elapsed), final_pending);
        }

        if (s->owns_fd) {
            // IOCP-associated handles are closed directly: the kernel IOCP
            // association cannot be revoked, so a recycled HANDLE would fail
            // CreateIoCompletionPort with err=87 (see docs/CHANGES.md 1.3.0).
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
                CloseHandle(s->handle);
            }
            s->handle = INVALID_HANDLE_VALUE;
        }
    }

    std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
    m_sessions.clear();
}

// ════════════════════════════════════════════════════════════════════════════
// 回归测试/诊断：quarantine 深度与 drain 扫描量（原 bindings.cpp）
//
// 持续负载下 live 应稳定在 (吞吐 × QUARANTINE_MS) 量级，scanned 应按 op 数线性
// 增长（摊销 O(1)），而不是随队列深度平方增长。quarantine 本身是 IOCP 专属
// （io_uring / GCD 后端没有这层延迟释放），所以只有这个文件提供它。
// ════════════════════════════════════════════════════════════════════════════

py::dict py_debug_quarantine_stats() {
    QuarantineStats s = quarantine_stats();
    py::dict d;
    d["live"]    = s.live;
    d["pushes"]  = s.pushes;
    d["scanned"] = s.scanned;
    d["drains"]  = s.drains;
    d["peak"]    = s.peak;
    return d;
}

} // namespace ayafileio
