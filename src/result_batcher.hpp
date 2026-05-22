#pragma once
#include "globals.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// BatchEntry — one pending future result
// ─────────────────────────────────────────────────────────────────────────────
struct BatchEntry {
    PyObject *set_fn;  // owned (future.set_result or future.set_exception)
    PyObject *val;     // owned (result value or exception object)
};

// ─────────────────────────────────────────────────────────────────────────────
// ResultBatcher — dual-trigger completion batching
//   1. Count threshold: flush when batch.size() >= m_threshold
//   2. Idle timeout:   flush when oldest entry older than m_idle_timeout_ms
//
// Used by all backends.  The IOCP path benefits from threshold+idle-timeout
// batching via periodic flush_batchers() calls from the worker thread.
// Non-Windows backends call push() + flush() per-completion (same behavior as
// the old LoopHandle) but still inherit retry-on-failure and
// InvalidStateError suppression.
//
// Thread safety: push() and flush() must be called with GIL held.
// has_pending(), idle_expired(), get_timeout_ms() are lock-free safe.
// ─────────────────────────────────────────────────────────────────────────────
class ResultBatcher {
public:
    ResultBatcher(PyObject *loop, size_t threshold = 64,
                  unsigned idle_timeout_ms = 5);
    ~ResultBatcher();

    // ── data path (GIL required) ──────────────────────────────────────────
    bool push(PyObject *set_fn, PyObject *val);
    void flush();

    // ── queries for worker thread (no GIL, thread-safe) ───────────────────
    bool has_pending() const;
    bool idle_expired() const;
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

// ── Global batcher registry (one per event loop, for non-IOCP backends) ─────
// Returns an existing batcher for the given loop, or creates a new one.
// The returned pointer is stable until clear_batchers() is called.
ResultBatcher *get_or_create_batcher(PyObject *loop);
void clear_batchers();
