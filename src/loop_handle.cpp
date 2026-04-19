#include "loop_handle.hpp"
#include <nanobind/nanobind.h>

namespace py = nanobind;

LoopHandle::LoopHandle(PyObject *loop) {
    call_soon_ts = PyObject_GetAttr(loop, g_str_call_soon_ts);
    if (!call_soon_ts) {
        throw std::runtime_error("Failed to get call_soon_threadsafe from loop");
    }
    batch.reserve(64);

    py::object fn = py::cpp_function([this]() {
        std::vector<BatchEntry> local;
        {
            std::lock_guard<std::mutex> lk(batch_mtx);
            local.swap(batch);
            dispatch_pending.store(false, std::memory_order_release);
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
    drain_cb = fn.release().ptr();
}

LoopHandle::~LoopHandle() {
    Py_XDECREF(call_soon_ts);
    Py_XDECREF(drain_cb);
    // 清理可能残留的 batch 条目
    for (auto &e : batch) {
        Py_XDECREF(e.set_fn);
        Py_XDECREF(e.val);
    }
}

void LoopHandle::push(PyObject *set_fn, PyObject *val) {
    if (!set_fn || !val) {
        Py_XDECREF(set_fn);
        Py_XDECREF(val);
        return;
    }
    
    bool need_schedule = false;
    {
        std::lock_guard<std::mutex> lk(batch_mtx);
        batch.push_back({set_fn, val});
        if (!dispatch_pending.load(std::memory_order_relaxed)) {
            dispatch_pending.store(true, std::memory_order_relaxed);
            need_schedule = true;
        }
    }
    
    if (need_schedule) {
        PyObject *r = PyObject_CallFunctionObjArgs(call_soon_ts, drain_cb, nullptr);
        if (!r) {
            // 调度失败：打印错误并清理已入队的回调，防止内存泄漏
            PyErr_Print();
            std::lock_guard<std::mutex> lk(batch_mtx);
            for (auto &e : batch) {
                Py_DECREF(e.set_fn);
                Py_DECREF(e.val);
            }
            batch.clear();
            dispatch_pending.store(false, std::memory_order_relaxed);
        } else {
            Py_DECREF(r);
        }
    }
}

static std::mutex                                      g_loopsMtx;
static std::vector<std::pair<PyObject*, LoopHandle*>>  g_loops;

LoopHandle *get_or_create_loop_handle(PyObject *loop) {
    std::lock_guard<std::mutex> lk(g_loopsMtx);
    for (auto &kv : g_loops) {
        if (kv.first == loop) {
            return kv.second;
        }
    }
    auto *h = new LoopHandle(loop);
    g_loops.emplace_back(loop, h);
    return h;
}