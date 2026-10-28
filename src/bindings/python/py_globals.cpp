#include "py_globals.hpp"

#include "batcher_registry.hpp"
#include "../../backend/builtin.hpp"
#include "../../core/completion.hpp"
#include "../../core/global_thread_pool.hpp"
#include "../../diagnostics/yuyuko.hpp"

namespace ayafileio::python {
namespace {

// 后端 worker 线程的宿主锚定：线程启动时建一次 thread state，之后一直留着。
// 不锚定的话，每次完成都要 PyGILState_Ensure/Release 一轮，free-threading 构建上
// 那意味着每个 op 重建一次 thread state —— 实测 1.3M cycles/op（~350 µs）。
// 锚定后 per-op 只剩抢一次 GIL（实测 ~40 ns）。
thread_local PyThreadState* g_anchor_tstate = nullptr;
thread_local PyGILState_STATE g_anchor_state{};

void py_worker_attach() {
    if (g_anchor_tstate) return;
    g_anchor_state = PyGILState_Ensure();    // 建 tstate，计数 0->1，持 GIL
    g_anchor_tstate = PyEval_SaveThread();   // 放掉 GIL，但 tstate 留着
}

void py_worker_detach() {
    if (!g_anchor_tstate) return;
    PyEval_RestoreThread(g_anchor_tstate);   // 重新持 GIL
    g_anchor_tstate = nullptr;
    PyGILState_Release(g_anchor_state);      // 计数 1->0，此时才销毁 tstate
}

} // namespace

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
    core::set_worker_thread_hooks(&py_worker_attach, &py_worker_detach);
}

void py_cleanup() {
    {
        GilRelease release;
        backend::shutdown_builtin_backends();
        core::GlobalThreadPool::instance().shutdown();
    }
    core::set_worker_thread_hooks(nullptr, nullptr);
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
