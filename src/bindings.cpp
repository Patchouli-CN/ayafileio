/*
 * bindings.cpp - nanobind module entry point
 */
#include "globals.hpp"
#include "result_batcher.hpp"   // refresh_batchers_config() / debug_batcher_state()
#ifdef _WIN32
#include "iocp.hpp"
#include "iocp_context.hpp"
#endif
#ifdef HAVE_IO_URING
#include <liburing.h>
#include "uring_pool.hpp"
#endif
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include "file_handle.hpp"
#include "handle_pool.hpp"
#include "config.hpp"
#include "pool.hpp"
#include "discover.h"
#include "global_thread_pool.hpp"
#include <nanobind/stl/string.h>
#include <algorithm>

// nanobind bindings — 实现全部位于 namespace ayafileio，此处引入
using namespace ayafileio;

// ════════════════════════════════════════════════════════════════════════════
// 统一配置 API
// ════════════════════════════════════════════════════════════════════════════

static void py_configure(py::dict options) {
    auto cfg = ayafileio::config().get();

    if (options.contains("handle_pool_max_per_key")) {
        cfg.handle_pool_max_per_key = py::cast<size_t>(options["handle_pool_max_per_key"]);
    }
    if (options.contains("handle_pool_max_total")) {
        cfg.handle_pool_max_total = py::cast<size_t>(options["handle_pool_max_total"]);
    }
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

    // 同步 handle pool 原子变量，保证 handle_pool_release 读到最新值
    set_handle_pool_limits(cfg.handle_pool_max_per_key, cfg.handle_pool_max_total);
    // 同步 BufferPool 缓存配置
    BufferPool::instance().refresh_config();
    // 同步已存在 batcher 的批处理配置：batcher 创建时缓存了
    // iocp_batch_size / adaptive_batch / adaptive_target_latency_us，少了这一步，
    // 进程跑过第一个异步操作之后 configure() 对这几个键就是静默无效的。
    refresh_batchers_config();
    // 同步旧的全局变量（向后兼容）
    g_worker_count.store(cfg.io_worker_count);
}

static py::dict py_get_config() {
    auto cfg = ayafileio::config().get();
    py::dict result;
    result["handle_pool_max_per_key"] = cfg.handle_pool_max_per_key;
    result["handle_pool_max_total"] = cfg.handle_pool_max_total;
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

static void py_reset_config() {
    ayafileio::config().update(ayafileio::Config::defaults());

    auto defaults = ayafileio::Config::defaults();
    set_handle_pool_limits(defaults.handle_pool_max_per_key, defaults.handle_pool_max_total);
    BufferPool::instance().refresh_config();
    // 与 py_configure 对称：重置也必须把已存在的 batcher 拉回默认值，
    // 否则 reset_config() 之后它们还停在上一次 configure() 的参数上。
    refresh_batchers_config();
    g_worker_count.store(0);
}

// ════════════════════════════════════════════════════════════════════════════
// 后端信息 API
// ════════════════════════════════════════════════════════════════════════════

static py::dict py_get_backend_info() {
    py::dict info;
    info["os_version"] = os_version();

#ifdef _WIN32
    info["platform"] = "windows";
    info["backend"] = "iocp";
    info["is_truly_async"] = true;
    info["description"] = "I/O Completion Ports - native async I/O";

#elif defined(HAVE_IO_URING)
    info["platform"] = "linux";
    if (io_uring_available()) {
        info["backend"] = "io_uring";
        info["is_truly_async"] = true;
        info["description"] = "io_uring - native async I/O (Linux 5.1+)";
    } else {
        info["backend"] = "thread_pool";
        info["is_truly_async"] = false;
        info["description"] = "Thread pool - fallback mode (io_uring not available)";
    }

#elif defined(__APPLE__)
    info["platform"] = "macos";

    if (gcd_dispatch_io_available()) {
        info["backend"] = "dispatch_io";
        info["is_truly_async"] = true;
        info["description"] = "Dispatch I/O (GCD) - native async I/O";
    } else {
        info["backend"] = "thread_pool";
        info["is_truly_async"] = false;
        info["description"] = "Thread pool - fallback mode (Dispatch I/O not available)";
    }

#else
    info["platform"] = "posix";
    info["backend"] = "thread_pool";
    info["is_truly_async"] = false;
    info["description"] = "Thread pool - fallback mode";
#endif

    return info;
}

// 能力发现：身份信息之外，附加当前后端在此机器上探测到的原生能力明细
// （库级能力矩阵由 Python 层 _capabilities.py 拼装）
static py::dict py_get_capabilities() {
    py::dict caps = py_get_backend_info();
    caps["backend_detail"] = backend_detail();
    return caps;
}

// ════════════════════════════════════════════════════════════════════════════
// 模块定义
// ════════════════════════════════════════════════════════════════════════════

NB_MODULE(_ayafileio, m) {
    m.doc() = "Cross-platform async file I/O module";

    // 首先缓存全局变量
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

    // 清理由 Python 层负责注册；在 C++ 层暴露一个可调用的 cleanup()
    m.def("cleanup", []() {
    // 总是先尝试 drain handle pool（跨平台）
    handle_pool_drain();
#ifdef HAVE_IO_URING
    // Linux 特有：清理所有 io_uring 实例
    uring_cleanup_all();
#endif
    // 关闭全局共享线程池（所有后端的 fallback 都依赖它）
    GlobalThreadPool::instance().shutdown();
    // 清理全局 LoopHandle 缓存
    clear_batchers();
#ifdef _WIN32
    // Windows 特有：关闭所有打开文件并停止 IOCP
    close_all_files();
    shutdown_iocp();
#endif
}, "Perform native cleanup (safe to call from Python atexit)");

    // AsyncFile 类
    py::class_<PyAsyncFile>(m, "AsyncFile")
        .def(py::init<const char*, const char*>(),
             py::arg("path"), py::arg("mode") = "rb")
        .def(py::init<int, const char*, bool>(), 
             py::arg("fd"), py::arg("mode") = "rb", 
             py::arg("owns_fd") = false)
        .def("read",  &PyAsyncFile::read,  py::arg("size") = -1)
        .def("read_at", &PyAsyncFile::read_at,
             py::arg("offset"), py::arg("size") = -1)
        .def("write", &PyAsyncFile::write)
        .def("write_at", &PyAsyncFile::write_at, py::arg("offset"), py::arg("data"))
        .def("seek",  &PyAsyncFile::seek,  py::arg("offset"), py::arg("whence") = 0)
        .def("flush", &PyAsyncFile::flush)
        .def("close", &PyAsyncFile::close)
        .def("tell", &PyAsyncFile::tell)
        .def("truncate", &PyAsyncFile::truncate, py::arg("size"))
        .def("readinto", &PyAsyncFile::readinto, py::arg("buf"))
        .def("fileno", &PyAsyncFile::fileno)
        .def("_close_impl", &PyAsyncFile::close_impl);

    // ── open_async：C++ 线程池异步打开 ────────────────────────────────────
    // OS open 提交到 GlobalThreadPool：工作线程持 GIL 构造 FileHandle，
    // 构造内部的阻塞系统调用经 GilRelease 释放 GIL，多 worker 因此真正
    // 并行打开；结果（包装好的 AsyncFile 实例或异常对象）经 ResultBatcher
    // 投递，与 I/O 完成共用同一条批量唤醒链路。Windows 构造需要的
    // loop/create_future 在本线程（有 running loop）预取注入；POSIX 后端
    // 首次 I/O 时才惰性绑定事件循环，注入参数被忽略。
    m.def("open_async", [](const char* path, const char* mode) {
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) throw py::python_error();
        PyObject* create_future = PyObject_GetAttr(loop, g_str_create_future);
        if (!create_future) { Py_DECREF(loop); throw py::python_error(); }
        PyObject* future = PyObject_CallNoArgs(create_future);
        if (!future) { Py_DECREF(create_future); Py_DECREF(loop); throw py::python_error(); }
        ResultBatcher* batcher = get_or_create_batcher(loop);
        PyObject* set_result = PyObject_GetAttr(future, g_str_set_result);
        if (!set_result) {
            Py_DECREF(future); Py_DECREF(create_future); Py_DECREF(loop);
            throw py::python_error();
        }

        batcher->op_submitted();

        // 平台分发：Linux 优先 io_uring OPENAT；不支持时返回 false。
        // 回退会继续使用同一个提交计数，由线程池完成路径负责配平。
        if (try_native_aopen(loop, future, batcher, path, mode)) {
            Py_DECREF(create_future);
            Py_DECREF(loop);
            Py_DECREF(set_result);
            return py::steal<py::object>(py::handle(future));
        }

        // worker 结尾会 DECREF future——但下面 return 的那份 steal 引用属于
        // Python 调用方，不是 worker 的。原先两者共享 CallNoArgs 的那一份
        // 引用：正常情况下 batcher 里的 set_result 绑定方法临时持有的一份
        // 恰好盖住窗口，但在并发大量失败（如 EMFILE）触发立即 flush 时窗口
        // 会被击中，future 提前释放 → await 方读到已释放对象 → 段错误。
        // 让 worker 持有自己的那一份（macOS CI 实测修复，见回归场景）。
        Py_INCREF(future);

        // GlobalThreadPool 是按需启动的：IOCP/uring/GCD 后端都不会启动
        // 它，只有线程池后端会 ensure_started——open_async 可能是第一个
        // 用户，必须先确保 worker 在跑，否则任务永远躺在队列里
        unsigned workers = ayafileio::config().io_worker_count();
        if (workers == 0) {
            unsigned hc = std::thread::hardware_concurrency();
            if (hc == 0) hc = 1;
            workers = std::max(1u, std::min(hc * 2u, 16u));
        }
        GlobalThreadPool::instance().ensure_started(workers);

        std::string p(path), m(mode);
        GlobalThreadPool::instance().enqueue(
            [batcher, future, set_result, loop, create_future, p, m]() {
                PyGILState_STATE gs = PyGILState_Ensure();

                PyObject* set_fn = nullptr;
                PyObject* val = nullptr;
                try {
                    auto* pf = new PyAsyncFile(p.c_str(), m.c_str(), loop, create_future);
                    py::object obj = py::cast(pf, py::rv_policy::take_ownership);
                    val = obj.ptr();
                    Py_INCREF(val);   // batcher 持一个引用；obj 析构归还另一个
                    set_fn = set_result;
                    Py_INCREF(set_fn);
                } catch (py::python_error &e) {
                    e.restore();
                    val = fetch_current_exc();
                    set_fn = PyObject_GetAttr(future, g_str_set_exception);
                    if (!set_fn) PyErr_Clear();
                } catch (const py::builtin_exception &e) {
                    // nanobind 内建异常（value_error 是返回 builtin_exception
                    // 的工厂函数而非类型，不能直接 catch）。构造路径唯一来源
                    // 是 parse_mode 的非法模式 → ValueError
                    PyErr_SetString(g_ValueError, e.what());
                    val = fetch_current_exc();
                    set_fn = PyObject_GetAttr(future, g_str_set_exception);
                    if (!set_fn) PyErr_Clear();
                } catch (const std::exception &e) {
                    PyErr_SetString(g_OSError, e.what());
                    val = fetch_current_exc();
                    set_fn = PyObject_GetAttr(future, g_str_set_exception);
                    if (!set_fn) PyErr_Clear();
                }

                // 与 complete_ok 相同的批量 flush 策略
                bool last = batcher->op_completed();
                if (set_fn && val) {
                    bool threshold = batcher->push(set_fn, val);
                    if (threshold || last) batcher->flush();
                } else {
                    Py_XDECREF(set_fn);
                    Py_XDECREF(val);
                }

                Py_DECREF(create_future);
                // loop 引用不归还：Windows Session 借用 loop（borrowed
                // ref），这个 +1 是它的续命来源，与同步 open 的 ctor 一致
                Py_DECREF(future);
                Py_DECREF(set_result);
                PyGILState_Release(gs);
            });

        return py::steal<py::object>(py::handle(future));
    }, py::arg("path"), py::arg("mode") = "rb",
    "Open a file asynchronously: Linux uses io_uring when available, "
    "other platforms and fallback use the C++ thread pool, "
    "resolving to a ready-to-use AsyncFile.");

    // 调试/回归测试：当前 loop 的 batcher 在飞 op 记账（正常应为 0）。
    // 同步完成路径若泄漏 op_submitted，这里会看到只增不减的计数
    m.def("debug_batcher_outstanding", []() {
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) throw py::python_error();
#ifdef _WIN32
        ResultBatcher* b = IOCPContext::instance().get_batcher(loop);
#else
        ResultBatcher* b = get_or_create_batcher(loop);
#endif
        Py_DECREF(loop);
        return b ? b->outstanding() : 0L;
    }, "Current loop's in-flight op accounting (test/debug aid)");

    // 调试/回归测试：IOCP quarantine 深度与 drain 扫描量。
    // 持续负载下 live 应稳定在 (吞吐 × QUARANTINE_MS) 量级，
    // scanned 应按 op 数线性增长（摊销 O(1)），而不是随队列深度平方增长。
    // quarantine 本身是 IOCP 专属（io_uring / GCD 后端没有这层延迟释放），
    // 所以只在 Windows 上导出；tests/test_quarantine.py 在其它平台跳过。
#ifdef _WIN32
    m.def("debug_quarantine_stats", []() {
        QuarantineStats s = quarantine_stats();
        py::dict d;
        d["live"]    = s.live;
        d["pushes"]  = s.pushes;
        d["scanned"] = s.scanned;
        d["drains"]  = s.drains;
        d["peak"]    = s.peak;
        return d;
    }, "Quarantine depth / drain scan accounting (test/debug aid)");
#endif

    // 调试/回归测试：当前 loop 的 batcher 实际生效的批处理参数。
    // configure() 热更新的回归测试就是读这里：改完配置后再读，值必须跟着变。
    m.def("debug_batcher_state", []() {
        PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
        if (!loop) throw py::python_error();
#ifdef _WIN32
        ResultBatcher* b = IOCPContext::instance().get_batcher(loop);
#else
        ResultBatcher* b = get_or_create_batcher(loop);
#endif
        Py_DECREF(loop);
        py::dict d;
        if (!b) return d;
        d["outstanding"]       = b->outstanding();
        d["threshold"]         = b->current_threshold();
        d["threshold_max"]     = b->threshold_max();
        d["idle_ms"]           = b->current_idle_ms();
        d["idle_max_ms"]       = b->idle_timeout_max_ms();
        d["adaptive"]          = b->adaptive();
        d["target_latency_us"] = b->target_latency_us();
        d["median_interval_us"] = b->median_interval_us();
        return d;
    }, "Effective batching parameters of the current loop (test/debug aid)");

    m.def("debug_native_aopen_count", &native_aopen_submission_count,
          "Number of opens submitted through the native backend (test/debug aid)");

    // 向后兼容的句柄池 API
    m.def("set_handle_pool_limits", &set_handle_pool_limits,
        "Set handle pool max_per_key and max_total",
        py::arg("max_per_key"), py::arg("max_total"));

    m.def("drain_handle_pool", &handle_pool_drain,
        "Drain all cached handles from the pool (useful between benchmark rounds)");

    m.def("drain_buffer_pool", &pool_clear,
        "Drain all cached I/O buffers from the buffer pool");

    m.def("get_handle_pool_limits", []() {
        auto p = get_handle_pool_limits();
        return py::make_tuple(p.first, p.second);
    }, "Get current handle pool limits as (max_per_key, max_total)");

    // 向后兼容的 worker count API
#ifdef _WIN32
    m.def("set_iocp_worker_count", &set_iocp_worker_count,
        "set iocp worker count");
#endif
    m.def("set_worker_count", &set_worker_count,
        "Set global IO worker count (cross-platform)", py::arg("count"));

    // ════════════════════════════════════════════════════════════════════════
    // 统一配置 API (推荐使用)
    // ════════════════════════════════════════════════════════════════════════
    
    m.def("configure", &py_configure, 
          R"doc(Configure ayafileio with a dictionary of options.

Options:
    handle_pool_max_per_key (int): Max cached handles per file (Windows, default 64)
    handle_pool_max_total (int): Max total cached handles (Windows, default 2048)
    io_worker_count (int): IO worker threads, 0=auto (default 0, max 128)
    buffer_pool_max (int): Max cached buffers (default 512)
    buffer_size (int): Buffer size in bytes (default 65536)
    close_timeout_ms (int): Close timeout in ms (default 4000)
    io_uring_queue_depth (int): io_uring queue depth (Linux, default 256)
    io_uring_sqpoll (bool): Enable SQPOLL mode (Linux, default False)
    enable_debug_log (bool): Enable debug logging (default False)

Example:
    >>> ayafileio.configure({
    ...     "io_worker_count": 8,
    ...     "buffer_size": 131072,
    ...     "close_timeout_ms": 2000,
    ... })
)doc",
          py::arg("options"));

    m.def("get_config", &py_get_config, 
          "Get current configuration as a dictionary");

    m.def("reset_config", &py_reset_config, 
          "Reset configuration to defaults");

    m.def("get_backend_info", &py_get_backend_info,
          R"doc(Get current backend information.

Returns:
    Dictionary with keys:
        - platform: str ("windows", "linux", "macos", "posix")
        - backend: str ("iocp", "io_uring", "dispatch_io", "thread_pool")
        - is_truly_async: bool
        - os_version: str (kernel version, e.g. "10.0.26100" / "Linux 6.5.0-...")
        - description: str

Example:
    >>> info = ayafileio.get_backend_info()
    >>> print(info)
    {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True, ...}
)doc");

    m.def("get_capabilities", &py_get_capabilities,
          R"doc(Get backend identity plus natively probed capability detail.

Everything in get_backend_info(), plus a backend_detail dict whose keys
depend on the active backend:

    iocp (Windows):
        completion_model, batch_harvest, handle_pool
    io_uring (Linux):
        available, completion_model, sqpoll,
        features (ring.features bit matrix, e.g. single_mmap, fast_poll),
        opcodes (per-opcode support via IORING_REGISTER_PROBE; None on
                 kernels < 5.6 that lack the probe command)
    dispatch_io (macOS):
        dispatch_io, completion_model, kernel_managed_workqueue,
        mincore_fastpath
    thread_pool (fallback):
        completion_model (False), note

Capabilities are probed by asking the kernel directly (probe ring /
trial ring creation) rather than parsing kernel version strings, so
backported kernels report what they actually support. Results are
cached process-wide; repeated calls are free.

Example:
    >>> caps = ayafileio.get_capabilities()["backend_detail"]
    >>> caps["sqpoll"]          # Linux io_uring only
    True
)doc");
}
