#pragma once
#ifdef _WIN32

#include "globals.hpp"
#include "io_request.hpp"
#include "result_batcher.hpp"
#include "handle_pool.hpp"
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <thread>
#include <vector>

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
    PyObject       *loop = nullptr;          // borrowed reference
    PyObject       *create_future = nullptr; // owned
    std::atomic<long> pending{0};
    bool            owns_fd = true;
    PoolKey         poolKey;

    // Cached file state (avoids GetFileSizeEx syscall on every read)
    uint64_t        cachedFileSize = 0;  // updated on write/truncate, read under posMtx

    // Cached config values (snapshot at open time)
    size_t          cached_buffer_size = 65536;
    size_t          cached_buffer_pool_max = 512;
    unsigned        cached_close_timeout_ms = 4000;

    ~Session() {
        Py_XDECREF(create_future);
        // loop is borrowed — no DECREF
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
                            bool appendMode, const PoolKey &poolKey, bool owns_fd);
    std::shared_ptr<Session> get_session(uint64_t id) const;
    void remove_session(uint64_t id);

    // ── I/O submission (called from WindowsIOBackend) ─────────────────────
    PyObject* submit_read(uint64_t session_id, int64_t size);
    PyObject* submit_write(uint64_t session_id, Py_buffer *view);
    PyObject* submit_seek(uint64_t session_id, int64_t offset, int whence);
    PyObject* submit_flush(uint64_t session_id);
    PyObject* submit_close(uint64_t session_id);
    PyObject* submit_tell(uint64_t session_id);
    PyObject* submit_truncate(uint64_t session_id, int64_t size);
    PyObject* submit_readinto(uint64_t session_id, PyObject *buf);

    // ── batcher management ────────────────────────────────────────────────
    ResultBatcher* get_batcher(PyObject *loop);

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

    // ── Ctrl+C ────────────────────────────────────────────────────────────
    std::atomic<bool> m_ctrlcTriggered{false};

    // ── workers ───────────────────────────────────────────────────────────
    void worker_proc(HANDLE iocp);
    void process_one(uint64_t sessionId, IORequest *req, DWORD bytes, DWORD err);
    void flush_batchers();

    // ── internal helpers ──────────────────────────────────────────────────
    PyObject* check_session_closed(const std::shared_ptr<Session> &s);
    HANDLE iocp_for_session(const std::shared_ptr<Session> &s);
};

#endif // _WIN32
