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
#include <linux/time.h>  // 或者手动定义
using DWORD = uint32_t;
using HANDLE = int;
static constexpr HANDLE INVALID_HANDLE_VALUE = (HANDLE)-1;
static inline void CloseHandle(HANDLE) { (void)0; }
#endif
#include <atomic>

namespace py = nanobind;

// ════════════════════════════════════════════════════════════════════════════
// §1  Cached CPython globals
// ════════════════════════════════════════════════════════════════════════════

extern PyObject *g_OSError;
extern PyObject *g_FileNotFoundError;
extern PyObject *g_PermissionError;
extern PyObject *g_ValueError;
extern PyObject *g_KeyboardInterrupt;
extern PyObject *g_FileExistsError;
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
#include <cstring>

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
[[noreturn]] inline void throw_os_error(int err, const char *msg, const char *filename = nullptr) {
    PyObject *cls = map_posix_error(err);
    
    char error_msg[1024];
    const char *final_msg = msg;
    
    if (filename) {
        snprintf(error_msg, sizeof(error_msg), 
                 "[Errno %d] %s: '%s'", err, msg, filename);
        final_msg = error_msg;
    } else {
        // 尝试获取系统错误描述
        const char *sys_msg = strerror(err);
        if (sys_msg) {
            snprintf(error_msg, sizeof(error_msg), 
                     "[Errno %d] %s: %s", err, msg, sys_msg);
        } else {
            snprintf(error_msg, sizeof(error_msg), 
                     "[Errno %d] %s", err, msg);
        }
        final_msg = error_msg;
    }
    
    PyObject *exc = PyObject_CallFunction(cls, "is", err, final_msg);
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
    
    char error_msg[1024];
    const char *final_msg = msg;
    
    if (filename) {
        snprintf(error_msg, sizeof(error_msg), 
                 "[Errno %d] %s: '%s'", err, msg, filename);
        final_msg = error_msg;
    } else {
        const char *sys_msg = strerror(err);
        if (sys_msg) {
            snprintf(error_msg, sizeof(error_msg), 
                     "[Errno %d] %s: %s", err, msg, sys_msg);
        } else {
            snprintf(error_msg, sizeof(error_msg), 
                     "[Errno %d] %s", err, msg);
        }
        final_msg = error_msg;
    }
    
    PyObject *exc = PyObject_CallFunction(cls, "is", err, final_msg);
    PyErr_SetObject(cls, exc);
    Py_DECREF(exc);
}

inline void set_os_error(const char *msg, const char *filename = nullptr) {
    set_os_error(errno, msg, filename);
}

#endif