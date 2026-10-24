#include "globals.hpp"
#include "discover.h"
#include "pool.hpp"
#include "result_batcher.hpp"
#include "global_thread_pool.hpp"
#include <nanobind/stl/string.h>
#ifdef HAVE_IO_URING
#include "uring_pool.hpp"
#endif
#ifdef _WIN32
#include "iocp.hpp"
#endif

namespace ayafileio {

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

// ════════════════════════════════════════════════════════════════════════════
// Python 侧入口：模块启动/关停 + 配置 API（原 bindings.cpp 的实现）
// ════════════════════════════════════════════════════════════════════════════

void py_init_module() {
    try {
        cache_globals();
    } catch (const std::exception& e) {
        printf("Warning: Failed to cache globals: %s\n", e.what());
    }
#ifdef _WIN32
    try {
        init_iocp();
    } catch (const std::exception& e) {
        printf("Warning: Failed to init IOCP: %s\n", e.what());
    }
#endif
}

void py_cleanup() {
#ifdef HAVE_IO_URING
    // Linux 特有：清理所有 io_uring 实例
    uring_cleanup_all();
#endif
    // 关闭全局共享线程池（所有后端的 fallback 都依赖它）
    GlobalThreadPool::instance().shutdown();
    // 清理全局 batcher 缓存
    clear_batchers();
#ifdef _WIN32
    // Windows 特有：关闭所有打开文件并停止 IOCP
    close_all_files();
    shutdown_iocp();
#endif
}

void py_configure(py::dict options) {
    auto cfg = ayafileio::config().get();

    // 后端选择：只影响之后打开的句柄
    if (options.contains("backend")) {
        std::string name = py::cast<std::string>(options["backend"]);
        BackendKind kind = BackendKind::Auto;
        if (!parse_backend_name(name, kind)) {
            throw py::value_error(
                "backend must be one of: 'auto', 'iocp', 'io_uring', "
                "'dispatch_io', 'thread_pool'");
        }
        // 强制一个本构建/本机器上没有的后端，只会让之后每个 open 都失败；
        // 当场报错比留到运行时更诚实（与 io_uring_sqpoll 的校验同一风格）
        if (kind != BackendKind::Auto && !backend_available(kind)) {
            const std::string msg =
                "backend '" + name + "' is not available on this platform; "
                "call ayafileio.get_backend_info() to see the default backend";
            throw py::value_error(msg.c_str());
        }
        cfg.backend = kind;
    }

    // handle_pool_max_per_key / handle_pool_max_total 已废弃（句柄池移除），
    // 静默忽略以免已发布的用户代码报错
    if (options.contains("io_worker_count")) {
        unsigned val = py::cast<unsigned>(options["io_worker_count"]);
        if (val > 128) throw py::value_error("io_worker_count must be 0-128");
        cfg.io_worker_count = val;
    }
    if (options.contains("buffer_pool_max")) {
        cfg.buffer_pool_max = py::cast<size_t>(options["buffer_pool_max"]);
    }
    if (options.contains("buffer_size")) {
        cfg.buffer_size = py::cast<size_t>(options["buffer_size"]);
    }
    if (options.contains("close_timeout_ms")) {
        cfg.close_timeout_ms = py::cast<unsigned>(options["close_timeout_ms"]);
    }
    if (options.contains("io_uring_queue_depth")) {
        cfg.io_uring_queue_depth = py::cast<unsigned>(options["io_uring_queue_depth"]);
    }
    if (options.contains("iocp_batch_size")) {
        unsigned val = py::cast<unsigned>(options["iocp_batch_size"]);
        if (val < 1 || val > 256) throw py::value_error("iocp_batch_size must be 1-256");
        cfg.iocp_batch_size = val;
    }
    if (options.contains("io_uring_sqpoll")) {
        bool val = py::cast<bool>(options["io_uring_sqpoll"]);
        if (val) {
            // 能力校验：请求内核不支持的能力时显式报错，不静默接受。
            // 关（false）永远允许——跨平台代码可以无条件写 False
            //
            // 先看**目标后端**：backend 一栏在上面已经写进 cfg 了，所以这里不能
            // 用 effective_backend()（它还读着旧配置）。强制 thread_pool 时
            // 再开 SQPOLL 毫无意义——即使本机内核支持，那个后端也不会用到它。
            const BackendKind want =
                cfg.backend == BackendKind::Auto ? effective_backend() : cfg.backend;
            if (want != BackendKind::IoUring) {
                throw py::value_error(
                    "io_uring_sqpoll=True requires the io_uring backend; the effective "
                    "backend is not io_uring, so this option would have no effect. "
                    "Check ayafileio.get_backend_info()['backend'] first.");
            }
#ifdef HAVE_IO_URING
            if (!io_uring_available()) {
                throw py::value_error(
                    "io_uring_sqpoll=True requested, but io_uring is unavailable "
                    "on this system (backend: thread_pool). "
                    "Check ayafileio.get_capabilities()['backend_detail'] first.");
            }
            if (!io_uring_sqpoll_supported()) {
                throw py::value_error(
                    "io_uring_sqpoll=True requested, but this kernel cannot create "
                    "an SQPOLL ring (needs kernel 5.11+ or privileges). "
                    "Check ayafileio.get_capabilities()['backend_detail']['sqpoll'] first.");
            }
#else
            throw py::value_error(
                "io_uring_sqpoll is only meaningful on the Linux io_uring backend");
#endif
        }
        cfg.io_uring_sqpoll = val;
    }
    if (options.contains("adaptive_batch")) {
        cfg.adaptive_batch = py::cast<bool>(options["adaptive_batch"]);
    }
    if (options.contains("adaptive_target_latency_us")) {
        unsigned val = py::cast<unsigned>(options["adaptive_target_latency_us"]);
        if (val < 1 || val > 10000) throw py::value_error("adaptive_target_latency_us must be 1-10000");
        cfg.adaptive_target_latency_us = val;
    }

    ayafileio::config().update(cfg);

    // 同步 BufferPool 缓存配置
    BufferPool::instance().refresh_config();
    // 同步已存在 batcher 的批处理配置：batcher 创建时缓存了
    // iocp_batch_size / adaptive_batch / adaptive_target_latency_us，少了这一步，
    // 进程跑过第一个异步操作之后 configure() 对这几个键就是静默无效的。
    refresh_batchers_config();
    // 同步全局线程池的 worker 数：池一旦启动就只认第一次的规模，不主动同步的话
    // configure({"io_worker_count": N}) 会「配置生效、实际无效」。
    GlobalThreadPool::instance().target_workers(effective_io_worker_count());
    // 同步旧的全局变量（向后兼容）
    g_worker_count.store(cfg.io_worker_count);
}

py::dict py_get_config() {
    auto cfg = ayafileio::config().get();
    py::dict result;
    result["backend"] = backend_name(cfg.backend);
    result["io_worker_count"] = cfg.io_worker_count;
    result["buffer_pool_max"] = cfg.buffer_pool_max;
    result["buffer_size"] = cfg.buffer_size;
    result["close_timeout_ms"] = cfg.close_timeout_ms;
    result["iocp_batch_size"] = cfg.iocp_batch_size;
    result["io_uring_queue_depth"] = cfg.io_uring_queue_depth;
    result["io_uring_sqpoll"] = cfg.io_uring_sqpoll;
    result["adaptive_batch"] = cfg.adaptive_batch;
    result["adaptive_target_latency_us"] = cfg.adaptive_target_latency_us;
    return result;
}

void py_reset_config() {
    ayafileio::config().update(ayafileio::Config::defaults());

    BufferPool::instance().refresh_config();
    // 与 py_configure 对称：重置也必须把已存在的 batcher 拉回默认值，
    // 否则 reset_config() 之后它们还停在上一次 configure() 的参数上。
    refresh_batchers_config();
    GlobalThreadPool::instance().target_workers(effective_io_worker_count());
    g_worker_count.store(0);
}

void py_set_worker_count(int count) {
    set_worker_count(count);
}

#ifdef _WIN32
void py_set_iocp_worker_count(int count) {
    set_iocp_worker_count(count);
}
#endif

} // namespace ayafileio
