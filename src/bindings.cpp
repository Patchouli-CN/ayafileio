/*
 * bindings.cpp - nanobind module entry point
 */
#include "globals.hpp"
#ifdef _WIN32
#include "iocp.hpp"
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

// nanobind bindings

struct PyAsyncFile {
    FileHandle *fh;
    // 修改构造函数：接受 py::object 或 const char*
    explicit PyAsyncFile(const char* path, const char* mode = "rb")
        : fh(new FileHandle(std::string(path), std::string(mode))) {}

    PyAsyncFile(int fd, const char* mode = "rb", bool owns_fd = false)
        : fh(new FileHandle(fd, std::string(mode), owns_fd)) {}

    ~PyAsyncFile() { delete fh; }

    py::object read(int64_t size = -1) {
        PyObject *r = fh->read(size);
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
    if (options.contains("io_uring_sqpoll")) {
        cfg.io_uring_sqpoll = py::cast<bool>(options["io_uring_sqpoll"]);
    }
    
    ayafileio::config().update(cfg);
    
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
    result["io_uring_queue_depth"] = cfg.io_uring_queue_depth;
    result["io_uring_sqpoll"] = cfg.io_uring_sqpoll;
    return result;
}

static void py_reset_config() {
    ayafileio::config().update(ayafileio::Config::defaults());
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
        printf("Warning: Failed to init IOCP:", e.what());
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
        .def("write", &PyAsyncFile::write)
        .def("seek",  &PyAsyncFile::seek,  py::arg("offset"), py::arg("whence") = 0)
        .def("flush", &PyAsyncFile::flush)
        .def("close", &PyAsyncFile::close)
        .def("tell", &PyAsyncFile::tell)
        .def("truncate", &PyAsyncFile::truncate, py::arg("size"))
        .def("readinto", &PyAsyncFile::readinto, py::arg("buf"))
        .def("fileno", &PyAsyncFile::fileno)
        .def("_close_impl", &PyAsyncFile::close_impl);

    // 向后兼容的句柄池 API
    m.def("set_handle_pool_limits", &set_handle_pool_limits,
        "Set handle pool max_per_key and max_total",
        py::arg("max_per_key"), py::arg("max_total"));

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
