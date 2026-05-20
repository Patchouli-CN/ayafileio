#pragma once
#include "globals.hpp"
#include "loop_handle.hpp"   // for BatchEntry
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ResultBatcher — dual-trigger completion batching
//   1. Count threshold: flush when batch.size() >= m_threshold
//   2. Idle timeout:   flush when oldest entry older than m_idle_timeout_ms
//
// Thread safety: push() and flush() must be called with GIL held.
// has_pending(), idle_expired(), get_timeout_ms() are safe to call from
// worker threads (no GIL required).
// ─────────────────────────────────────────────────────────────────────────────
class ResultBatcher {
public:
    ResultBatcher(PyObject *loop, size_t threshold = 64,
                  unsigned idle_timeout_ms = 5);
    ~ResultBatcher();

    // ── data path (GIL required) ──────────────────────────────────────────
    // Transfer ownership of set_fn and val. Returns true if threshold reached.
    bool push(PyObject *set_fn, PyObject *val);

    // Schedule drain callback via call_soon_threadsafe. GIL required.
    void flush();

    // ── queries for worker thread (no GIL, thread-safe) ───────────────────
    bool has_pending() const;
    bool idle_expired() const;

    // Recommended GQCS timeout: remaining idle window, or INFINITE if empty.
    DWORD get_timeout_ms() const;

private:
    PyObject *m_call_soon_ts = nullptr;
    PyObject *m_drain_cb     = nullptr;

    mutable std::mutex m_mtx;
    std::vector<BatchEntry> m_batch;
    std::atomic<bool> m_dispatch_pending{false};
    std::chrono::steady_clock::time_point m_first_push;  // guarded by m_mtx
    bool m_has_pending{false};                            // guarded by m_mtx

    size_t   m_threshold;
    unsigned m_idle_timeout_ms;
};
