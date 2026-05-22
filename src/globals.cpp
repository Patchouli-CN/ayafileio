#include "globals.hpp"

PyObject *g_OSError           = nullptr;
PyObject *g_FileNotFoundError = nullptr;
PyObject *g_FileExistsError   = nullptr;
PyObject *g_PermissionError   = nullptr;
PyObject *g_ValueError        = nullptr;
PyObject *g_KeyboardInterrupt = nullptr;
PyObject *g_InvalidStateError = nullptr;
PyObject *g_get_running_loop  = nullptr;

PyObject *g_str_set_result    = nullptr;
PyObject *g_str_set_exception = nullptr;
PyObject *g_str_create_future = nullptr;
PyObject *g_str_call_soon_ts  = nullptr;

// 全局 worker count（0 = 自动）
std::atomic<unsigned> g_worker_count{0};

void set_worker_count(unsigned count) {
    if (count == 0 || (count >= 1 && count <= 128)) {
        g_worker_count.store(count);
    } else {
        throw std::runtime_error("worker count must be 0 (auto) or 1-128");
    }
}

void cache_globals() {
    auto *builtins = PyImport_ImportModule("builtins");
    g_OSError           = PyObject_GetAttrString(builtins, "OSError");
    g_FileNotFoundError = PyObject_GetAttrString(builtins, "FileNotFoundError");
    g_FileExistsError   = PyObject_GetAttrString(builtins, "FileExistsError");
    g_PermissionError   = PyObject_GetAttrString(builtins, "PermissionError");
    g_ValueError        = PyObject_GetAttrString(builtins, "ValueError");
    g_KeyboardInterrupt = PyObject_GetAttrString(builtins, "KeyboardInterrupt");
    Py_DECREF(builtins);

    auto *asyncio = PyImport_ImportModule("asyncio");
    g_get_running_loop = PyObject_GetAttrString(asyncio, "get_running_loop");

    auto *asyncio_exc = PyImport_ImportModule("asyncio.exceptions");
    if (asyncio_exc) {
        g_InvalidStateError = PyObject_GetAttrString(asyncio_exc, "InvalidStateError");
        Py_DECREF(asyncio_exc);
    }

    Py_DECREF(asyncio);

    g_str_set_result    = PyUnicode_InternFromString("set_result");
    g_str_set_exception = PyUnicode_InternFromString("set_exception");
    g_str_create_future = PyUnicode_InternFromString("create_future");
    g_str_call_soon_ts  = PyUnicode_InternFromString("call_soon_threadsafe");
}

#ifdef _WIN32
[[noreturn]] void win_throw_os_error(DWORD err, const char *msg, const char *filename) {
    PyObject *cls = map_win_error(err);

    const char *error_msg = msg;
    char custom_msg[512];

    if (filename) {
        switch (err) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                snprintf(custom_msg, sizeof(custom_msg),
                         "No such file or directory: '%s'", filename);
                break;
            case ERROR_FILE_EXISTS:
                snprintf(custom_msg, sizeof(custom_msg),
                         "File exists: '%s'", filename);
                break;
            case ERROR_ACCESS_DENIED:
                snprintf(custom_msg, sizeof(custom_msg),
                         "Permission denied: '%s'", filename);
                break;
            default:
                snprintf(custom_msg, sizeof(custom_msg),
                         "%s: '%s'", msg, filename);
                break;
        }
        error_msg = custom_msg;
    }
    // Python 会根据平台自动添加 [Errno %d] 或 [WinError %d]，不要手动加
    PyObject *exc = PyObject_CallFunction(cls, "is", (int)err, error_msg);
    PyErr_SetObject((PyObject *)Py_TYPE(exc), exc);
    Py_DECREF(exc);
    throw py::python_error();
}
#endif
