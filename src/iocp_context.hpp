#pragma once
#ifdef _WIN32

#include "globals.hpp"
#include "io_request.hpp"
#include "result_batcher.hpp"
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <thread>
#include <vector>
#include <semaphore>

namespace ayafileio {

// ─────────────────────────────────────────────────────────────────────────────
// Session — per-file state owned by IOCPContext
//
// Each open file gets one Session. The sessionId is used as the IOCP
// completion key, replacing the raw "this" pointer so that completions
// arriving after close() can be detected safely.
// ─────────────────────────────────────────────────────────────────────────────
struct Session {
    uint64_t        id;
    HANDLE          handle = INVALID_HANDLE_VALUE;
    std::atomic<bool> running{false};
    uint64_t        filePos = 0;
    std::mutex      posMtx;
    bool            appendMode = false;
    PyObject       *loop = nullptr;          // owned（create_session INCREF，~Session DECREF）
    ResultBatcher  *batcher = nullptr;       // owned by IOCPContext::m_batchers；
                                             // 打开时解析缓存，完成热路径免锁免查表
    PyObject       *create_future = nullptr; // owned
    std::atomic<long> pending{0};
    std::counting_semaphore<INT32_MAX> close_wake{0};
    bool            owns_fd = true;
    // GENERIC_READ/GENERIC_WRITE 组合：决定 session 挂到 read 还是 write IOCP
    DWORD           access = 0;

    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 是否成功启用。
    // 启用时同步完成的 op 不再投递 IOCP 完成包：提交线程必须自己递减
    // pending 并回收 req（worker 永远见不到它们）。设置失败则维持
    // 旧行为（worker 收包排水），两条路径严格二选一，否则会重复递减。
    bool            skipCPOnSuccess = false;

    // Cached file state (avoids GetFileSizeEx syscall on every read)
    uint64_t        cachedFileSize = 0;  // updated on write/truncate, read under posMtx

    // Cached config values (snapshot at open time)
    size_t          cached_buffer_size = 65536;
    size_t          cached_buffer_pool_max = 512;
    unsigned        cached_close_timeout_ms = 4000;

    long complete_request() {
        long prev = pending.fetch_sub(1, std::memory_order_release);
        close_wake.release();
        return prev;
    }

    ~Session() {
        Py_XDECREF(create_future);
        // 与 create_session 里的 Py_INCREF 配平。此前 loop 记成 borrowed 却
        // 由构造函数白送一个引用（注释说是 Session 的「续命来源」），
        // 结果每开一个文件就永久钉住一个事件循环对象——loop.close() 之后
        // 它依然回收不了。所有权必须单向：Session 持有，Session 释放。
        // 与 create_future 一样，析构必须在持 GIL 时发生（remove_session
        // 把 shared_ptr 挪到锁外再析构，正是为了这一点）。
        Py_XDECREF(loop);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// IOCPContext — global singleton, owns all Windows I/O resources
//
//   - Dual IOCP handles (read / write) with dedicated worker threads
//   - Session map (sessionId → shared_ptr<Session>) with shared_mutex
//   - ResultBatcher map (loop → batcher) for reducing call_soon_threadsafe
//     pressure via count-threshold + idle-timeout dual trigger
//   - Ctrl+C / shutdown lifecycle
// ─────────────────────────────────────────────────────────────────────────────
class IOCPContext {
public:
    static IOCPContext& instance();

    // ── lifecycle ─────────────────────────────────────────────────────────
    void init();
    void shutdown();

    // ── IOCP handles (for HANDLE association) ─────────────────────────────
    HANDLE read_iocp()  const { return m_readIocp; }
    HANDLE write_iocp() const { return m_writeIocp; }
    bool   is_running() const { return m_running.load(std::memory_order_acquire); }

    // ── session management ────────────────────────────────────────────────
    uint64_t create_session(HANDLE h, PyObject *loop, PyObject *create_future,
                            bool appendMode, DWORD access, bool owns_fd);
    std::shared_ptr<Session> get_session(uint64_t id) const;
    void remove_session(uint64_t id);

    // ── I/O submission (called from WindowsIOBackend) ─────────────────────
    PyObject* submit_read(uint64_t session_id, int64_t size);
    PyObject* submit_read_at(uint64_t session_id, int64_t offset, int64_t size);
    PyObject* submit_write(uint64_t session_id, Py_buffer *view, int64_t position = -1);
    PyObject* submit_seek(uint64_t session_id, int64_t offset, int whence);
    PyObject* submit_flush(uint64_t session_id);
    PyObject* submit_close(uint64_t session_id);
    PyObject* submit_tell(uint64_t session_id);
    PyObject* submit_truncate(uint64_t session_id, int64_t size);
    PyObject* submit_readinto(uint64_t session_id, PyObject *buf);

    // ── batcher management ────────────────────────────────────────────────
    ResultBatcher* get_batcher(PyObject *loop);
    // 当前所有 per-loop batcher 的快照（configure() 热更新用）。
    // 锁内只拷贝裸指针，锁外再调用 —— 与 flush_batchers() 同一套理由。
    std::vector<ResultBatcher*> snapshot_batchers();

    // ── Ctrl+C / emergency close ──────────────────────────────────────────
    void trigger_ctrlc() { m_ctrlcTriggered.store(true, std::memory_order_relaxed); }
    bool is_ctrlc_triggered() const {
        return m_ctrlcTriggered.load(std::memory_order_relaxed);
    }
    void close_all_sessions();

    // ── timeout helper (for worker thread GQCS) ───────────────────────────
    DWORD get_min_batcher_timeout_ms();

private:
    IOCPContext() = default;
    ~IOCPContext();
    IOCPContext(const IOCPContext&) = delete;
    IOCPContext& operator=(const IOCPContext&) = delete;

    // ── IOCP handles ──────────────────────────────────────────────────────
    HANDLE m_readIocp  = NULL;
    HANDLE m_writeIocp = NULL;
    std::atomic<bool> m_running{false};
    std::thread m_readWorker;
    std::thread m_writeWorker;

    // ── sessions ──────────────────────────────────────────────────────────
    mutable std::shared_mutex m_sessionsMtx;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> m_sessions;
    std::atomic<uint64_t> m_nextSessionId{1};

    // ── batchers (one per event loop) ─────────────────────────────────────
    std::mutex m_batchersMtx;
    std::unordered_map<PyObject*, std::unique_ptr<ResultBatcher>> m_batchers;
    // 退役的 batcher：缓存键（loop 的地址）被新 loop 复用后不再使用，但
    // Session::batcher 与 snapshot_batchers() 都可能还握着它的裸指针，
    // 所以只退役不销毁，统一在 shutdown() 释放（与「batcher 只在 shutdown
    // 时销毁」这条既有不变量一致）。见 get_batcher()。
    std::vector<std::unique_ptr<ResultBatcher>> m_retiredBatchers;

    // ── Ctrl+C ────────────────────────────────────────────────────────────
    std::atomic<bool> m_ctrlcTriggered{false};

    // ── workers ───────────────────────────────────────────────────────────
    void worker_proc(HANDLE iocp);
    void process_one(uint64_t sessionId, IORequest *req, DWORD bytes, DWORD err);
    void flush_batchers();

    // ── internal helpers ──────────────────────────────────────────────────
    PyObject* check_session_closed(const std::shared_ptr<Session> &s);
    HANDLE iocp_for_session(const std::shared_ptr<Session> &s);

    // Build an already-failed future (ValueError) for the case where the
    // session no longer exists. Used by submit_* so we never return nullptr
    // without an active Python exception (which would surface as SystemError).
    PyObject* make_failed_future_no_session();
};

// ── Quarantine instrumentation (debug / regression tests only) ──────────────
// The quarantine holds completed IORequests for QUARANTINE_MS before freeing
// them, so under sustained load its depth converges to (rate × window).  The
// drain cost has to stay amortised O(1) per entry; `scanned` is the number of
// ring entries the drain loop looked at, which is the number an O(n) drain
// implementation would inflate without bound.  Relaxed atomics only — no
// ordering is required for these counters.
struct QuarantineStats {
    std::uint64_t live     = 0;  // currently held (this process, all worker threads)
    std::uint64_t pushes   = 0;
    std::uint64_t scanned  = 0;  // entries inspected by drain loops
    std::uint64_t drains   = 0;  // drain invocations
    std::uint64_t peak     = 0;  // high-water mark of `live`
};

QuarantineStats quarantine_stats();

} // namespace ayafileio

#endif // _WIN32
