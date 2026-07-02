#include "result_batcher.hpp"
#include "config.hpp"
#include <nanobind/nanobind.h>
#include <cmath>

namespace py = nanobind;

ResultBatcher::ResultBatcher(PyObject *loop, size_t threshold,
                             unsigned idle_timeout_ms)
    : m_threshold_max(threshold), m_idle_timeout_max_ms(idle_timeout_ms) {

    m_current_threshold.store(threshold, std::memory_order_relaxed);
    m_current_idle_ms.store(idle_timeout_ms, std::memory_order_relaxed);

    m_call_soon_ts = PyObject_GetAttr(loop, g_str_call_soon_ts);
    if (!m_call_soon_ts) {
        throw std::runtime_error("Failed to get call_soon_threadsafe from loop");
    }

    py::object fn = py::cpp_function([this]() {
        std::vector<BatchEntry> local;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            local.swap(m_batch);
            m_dispatch_pending.store(false, std::memory_order_release);
            m_has_pending = false;
        }
        for (auto &e : local) {
            if (e.set_fn && e.val) {
                PyObject *r = PyObject_CallFunctionObjArgs(e.set_fn, e.val, nullptr);
                if (!r) {
                    // Future was cancelled (e.g. Ctrl+C cleanup) — silently discard
                    if (g_InvalidStateError &&
                        PyErr_ExceptionMatches(g_InvalidStateError)) {
                        PyErr_Clear();
                    } else {
                        PyErr_Print();
                    }
                } else {
                    Py_DECREF(r);
                }
                Py_DECREF(e.set_fn);
                Py_DECREF(e.val);
            }
        }
    });
    m_drain_cb = fn.release().ptr();
}

ResultBatcher::~ResultBatcher() {
    Py_XDECREF(m_call_soon_ts);
    Py_XDECREF(m_drain_cb);
    for (auto &e : m_batch) {
        Py_XDECREF(e.set_fn);
        Py_XDECREF(e.val);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Adaptive threshold calculation
//
// On each push(), we record the inter-completion interval (time since the
// last push).  After collecting enough samples, we compute the rolling
// median and derive:
//
//   dynamic_threshold = target_latency_us / median_interval_us
//   dynamic_idle_ms    = min(idle_max, max(1, median_us / 250))
//
// The idle timeout adapts too: on fast disks, wait up to 5ms for a batch
// to fill; on slow disks, use a shorter timeout proportional to the I/O rate
// so we don't add unnecessary latency.
//
// Clamping:
//   threshold: [1, m_threshold_max]
//   idle_ms:   [1, m_idle_timeout_max_ms]
// ════════════════════════════════════════════════════════════════════════════

void ResultBatcher::update_adaptive_locked() {
    if (!m_adaptive.load(std::memory_order_relaxed)) return;
    if (m_ring_count < 4) return;  // need at least a few samples

    // ── Compute median of intervals in ring buffer ────────────────────────
    std::array<uint64_t, RING_SIZE> sorted;
    size_t n = std::min(m_ring_count, RING_SIZE);
    for (size_t i = 0; i < n; ++i)
        sorted[i] = m_intervals_us[i];

    auto mid = sorted.begin() + n / 2;
    std::nth_element(sorted.begin(), mid, sorted.begin() + n);
    uint64_t median_us = *mid;
    if (n % 2 == 0 && n > 1) {
        // Even count: average of two middle elements
        auto mid2 = std::max_element(sorted.begin(), mid);
        median_us = (median_us + *mid2) / 2;
    }
    if (median_us == 0) median_us = 1;  // prevent div-by-zero

    // ── Dynamic threshold ─────────────────────────────────────────────────
    unsigned target_us = m_target_latency_us.load(std::memory_order_relaxed);
    size_t dyn_threshold = static_cast<size_t>(
        std::llround(static_cast<double>(target_us) / static_cast<double>(median_us)));
    if (dyn_threshold < 1)  dyn_threshold = 1;
    if (dyn_threshold > m_threshold_max) dyn_threshold = m_threshold_max;
    m_current_threshold.store(dyn_threshold, std::memory_order_relaxed);

    // ── Dynamic idle timeout ──────────────────────────────────────────────
    // Scale with the median interval: ~4× the median, clamped.
    unsigned dyn_idle = static_cast<unsigned>(median_us * 4 / 1000);
    if (dyn_idle < 1) dyn_idle = 1;
    if (dyn_idle > m_idle_timeout_max_ms) dyn_idle = m_idle_timeout_max_ms;
    m_current_idle_ms.store(dyn_idle, std::memory_order_relaxed);
}

// ════════════════════════════════════════════════════════════════════════════
// push — record timing + check adaptive threshold
// ════════════════════════════════════════════════════════════════════════════

bool ResultBatcher::push(PyObject *set_fn, PyObject *val) {
    if (!set_fn || !val) {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    bool threshold_reached = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);

        // ── Record inter-completion interval ──────────────────────────────
        if (m_ring_count > 0) {
            auto delta_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - m_last_push).count());
            // Cap at 1 second to avoid outliers dominating the median
            if (delta_us > 1'000'000) delta_us = 1'000'000;
            m_intervals_us[m_ring_pos] = delta_us;
            m_ring_pos = (m_ring_pos + 1) % RING_SIZE;
            if (m_ring_count < RING_SIZE) m_ring_count++;
        } else {
            // First push — seed the ring buffer with a neutral value
            m_intervals_us[0] = 1000;  // 1ms default assumption
            m_ring_pos = 1;
            m_ring_count = 1;
        }
        m_last_push = now;

        // ── Batch state ───────────────────────────────────────────────────
        if (!m_has_pending) {
            m_first_push = now;
            m_has_pending = true;
        }
        m_batch.push_back({set_fn, val});

        // ── Update adaptive threshold ─────────────────────────────────────
        // 中位数是慢变量：每 ADAPTIVE_UPDATE_INTERVAL 次完成重算一次即可，
        // 避免每个完成都在持锁 + 持 GIL 状态下做 O(RING_SIZE) 的 nth_element
        if (++m_pushes_since_update >= ADAPTIVE_UPDATE_INTERVAL) {
            m_pushes_since_update = 0;
            update_adaptive_locked();
        }

        size_t effective_threshold = m_adaptive.load(std::memory_order_relaxed)
            ? m_current_threshold.load(std::memory_order_relaxed)
            : m_threshold_max;
        threshold_reached = (m_batch.size() >= effective_threshold);
    }
    return threshold_reached;
}

// ════════════════════════════════════════════════════════════════════════════
// flush
// ════════════════════════════════════════════════════════════════════════════

void ResultBatcher::flush() {
    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_batch.empty()) return;
        if (!m_dispatch_pending.load(std::memory_order_relaxed)) {
            m_dispatch_pending.store(true, std::memory_order_relaxed);
            need_schedule = true;
        }
    }

    if (need_schedule) schedule_drain();
}

void ResultBatcher::flush_if_idle() {
    // 与 has_pending() + idle_expired() + flush() 等价，但只加一次锁，
    // 供 worker 每轮批处理后的 flush_batchers() 热路径使用。
    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_has_pending || m_batch.empty()) return;

        unsigned effective_idle_ms = m_adaptive.load(std::memory_order_relaxed)
            ? m_current_idle_ms.load(std::memory_order_relaxed)
            : m_idle_timeout_max_ms;

        auto elapsed = std::chrono::steady_clock::now() - m_first_push;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                < static_cast<long long>(effective_idle_ms))
            return;

        if (!m_dispatch_pending.load(std::memory_order_relaxed)) {
            m_dispatch_pending.store(true, std::memory_order_relaxed);
            need_schedule = true;
        }
    }

    if (need_schedule) schedule_drain();
}

void ResultBatcher::schedule_drain() {
    PyObject *r = PyObject_CallFunctionObjArgs(m_call_soon_ts, m_drain_cb, nullptr);
    if (!r) {
        PyErr_Print();
        std::lock_guard<std::mutex> lk(m_mtx);
        m_dispatch_pending.store(false, std::memory_order_relaxed);
    } else {
        Py_DECREF(r);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Queries
// ════════════════════════════════════════════════════════════════════════════

bool ResultBatcher::has_pending() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_has_pending && !m_batch.empty();
}

bool ResultBatcher::idle_expired() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_has_pending || m_batch.empty()) return false;

    unsigned effective_idle_ms = m_adaptive.load(std::memory_order_relaxed)
        ? m_current_idle_ms.load(std::memory_order_relaxed)
        : m_idle_timeout_max_ms;

    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
           >= static_cast<long long>(effective_idle_ms);
}

DWORD ResultBatcher::get_timeout_ms() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_has_pending || m_batch.empty()) return (DWORD)-1;  // INFINITE

    unsigned effective_idle_ms = m_adaptive.load(std::memory_order_relaxed)
        ? m_current_idle_ms.load(std::memory_order_relaxed)
        : m_idle_timeout_max_ms;

    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    long long remaining = static_cast<long long>(effective_idle_ms) - elapsed_ms;

    if (remaining <= 0) return 0;
    if (remaining > (long long)((DWORD)-1) - 1) return (DWORD)-1 - 1;
    return static_cast<DWORD>(remaining);
}

double ResultBatcher::median_interval_us() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_ring_count == 0) return 0.0;

    std::array<uint64_t, RING_SIZE> sorted;
    size_t n = std::min(m_ring_count, RING_SIZE);
    for (size_t i = 0; i < n; ++i)
        sorted[i] = m_intervals_us[i];

    auto mid = sorted.begin() + n / 2;
    std::nth_element(sorted.begin(), mid, sorted.begin() + n);
    double median = static_cast<double>(*mid);
    if (n % 2 == 0 && n > 1) {
        auto mid2 = std::max_element(sorted.begin(), mid);
        median = (median + static_cast<double>(*mid2)) / 2.0;
    }
    return median;
}

// ════════════════════════════════════════════════════════════════════════════
// Global batcher registry (shared by non-IOCP backends)
// ════════════════════════════════════════════════════════════════════════════

static std::mutex                                           g_batchersMtx;
static std::vector<std::pair<PyObject*, ResultBatcher*>>    g_batchers;

ResultBatcher *get_or_create_batcher(PyObject *loop) {
    std::lock_guard<std::mutex> lk(g_batchersMtx);
    for (auto &kv : g_batchers) {
        if (kv.first == loop) return kv.second;
    }
    auto &cfg = ayafileio::config();
    auto *b = new ResultBatcher(loop, cfg.iocp_batch_size(), 5);
    b->set_adaptive(cfg.adaptive_batch());
    b->set_target_latency_us(cfg.adaptive_target_latency_us());
    g_batchers.emplace_back(loop, b);
    return b;
}

void clear_batchers() {
    std::lock_guard<std::mutex> lk(g_batchersMtx);
    for (auto &kv : g_batchers) delete kv.second;
    g_batchers.clear();
}
