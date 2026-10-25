#include "py_api.hpp"

#include "batcher_registry.hpp"
#include "../../backend/builtin.hpp"
#include "../../core/global_thread_pool.hpp"
#ifdef _WIN32
#include "../../backend/windows/iocp_context.hpp"
#endif

namespace ayafileio::python {
namespace {

PyObject* current_loop() {
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw nb::python_error();
    return loop;
}

} // namespace

long py_debug_batcher_outstanding() {
    PyObject* loop = current_loop();
    auto* batcher = get_or_create_batcher(loop);
    Py_DECREF(loop);
    return batcher ? batcher->outstanding() : 0L;
}

nb::dict py_debug_batcher_state() {
    PyObject* loop = current_loop();
    auto* batcher = get_or_create_batcher(loop);
    Py_DECREF(loop);
    nb::dict result;
    if (!batcher) return result;
    result["outstanding"] = batcher->outstanding();
    result["threshold"] = batcher->current_threshold();
    result["threshold_max"] = batcher->threshold_max();
    result["idle_ms"] = batcher->current_idle_ms();
    result["idle_max_ms"] = batcher->idle_timeout_max_ms();
    result["adaptive"] = batcher->adaptive();
    result["target_latency_us"] = batcher->target_latency_us();
    result["median_interval_us"] = batcher->median_interval_us();
    return result;
}

unsigned py_debug_pool_workers() {
    return core::GlobalThreadPool::instance().running_workers();
}

#ifdef _WIN32
nb::dict py_debug_quarantine_stats() {
    const auto stats = backend::windows::quarantine_stats();
    nb::dict result;
    result["live"] = stats.live;
    result["pushes"] = stats.pushes;
    result["scanned"] = stats.scanned;
    result["drains"] = stats.drains;
    result["peak"] = stats.peak;
    return result;
}
#endif

} // namespace ayafileio::python
