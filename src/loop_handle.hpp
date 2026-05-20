#pragma once
#include "globals.hpp"
#include <atomic>
#include <mutex>
#include <vector>

// LoopHandle - batched call_soon_threadsafe dispatcher

struct BatchEntry {
    PyObject *set_fn;  // owned
    PyObject *val;     // owned
};

struct LoopHandle {
    PyObject             *call_soon_ts  = nullptr;  // owned
    PyObject             *drain_cb      = nullptr;  // owned
    std::mutex            batch_mtx;
    std::vector<BatchEntry> batch;
    std::atomic<bool>     dispatch_pending{false};

    explicit LoopHandle(PyObject *loop);
    ~LoopHandle();

    // GIL must be held. Ownership of set_fn and val is transferred here.
    void push(PyObject *set_fn, PyObject *val);
};

LoopHandle *get_or_create_loop_handle(PyObject *loop);
void clear_loop_handles();
