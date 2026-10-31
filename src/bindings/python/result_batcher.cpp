#include "result_batcher.hpp"
#include "../../core/config.hpp"
#include "../../diagnostics/yuyuko/mem_watch.hpp"
#include <cstdlib>
#include <nanobind/nanobind.h>
#include <cmath>
#include <memory>
#include <vector>

namespace ayafileio::python {

ResultBatcher::ResultBatcher(PyObject *loop, size_t threshold,
                             unsigned idle_timeout_ms)
    : m_threshold_max(threshold), m_idle_timeout_max_ms(idle_timeout_ms) {

    m_current_threshold.store(threshold, std::memory_order_relaxed);
    m_current_idle_ms.store(idle_timeout_ms, std::memory_order_relaxed);

    // ── 可选：用 x64 数据断点监视这把 mutex 的写入者 ──────────────────────
    // 崩溃现场（msvcp140!_Mtx_lock，rcx=0x70）指向 get_timeout_ms() 锁 m_mtx 时
    // 该字段已是垃圾值，说明它被**别处写坏**了。CRC 类工具只能事后发现"内容变了"，
    // 数据断点能在写入指令执行后当场报告"谁写的"（模块+偏移+线程）。
    //
    // 默认关闭；设 AYAFILEIO_WATCH_BATCHER=1 才装，因为硬件最多 4 个断点、
    // 且命中后会摘除该槽。
    static const bool watch_batcher = std::getenv("AYAFILEIO_WATCH_BATCHER") != nullptr;
    if (watch_batcher) {
        diagnostics::yuyuko::mem_watch(&m_mtx, sizeof(void*), "ResultBatcher::m_mtx");
    }

    nb::object on_loop_collected = nb::cpp_function([this](nb::handle) {
        std::lock_guard<std::mutex> lock(m_scheduler_mtx);
        m_scheduler_lifetime.reset();
    });
    // 弱引用：batcher 不能把事件循环对象钉住（见头文件里的说明）
    m_loop_ref = PyWeakref_NewRef(loop, on_loop_collected.ptr());
    if (!m_loop_ref) {
        throw std::runtime_error("Failed to create weakref to the event loop");
    }

    nb::object fn = nb::cpp_function([this]() {
        std::vector<BatchEntry> local;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            local.swap(m_batch);
            m_dispatch_pending.store(false, std::memory_order_release);
            m_has_pending = false;
        }
        if (std::getenv("AYAFILEIO_DEBUG_WHOLE")) {
            std::fprintf(stderr, "[DEBUG-whole] drain this=%p entries=%zu\n",
                         static_cast<void*>(this), local.size());
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
    Py_XDECREF(m_loop_ref);
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

void ResultBatcher::set_threshold_max(size_t value) {
    if (value < 1) value = 1;
    if (value > 256) value = 256;
    m_threshold_max.store(value, std::memory_order_relaxed);
    // 调小上界时旧的更大 current_threshold 会继续生效到下一次自适应重算，
    // 这里立刻夹紧，configure() 的效果对调用方是立即可见的。
    size_t current = m_current_threshold.load(std::memory_order_relaxed);
    if (current > value) m_current_threshold.store(value, std::memory_order_relaxed);
}

void ResultBatcher::set_idle_timeout_max_ms(unsigned value) {
    if (value < 1) value = 1;
    m_idle_timeout_max_ms.store(value, std::memory_order_relaxed);
    unsigned current = m_current_idle_ms.load(std::memory_order_relaxed);
    if (current > value) m_current_idle_ms.store(value, std::memory_order_relaxed);
}

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
    const size_t threshold_cap = m_threshold_max.load(std::memory_order_relaxed);
    if (dyn_threshold > threshold_cap) dyn_threshold = threshold_cap;
    m_current_threshold.store(dyn_threshold, std::memory_order_relaxed);

    // ── Dynamic idle timeout ──────────────────────────────────────────────
    // Scale with the median interval: ~4× the median, clamped.
    unsigned dyn_idle = static_cast<unsigned>(median_us * 4 / 1000);
    if (dyn_idle < 1) dyn_idle = 1;
    const unsigned idle_cap = m_idle_timeout_max_ms.load(std::memory_order_relaxed);
    if (dyn_idle > idle_cap) dyn_idle = idle_cap;
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
        if (std::getenv("AYAFILEIO_DEBUG_WHOLE")) {
            std::fprintf(stderr, "[DEBUG-whole] push this=%p entries=%zu\n",
                         static_cast<void*>(this), m_batch.size());
        }

        // ── Update adaptive threshold ─────────────────────────────────────
        // 中位数是慢变量：每 ADAPTIVE_UPDATE_INTERVAL 次完成重算一次即可，
        // 避免每个完成都在持锁 + 持 GIL 状态下做 O(RING_SIZE) 的 nth_element
        if (++m_pushes_since_update >= ADAPTIVE_UPDATE_INTERVAL) {
            m_pushes_since_update = 0;
            update_adaptive_locked();
        }

        size_t effective_threshold = m_adaptive.load(std::memory_order_relaxed)
            ? m_current_threshold.load(std::memory_order_relaxed)
            : m_threshold_max.load(std::memory_order_relaxed);
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
            : m_idle_timeout_max_ms.load(std::memory_order_relaxed);

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
    auto discard = [&]() {
        std::vector<BatchEntry> dead;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            dead.swap(m_batch);
            m_has_pending = false;
            m_dispatch_pending.store(false, std::memory_order_relaxed);
        }
        for (auto &entry : dead) {
            Py_XDECREF(entry.set_fn);
            Py_XDECREF(entry.val);
        }
    };

    PyObject *loop = weakref_target(m_loop_ref);
    if (!loop) {
        discard();
        return;
    }

    if (std::getenv("AYAFILEIO_DEBUG_WHOLE")) {
        std::fprintf(stderr,
                     "[DEBUG-whole] schedule this=%p entries=%zu outstanding=%ld dispatch=%d\n",
                     static_cast<void*>(this), m_batch.size(), m_outstanding.load(),
                     static_cast<int>(m_dispatch_pending.load()));
    }
    PyObject *r = PyObject_CallMethod(loop, "call_soon_threadsafe", "O", m_drain_cb);
    if (!r) {
        PyObject *type = nullptr;
        PyObject *value = nullptr;
        PyObject *traceback = nullptr;
        PyErr_Fetch(&type, &value, &traceback);
        PyObject *closed = PyObject_CallMethod(loop, "is_closed", nullptr);
        const bool is_closed = !closed || PyObject_IsTrue(closed) == 1;
        Py_XDECREF(closed);
        Py_DECREF(loop);
        if (is_closed) {
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
            PyErr_Clear();
            discard();
            return;
        }
        PyErr_Restore(type, value, traceback);
        PyErr_Print();
        std::lock_guard<std::mutex> lk(m_mtx);
        m_dispatch_pending.store(false, std::memory_order_relaxed);
        return;
    }

    Py_DECREF(r);
    Py_DECREF(loop);
}

bool ResultBatcher::loop_is(PyObject *loop) const {
    PyObject *live = weakref_target(m_loop_ref);
    const bool same = (live == loop);
    Py_XDECREF(live);
    return same;
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
        : m_idle_timeout_max_ms.load(std::memory_order_relaxed);

    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
           >= static_cast<long long>(effective_idle_ms);
}

DWORD ResultBatcher::get_timeout_ms() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_has_pending || m_batch.empty()) return (DWORD)-1;  // INFINITE

    unsigned effective_idle_ms = m_adaptive.load(std::memory_order_relaxed)
        ? m_current_idle_ms.load(std::memory_order_relaxed)
        : m_idle_timeout_max_ms.load(std::memory_order_relaxed);

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

} // namespace ayafileio::python
