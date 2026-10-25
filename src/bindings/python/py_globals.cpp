#include "py_globals.hpp"

#include "batcher_registry.hpp"
#include "../../backend/builtin.hpp"
#include "../../core/global_thread_pool.hpp"
#include "../../diagnostics/yuyuko.hpp"

namespace ayafileio::python {

PyObject* g_OSError = nullptr;
PyObject* g_FileNotFoundError = nullptr;
PyObject* g_FileExistsError = nullptr;
PyObject* g_PermissionError = nullptr;
PyObject* g_ValueError = nullptr;
PyObject* g_KeyboardInterrupt = nullptr;
PyObject* g_InvalidStateError = nullptr;
PyObject* g_UnsupportedOperation = nullptr;
PyObject* g_get_running_loop = nullptr;
PyObject* g_str_set_result = nullptr;
PyObject* g_str_set_exception = nullptr;
PyObject* g_str_create_future = nullptr;
PyObject* g_str_call_soon_ts = nullptr;
std::atomic<unsigned> g_worker_count{0};

void cache_globals() {
    auto* builtins = PyImport_ImportModule("builtins");
    if (!builtins) throw nb::python_error();
    g_OSError = PyObject_GetAttrString(builtins, "OSError");
    g_FileNotFoundError = PyObject_GetAttrString(builtins, "FileNotFoundError");
    g_FileExistsError = PyObject_GetAttrString(builtins, "FileExistsError");
    g_PermissionError = PyObject_GetAttrString(builtins, "PermissionError");
    g_ValueError = PyObject_GetAttrString(builtins, "ValueError");
    g_KeyboardInterrupt = PyObject_GetAttrString(builtins, "KeyboardInterrupt");
    Py_DECREF(builtins);

    auto* asyncio = PyImport_ImportModule("asyncio");
    if (!asyncio) throw nb::python_error();
    g_get_running_loop = PyObject_GetAttrString(asyncio, "get_running_loop");
    auto* asyncio_exc = PyImport_ImportModule("asyncio.exceptions");
    if (asyncio_exc) {
        g_InvalidStateError = PyObject_GetAttrString(asyncio_exc, "InvalidStateError");
        Py_DECREF(asyncio_exc);
    }
    Py_DECREF(asyncio);

    auto* io = PyImport_ImportModule("io");
    if (!io) throw nb::python_error();
    g_UnsupportedOperation = PyObject_GetAttrString(io, "UnsupportedOperation");
    Py_DECREF(io);

    g_str_set_result = PyUnicode_InternFromString("set_result");
    g_str_set_exception = PyUnicode_InternFromString("set_exception");
    g_str_create_future = PyUnicode_InternFromString("create_future");
    g_str_call_soon_ts = PyUnicode_InternFromString("call_soon_threadsafe");
}

void py_init_module() {
    cache_globals();
    diagnostics::init_request_hooks();
    backend::register_builtin_backends();
    core::set_completion_scheduler(&BatcherRegistry::instance());
}

void py_cleanup() {
    {
        GilRelease release;
        backend::shutdown_builtin_backends();
        core::GlobalThreadPool::instance().shutdown();
    }
    clear_batchers();
    core::set_completion_scheduler(nullptr);
}

void set_worker_count(unsigned count) {
    if (count == 0 || (count >= 1 && count <= 128)) {
        g_worker_count.store(count);
    } else {
        throw nb::value_error("worker count must be 0 (auto) or 1-128");
    }
}

} // namespace ayafileio::python
