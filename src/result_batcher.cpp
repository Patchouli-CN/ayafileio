#include "result_batcher.hpp"
#include "config.hpp"
#ifdef _WIN32
#include "iocp_context.hpp"   // snapshot_batchers(): the IOCP per-loop registry
#endif
#include <nanobind/nanobind.h>
#include <cmath>
#include <memory>
#include <vector>

namespace ayafileio {

namespace {

// 取回弱引用指向的对象（**新**引用）；referent 已死或出错返回 nullptr。
// 3.13+ 用 PyWeakref_GetRef（PyWeakref_GetObject 自 3.13 起废弃），
// 旧版本走 GetObject + 手工 INCREF。
PyObject *weakref_target(PyObject *ref) {
#if PY_VERSION_HEX >= 0x030D0000
    PyObject *obj = nullptr;
    if (PyWeakref_GetRef(ref, &obj) != 1) {   // 0 = referent 已死，-1 = error
        PyErr_Clear();
        return nullptr;
    }
    return obj;
#else
    PyObject *obj = PyWeakref_GetObject(ref);  // borrowed；死后返回 Py_None
    if (!obj || obj == Py_None) return nullptr;
    Py_INCREF(obj);
    return obj;
#endif
}

} // namespace

ResultBatcher::ResultBatcher(PyObject *loop, size_t threshold,
                             unsigned idle_timeout_ms)
    : m_threshold_max(threshold), m_idle_timeout_max_ms(idle_timeout_ms) {

    m_current_threshold.store(threshold, std::memory_order_relaxed);
    m_current_idle_ms.store(idle_timeout_ms, std::memory_order_relaxed);

    // 弱引用：batcher 不能把事件循环对象钉住（见头文件里的说明）
    m_loop_ref = PyWeakref_NewRef(loop, nullptr);
    if (!m_loop_ref) {
        throw std::runtime_error("Failed to create weakref to the event loop");
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
    PyObject *loop = weakref_target(m_loop_ref);
    if (!loop) {
        // 事件循环对象已经没了。这些结果再没有谁能消费（future 绑定在那个
        // loop 上），留着只会把 future——以及 future 里对 loop 的强引用——
        // 一直压在 m_batch 里。直接丢弃并复位派发位。
        std::vector<BatchEntry> dead;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            dead.swap(m_batch);
            m_has_pending = false;
            m_dispatch_pending.store(false, std::memory_order_relaxed);
        }
        for (auto &e : dead) {
            Py_XDECREF(e.set_fn);
            Py_XDECREF(e.val);
        }
        return;
    }

    // 不缓存 loop.call_soon_threadsafe：那是个持有 loop 的绑定方法（见头文件）。
    // 每次 flush 一次属性查找，与一次 call_soon_threadsafe 相比可忽略。
    PyObject *r = PyObject_CallMethod(loop, "call_soon_threadsafe", "O", m_drain_cb);
    Py_DECREF(loop);
    if (!r) {
        PyErr_Print();
        std::lock_guard<std::mutex> lk(m_mtx);
        m_dispatch_pending.store(false, std::memory_order_relaxed);
    } else {
        Py_DECREF(r);
    }
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

// ════════════════════════════════════════════════════════════════════════════
// Global batcher registry (shared by non-IOCP backends)
// ════════════════════════════════════════════════════════════════════════════

static std::mutex                                           g_batchersMtx;
static std::vector<std::pair<PyObject*, ResultBatcher*>>    g_batchers;
// 退役的 batcher：地址失效（旧 loop 已死、新 loop 复用了同一个 PyObject*），
// 但 flush_batchers()/refresh_batchers_config() 可能正拿着 snapshot 出来的裸
// 指针在用，所以**只退役不销毁**，统一在 clear_batchers()（shutdown 路径）
// 释放——与「batcher 只在 shutdown 时销毁」这条既有不变量保持一致。
static std::vector<ResultBatcher*>                          g_retired;

// 锁内调用：返回指向 loop 且仍然有效的 batcher；过期的挪进 g_retired。
static ResultBatcher *find_live_batcher_locked(PyObject *loop) {
    for (auto &kv : g_batchers) {
        if (kv.first != loop || !kv.second) continue;
        if (kv.second->loop_is(loop)) return kv.second;
        g_retired.push_back(kv.second);
        kv.first  = nullptr;
        kv.second = nullptr;
    }
    return nullptr;
}

ResultBatcher *get_or_create_batcher(PyObject *loop) {
    {
        std::lock_guard<std::mutex> lk(g_batchersMtx);
        if (ResultBatcher *hit = find_live_batcher_locked(loop)) return hit;
    }
    // 在锁外构造：ResultBatcher 构造函数会执行 Python 调用（创建弱引用、
    // 创建 cpp_function），可能触发 GC 进而释放/重取 GIL —— 不能发生在
    // 持 g_batchersMtx 时，否则会与其他 GIL 持有者互等死锁。
    auto &cfg = ayafileio::config();
    std::unique_ptr<ResultBatcher> mine(
        new ResultBatcher(loop, cfg.iocp_batch_size(), 5));
    mine->set_adaptive(cfg.adaptive_batch());
    mine->set_target_latency_us(cfg.adaptive_target_latency_us());

    ResultBatcher *result = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_batchersMtx);
        if (ResultBatcher *hit = find_live_batcher_locked(loop)) {
            result = hit;                     // 竞态：别的线程已注册（有效）
        } else {
            g_batchers.emplace_back(loop, mine.get());
            result = mine.release();
        }
        // 清掉退役后留下的空槽，避免 g_batchers 随 loop 生灭无限增长
        g_batchers.erase(
            std::remove_if(g_batchers.begin(), g_batchers.end(),
                           [](const auto &kv) { return kv.second == nullptr; }),
            g_batchers.end());
    }
    return result;   // mine 若未 release，在这里（锁外）析构
}

void clear_batchers() {
    std::vector<ResultBatcher *> victims;
    {
        std::lock_guard<std::mutex> lk(g_batchersMtx);
        for (auto &kv : g_batchers) {
            if (kv.second) victims.push_back(kv.second);
        }
        victims.insert(victims.end(), g_retired.begin(), g_retired.end());
        g_batchers.clear();
        g_retired.clear();
    }
    // 锁外析构：~ResultBatcher 里有 Py_DECREF，可能级联执行任意 Python 代码
    for (auto *b : victims) delete b;
}

void refresh_batchers_config() {
    auto &cfg = ayafileio::config();
    const size_t   threshold = cfg.iocp_batch_size();
    const bool     adaptive  = cfg.adaptive_batch();
    const unsigned target_us = cfg.adaptive_target_latency_us();

    // 只写原子量，不触发任何 Python 调用，因此不涉及那条
    // 「持 m_batchersMtx 调 call_soon_threadsafe → 与 GIL 互等」的死锁路径。
    const auto apply = [&](ResultBatcher *b) {
        if (!b) return;
        b->set_threshold_max(threshold);
        b->set_adaptive(adaptive);
        b->set_target_latency_us(target_us);
    };

#ifdef _WIN32
    for (auto *b : IOCPContext::instance().snapshot_batchers()) apply(b);
#endif
    std::vector<ResultBatcher *> local;
    {
        std::lock_guard<std::mutex> lk(g_batchersMtx);
        local.reserve(g_batchers.size());
        for (auto &kv : g_batchers) local.push_back(kv.second);
    }
    for (auto *b : local) apply(b);
}

} // namespace ayafileio
