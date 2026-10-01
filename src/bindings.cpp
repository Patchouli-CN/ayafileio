/*
 * bindings.cpp - nanobind module entry point
 */
#include "globals.hpp"
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
#include "global_thread_pool.hpp"
#include <algorithm>

// nanobind bindings — 实现全部位于 namespace ayafileio，此处引入
using namespace ayafileio;

struct PyAsyncFile {
    FileHandle *fh;
    // 修改构造函数：接受 py::object 或 const char*
    explicit PyAsyncFile(const char* path, const char* mode = "rb")
        : fh(new FileHandle(std::string(path), std::string(mode))) {}

    // open_async 注入路径：loop/create_future 由事件循环线程预取（见
    // FileHandle 同名构造函数）
    PyAsyncFile(const char* path, const char* mode, PyObject* loop, PyObject* create_future)
        : fh(new FileHandle(std::string(path), std::string(mode), loop, create_future)) {}

    PyAsyncFile(int fd, const char* mode = "rb", bool owns_fd = false)
        : fh(new FileHandle(fd, std::string(mode), owns_fd)) {}

    ~PyAsyncFile() { delete fh; }

    py::object read(int64_t size = -1) {
        PyObject *r = fh->read(size);
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object read_at(int64_t offset, int64_t size = -1) {
        PyObject *r = fh->read_at(offset, size);
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object write(py::object data) {
        // 使用 Python C API 获取 buffer
        Py_buffer view;
        if (PyObject_GetBuffer(data.ptr(), &view, PyBUF_SIMPLE) < 0) {
            throw py::python_error();
        }
        
        PyObject *r = fh->write(&view);
        PyBuffer_Release(&view);
        
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object write_at(int64_t offset, py::object data) {
        if (offset < 0) throw py::value_error("offset must be non-negative");
        Py_buffer view;
        if (PyObject_GetBuffer(data.ptr(), &view, PyBUF_SIMPLE) < 0)
            throw py::python_error();
        if (offset > INT64_MAX - view.len) {
            PyBuffer_Release(&view);
            PyErr_SetString(PyExc_OverflowError, "offset + buffer size exceeds INT64_MAX");
            throw py::python_error();
        }
        PyObject *r;
        try {
            r = fh->write(&view, offset);
        } catch (...) {
            PyBuffer_Release(&view);
            throw;
        }
        PyBuffer_Release(&view);
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object seek(int64_t offset, int whence = 0) {
        PyObject *r = fh->seek(offset, whence);
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object flush() {
        PyObject *r = fh->flush();
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }
    py::object close() {
        PyObject *r = fh->close();
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }

    py::object tell() {
        PyObject *r = fh->tell();
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }

    py::object truncate(int64_t size) {
        PyObject* r = fh->truncate(size);
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }

    py::object readinto(py::object buf) {
        PyObject* r = fh->readinto(buf.ptr());
        if (!r) throw py::python_error();
        return py::steal<py::object>(py::handle(r));
    }

    void close_impl() { fh->close_impl(); }
    int fileno() { return fh->fileno(); }
};

// 取当前 Python 错误的异常实例（owned 引用），供 future.set_exception
// 使用。调用前错误必须已设置（python_error.restore() / PyErr_SetString）。
static PyObject* fetch_current_exc() {
    PyObject *t = nullptr, *v = nullptr, *tb = nullptr;
    PyErr_Fetch(&t, &v, &tb);
    PyErr_NormalizeException(&t, &v, &tb);
    if (!v && t) v = PyObject_CallNoArgs(t);
    Py_XDECREF(t);
    Py_XDECREF(tb);
    if (!v) v = PyObject_CallNoArgs(g_OSError);  // 兜底（实例化也失败的极端情况）
    return v;
}

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
        cfg.io_uring_sqpoll = py::cast<bool>(options["io_uring_sqpoll"]);
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
    g_worker_count.store(0);
}

// ════════════════════════════════════════════════════════════════════════════
// 后端信息 API
// ════════════════════════════════════════════════════════════════════════════

static py::dict py_get_backend_info() {
    py::dict info;
    
#ifdef _WIN32
    info["platform"] = "windows";
    info["backend"] = "iocp";
    info["is_truly_async"] = true;
    info["description"] = "I/O Completion Ports - native async I/O";
    
#elif defined(HAVE_IO_URING)
    // 运行时检测 io_uring 是否真的可用
    static bool io_uring_available = []() {
        struct io_uring ring;
        int ret = io_uring_queue_init(8, &ring, 0);
        if (ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    
    info["platform"] = "linux";
    if (io_uring_available) {
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
    
    // 运行时检测 Dispatch I/O 是否真的可用
    static bool gcd_available = []() {
        // 尝试创建测试用的 dispatch queue
        dispatch_queue_t test_queue = dispatch_queue_create(
            "com.ayafileio.test", 
            DISPATCH_QUEUE_SERIAL
        );
        if (!test_queue) {
            return false;
        }
        
        // 创建临时文件测试 Dispatch I/O
        char tmp_path[] = "/tmp/ayafileio_test_XXXXXX";
        int fd = mkstemp(tmp_path);
        if (fd == -1) {
            dispatch_release(test_queue);
            return false;
        }
        
        // 尝试创建 dispatch I/O channel
        dispatch_io_t test_channel = dispatch_io_create(
            DISPATCH_IO_RANDOM,
            fd,
            test_queue,
            ^(int error) {
                // cleanup handler - 文件描述符会在这里被关闭
            }
        );
        
        bool available = (test_channel != nullptr);
        
        if (test_channel) {
            dispatch_io_close(test_channel, DISPATCH_IO_STOP);
            dispatch_release(test_channel);
        } else {
            // 如果 channel 创建失败，手动关闭 fd
            close(fd);
        }
        
        unlink(tmp_path);
        dispatch_release(test_queue);
        
        return available;
    }();
    
    if (gcd_available) {
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
    "Open a file asynchronously: the OS open runs on the C++ thread pool, "
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
        - backend: str ("iocp", "io_uring", "thread_pool")
        - is_truly_async: bool
        - description: str

Example:
    >>> info = ayafileio.get_backend_info()
    >>> print(info)
    {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True, 'description': '...'}
)doc");
}
