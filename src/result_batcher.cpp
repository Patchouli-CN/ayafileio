#include "result_batcher.hpp"
#include <nanobind/nanobind.h>

namespace py = nanobind;

ResultBatcher::ResultBatcher(PyObject *loop, size_t threshold,
                             unsigned idle_timeout_ms)
    : m_threshold(threshold), m_idle_timeout_ms(idle_timeout_ms) {

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

bool ResultBatcher::push(PyObject *set_fn, PyObject *val) {
    if (!set_fn || !val) {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
        return false;
    }

    bool threshold_reached = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_has_pending) {
            m_first_push = std::chrono::steady_clock::now();
            m_has_pending = true;
        }
        m_batch.push_back({set_fn, val});
        threshold_reached = (m_batch.size() >= m_threshold);
    }
    return threshold_reached;
}

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

    if (need_schedule) {
        PyObject *r = PyObject_CallFunctionObjArgs(m_call_soon_ts, m_drain_cb, nullptr);
        if (!r) {
            // call_soon_threadsafe failed — do NOT drop the batch entries;
            // that would leave futures unresolved and hang coroutines.
            // Reset dispatch_pending so the next flush() retries scheduling.
            PyErr_Print();  // also clears the error indicator
            std::lock_guard<std::mutex> lk(m_mtx);
            m_dispatch_pending.store(false, std::memory_order_relaxed);
            // leave m_batch and m_has_pending intact for retry
        } else {
            Py_DECREF(r);
        }
    }
}

bool ResultBatcher::has_pending() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_has_pending && !m_batch.empty();
}

bool ResultBatcher::idle_expired() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_has_pending || m_batch.empty()) return false;
    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
           >= static_cast<long long>(m_idle_timeout_ms);
}

DWORD ResultBatcher::get_timeout_ms() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_has_pending || m_batch.empty()) return (DWORD)-1;  // INFINITE

    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    long long remaining = static_cast<long long>(m_idle_timeout_ms) - elapsed_ms;

    if (remaining <= 0) return 0;        // already expired
    if (remaining > (long long)((DWORD)-1) - 1) return (DWORD)-1 - 1;  // clamp
    return static_cast<DWORD>(remaining);
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
    auto *b = new ResultBatcher(loop, 64, 5);
    g_batchers.emplace_back(loop, b);
    return b;
}

void clear_batchers() {
    std::lock_guard<std::mutex> lk(g_batchersMtx);
    for (auto &kv : g_batchers) delete kv.second;
    g_batchers.clear();
}
