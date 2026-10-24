#pragma once
#include <Python.h>
#include <nanobind/nanobind.h>

#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
using DWORD = uint32_t;
using HANDLE = int;
static constexpr HANDLE INVALID_HANDLE_VALUE = (HANDLE)-1;
static inline void CloseHandle(HANDLE) { (void)0; }
#endif
#include <atomic>

namespace py = nanobind;

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// §1  Cached CPython globals
// ════════════════════════════════════════════════════════════════════════════

extern PyObject *g_OSError;
extern PyObject *g_FileNotFoundError;
extern PyObject *g_PermissionError;
extern PyObject *g_ValueError;
extern PyObject *g_KeyboardInterrupt;
extern PyObject *g_FileExistsError;
extern PyObject *g_InvalidStateError;
extern PyObject *g_get_running_loop;

extern PyObject *g_str_set_result;
extern PyObject *g_str_set_exception;
extern PyObject *g_str_create_future;
extern PyObject *g_str_call_soon_ts;

// 全局可配置的 IO worker count（跨平台）。0 = 自动
extern std::atomic<unsigned> g_worker_count;

// 设置全局 worker count（暴露给绑定）
void set_worker_count(unsigned count);

void cache_globals();

// ════════════════════════════════════════════════════════════════════════════
// 事件循环弱引用
//
// 凡是「按 loop 缓存」的东西（ResultBatcher、UringInstance）都不能持 loop 的
// 强引用：那会把任何跑过一次 I/O 的 loop 钉到进程退出——`asyncio.run()` 包在
// 每个请求里的代码等于每请求漏一个 loop 对象。改成弱引用后，需要 loop 时用
// 这个函数现场取回。
//
// 返回 referent 的**新**引用；referent 已死或出错返回 nullptr（错误已清）。
// 3.13+ 用 PyWeakref_GetRef（PyWeakref_GetObject 自 3.13 起废弃，3.15 前
// 仍可用但会告警），旧版本走 GetObject + 手工 INCREF。
// ════════════════════════════════════════════════════════════════════════════
inline PyObject *weakref_target(PyObject *ref) {
    if (!ref) return nullptr;
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

// ════════════════════════════════════════════════════════════════════════════
// GilRelease — RAII：持 GIL 的线程上临时释放 GIL（包裹阻塞型系统调用）
//
// 用于构造函数内的 open/CreateFile/mmap 等：open_async 的工作线程并行
// 打开文件时，系统调用期间不持 GIL，彼此真正并行；构造中的 Python 对象
// 操作（错误路径、session 登记）仍在 GIL 保护下进行。同步 open 路径同样
// 受益：open 系统调用不再冻结整个解释器。
// ════════════════════════════════════════════════════════════════════════════
struct GilRelease {
    PyThreadState* state;
    GilRelease() : state(PyEval_SaveThread()) {}
    ~GilRelease() { PyEval_RestoreThread(state); }
    GilRelease(const GilRelease&) = delete;
    GilRelease& operator=(const GilRelease&) = delete;
};

// ════════════════════════════════════════════════════════════════════════════
// Windows 错误处理
// ════════════════════════════════════════════════════════════════════════════
#ifdef _WIN32
static inline PyObject *map_win_error(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:    return g_FileNotFoundError;
        case ERROR_FILE_EXISTS:       return g_FileExistsError;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_SHARING_VIOLATION: return g_PermissionError;
        default:                      return g_OSError;
    }
}

[[noreturn]] void win_throw_os_error(DWORD err, const char *msg,
                                  const char *filename = nullptr);

// ════════════════════════════════════════════════════════════════════════════
// POSIX (Linux/macOS) 错误处理
// ════════════════════════════════════════════════════════════════════════════
#else
#include <cerrno>

// POSIX 错误码到 Python 异常类的映射
static inline PyObject *map_posix_error(int err) {
    switch (err) {
        case ENOENT:
        case ENOTDIR:           return g_FileNotFoundError;
        case EEXIST:            return g_FileExistsError;
        case EACCES:
        case EPERM:
        case EROFS:             return g_PermissionError;
        case EINVAL:            return g_ValueError;
        default:                return g_OSError;
    }
}

// 设置 Python 错误并抛出异常
// Python 异常类会自动添加 [Errno %d] 前缀，因此 msg 中不要包含 [Errno %d]
// 匹配内置 open() 风格: FileNotFoundError: [Errno 2] No such file or directory: 'path'
[[noreturn]] inline void throw_os_error(int err, const char *msg, const char *filename = nullptr) {
    PyObject *cls = map_posix_error(err);
    PyObject *exc;
    if (filename) {
        exc = PyObject_CallFunction(cls, "iss", err, msg, filename);
    } else {
        exc = PyObject_CallFunction(cls, "is", err, msg);
    }
    PyErr_SetObject(cls, exc);
    Py_DECREF(exc);
    throw py::python_error();
}

// 便捷函数：使用当前 errno
[[noreturn]] inline void throw_os_error(const char *msg, const char *filename = nullptr) {
    throw_os_error(errno, msg, filename);
}

// 设置 Python 错误但不抛出（用于返回 future 的情况）
inline void set_os_error(int err, const char *msg, const char *filename = nullptr) {
    PyObject *cls = map_posix_error(err);
    PyObject *exc;
    if (filename) {
        exc = PyObject_CallFunction(cls, "iss", err, msg, filename);
    } else {
        exc = PyObject_CallFunction(cls, "is", err, msg);
    }
    PyErr_SetObject(cls, exc);
    Py_DECREF(exc);
}

inline void set_os_error(const char *msg, const char *filename = nullptr) {
    set_os_error(errno, msg, filename);
}

#endif

// 取当前 Python 错误的异常实例（owned 引用），供 future.set_exception
// 使用。调用前错误必须已设置（python_error.restore() / PyErr_SetString）。
inline PyObject* fetch_current_exc() {
    PyObject *t = nullptr, *v = nullptr, *tb = nullptr;
    PyErr_Fetch(&t, &v, &tb);
    PyErr_NormalizeException(&t, &v, &tb);
    if (!v && t) v = PyObject_CallNoArgs(t);
    Py_XDECREF(t);
    Py_XDECREF(tb);
    if (!v) v = PyObject_CallNoArgs(g_OSError);
    return v;
}

} // namespace ayafileio
