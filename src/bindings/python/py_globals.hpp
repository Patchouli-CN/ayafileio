#pragma once

#include <Python.h>
#include <nanobind/nanobind.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
using DWORD = uint32_t;
#endif

#include <atomic>

namespace nb = nanobind;

namespace ayafileio::python {

extern PyObject* g_OSError;
extern PyObject* g_FileNotFoundError;
extern PyObject* g_PermissionError;
extern PyObject* g_ValueError;
extern PyObject* g_KeyboardInterrupt;
extern PyObject* g_FileExistsError;
extern PyObject* g_InvalidStateError;
extern PyObject* g_UnsupportedOperation;
extern PyObject* g_get_running_loop;
extern PyObject* g_str_set_result;
extern PyObject* g_str_set_exception;
extern PyObject* g_str_create_future;
extern PyObject* g_str_call_soon_ts;
extern std::atomic<unsigned> g_worker_count;

inline PyObject* weakref_target(PyObject* ref) {
    if (!ref) return nullptr;
#if PY_VERSION_HEX >= 0x030D0000
    PyObject* obj = nullptr;
    if (PyWeakref_GetRef(ref, &obj) != 1) {
        PyErr_Clear();
        return nullptr;
    }
    return obj;
#else
    PyObject* obj = PyWeakref_GetObject(ref);
    if (!obj || obj == Py_None) return nullptr;
    Py_INCREF(obj);
    return obj;
#endif
}

struct GilRelease {
    PyThreadState* state;
    GilRelease() : state(PyEval_SaveThread()) {}
    ~GilRelease() { PyEval_RestoreThread(state); }
    GilRelease(const GilRelease&) = delete;
    GilRelease& operator=(const GilRelease&) = delete;
};

#ifdef _WIN32
inline PyObject* map_win_error(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return g_FileNotFoundError;
        case ERROR_FILE_EXISTS: return g_FileExistsError;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_SHARING_VIOLATION: return g_PermissionError;
        default: return g_OSError;
    }
}
#else
inline PyObject* map_posix_error(int err) {
    switch (err) {
        case ENOENT:
        case ENOTDIR: return g_FileNotFoundError;
        case EEXIST: return g_FileExistsError;
        case EACCES:
        case EPERM:
        case EROFS: return g_PermissionError;
        case EINVAL: return g_ValueError;
        default: return g_OSError;
    }
}
#endif

inline PyObject* fetch_current_exc() {
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    if (!value && type) value = PyObject_CallNoArgs(type);
    Py_XDECREF(type);
    Py_XDECREF(traceback);
    if (!value) value = PyObject_CallNoArgs(g_OSError);
    return value;
}

void cache_globals();
void py_init_module();
void py_cleanup();
void set_worker_count(unsigned count);

} // namespace ayafileio::python
