#pragma once
#include "globals.hpp"
#include <atomic>
#include <array>
#include <algorithm>
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
// ResultBatcher — adaptive dual-trigger completion batching
//
//   1. Count threshold: flush when batch.size() >= m_threshold (adaptive)
//   2. Idle timeout:    flush when oldest entry older than idle_timeout_ms
//
// Adaptive mode (enabled by default):
//   - Tracks inter-completion intervals via a 128-entry ring buffer
//   - Dynamically adjusts the effective threshold based on the
//     rolling median interval between completed I/O operations:
//
//       threshold = target_latency_us / median_interval_us
//
//   - Fast disk (NVMe, ~10μs intervals): threshold rises → batch more, fewer
//     call_soon_threadsafe calls, lower GIL pressure
//   - Slow disk (HDD, ~10ms intervals): threshold drops → flush sooner,
//     no wasted waiting for completions that haven't arrived yet
//
//   The idle timeout also adapts: min(5ms, median_interval * 4) for fast disks,
//   capped at the configured idle_timeout_ms for slow disks.
//
// Thread safety: push() and flush() must be called with GIL held.
// has_pending(), idle_expired(), get_timeout_ms() are lock-free safe.
// ─────────────────────────────────────────────────────────────────────────────
class ResultBatcher {
public:
    ResultBatcher(PyObject *loop, size_t threshold = 64,
                  unsigned idle_timeout_ms = 5);
    ~ResultBatcher();

    // ── adaptive tuning ────────────────────────────────────────────────────
    void set_adaptive(bool enabled)          { m_adaptive.store(enabled, std::memory_order_relaxed); }
    bool adaptive() const                    { return m_adaptive.load(std::memory_order_relaxed); }
    void set_target_latency_us(unsigned us)  { m_target_latency_us.store(us, std::memory_order_relaxed); }
    unsigned target_latency_us() const       { return m_target_latency_us.load(std::memory_order_relaxed); }

    // ── data path (GIL required) ──────────────────────────────────────────
    bool push(PyObject *set_fn, PyObject *val);
    void flush();
    void flush_if_idle();   // 单次持锁完成 判空+判超时+置位，等价于
                            // has_pending() && idle_expired() ? flush() : no-op

    // ── queries for worker thread (no GIL, thread-safe) ───────────────────
    bool has_pending() const;
    bool idle_expired() const;
    DWORD get_timeout_ms() const;

    // ── stats (for debugging / monitoring) ─────────────────────────────────
    size_t current_threshold() const { return m_current_threshold.load(std::memory_order_relaxed); }
    unsigned current_idle_ms()  const { return m_current_idle_ms.load(std::memory_order_relaxed); }
    double median_interval_us() const;

private:
    PyObject *m_call_soon_ts = nullptr;
    PyObject *m_drain_cb     = nullptr;

    mutable std::mutex m_mtx;
    std::vector<BatchEntry> m_batch;
    std::atomic<bool> m_dispatch_pending{false};
    std::chrono::steady_clock::time_point m_first_push;  // guarded by m_mtx
    std::chrono::steady_clock::time_point m_last_push;   // guarded by m_mtx
    bool m_has_pending{false};                            // guarded by m_mtx

    // ── fixed config ──────────────────────────────────────────────────────
    size_t   m_threshold_max;         // upper bound from config (e.g., 256)
    unsigned m_idle_timeout_max_ms;   // upper bound from config (e.g., 5ms)

    // ── adaptive state ────────────────────────────────────────────────────
    std::atomic<bool>     m_adaptive{true};
    std::atomic<unsigned> m_target_latency_us{1000};  // target max extra latency
    std::atomic<size_t>   m_current_threshold{64};
    std::atomic<unsigned> m_current_idle_ms{5};

    static constexpr size_t RING_SIZE = 128;
    static constexpr size_t ADAPTIVE_UPDATE_INTERVAL = 16;  // 每 N 次 push 重算一次中位数
    std::array<uint64_t, RING_SIZE> m_intervals_us{};  // ring buffer, guarded by m_mtx
    size_t   m_ring_pos{0};
    size_t   m_ring_count{0};
    size_t   m_pushes_since_update{0};                 // guarded by m_mtx

    // ── internal ──────────────────────────────────────────────────────────
    void update_adaptive_locked();
    void schedule_drain();  // call_soon_threadsafe(m_drain_cb)，锁外调用
};

// ── Global batcher registry (one per event loop, for non-IOCP backends) ─────
ResultBatcher *get_or_create_batcher(PyObject *loop);
void clear_batchers();
