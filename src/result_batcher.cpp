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
                    PyErr_Print();
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
            PyErr_Print();
            std::lock_guard<std::mutex> lk(m_mtx);
            for (auto &e : m_batch) {
                Py_DECREF(e.set_fn);
                Py_DECREF(e.val);
            }
            m_batch.clear();
            m_dispatch_pending.store(false, std::memory_order_relaxed);
            m_has_pending = false;
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
    if (!m_has_pending || m_batch.empty()) return INFINITE;

    auto elapsed = std::chrono::steady_clock::now() - m_first_push;
    long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    long long remaining = static_cast<long long>(m_idle_timeout_ms) - elapsed_ms;

    if (remaining <= 0) return 0;        // already expired
    if (remaining > INFINITE - 1) return INFINITE - 1;  // clamp
    return static_cast<DWORD>(remaining);
}
