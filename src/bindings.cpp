/*
 * bindings.cpp - nanobind module entry point
 *
 * 本文件**只做绑定**：NB_MODULE + m.def / py::class_。
 * 任何业务逻辑、策略校验、平台 #ifdef 的实现细节都不属于这里——它们各自的
 * 家是：
 *
 *   globals.cpp        py_init_module / py_cleanup / 配置 API（configure 等）
 *   discover.cpp       py_get_backend_info / py_get_capabilities
 *   file_handle.cpp    py_open_async（异步打开编排 + 引用计数契约）
 *   result_batcher.cpp py_debug_batcher_outstanding / py_debug_batcher_state
 *   iocp_context.cpp   py_debug_quarantine_stats（Windows）
 *
 * 这里剩下的 #ifdef 只用于「模块对外 API 在不同平台上有无这个函数」，
 * 不包含任何实现。
 */
#include "globals.hpp"
#include "discover.h"      // py_get_backend_info / py_get_capabilities
#include "file_handle.hpp"
#include "pool.hpp"

using namespace ayafileio;

NB_MODULE(_ayafileio, m) {
    m.doc() = "Cross-platform async file I/O module";

    // 缓存全局字符串、初始化平台设施（实现见 globals.cpp）
    py_init_module();

    // ── AsyncFile 类 ──────────────────────────────────────────────────────
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

    // ── 生命周期 ──────────────────────────────────────────────────────────
    m.def("cleanup", &py_cleanup,
          "Perform native cleanup (safe to call from Python atexit)");

    // ── 异步打开 ──────────────────────────────────────────────────────────
    m.def("open_async", &py_open_async, py::arg("path"), py::arg("mode") = "rb",
          "Open a file asynchronously: Linux uses io_uring when available, "
          "other platforms and fallback use the C++ thread pool, "
          "resolving to a ready-to-use AsyncFile.");

    // ── 诊断 / 回归测试 ───────────────────────────────────────────────────
    m.def("debug_batcher_outstanding", &py_debug_batcher_outstanding,
          "Current loop's in-flight op accounting (test/debug aid)");

#ifdef _WIN32
    m.def("debug_quarantine_stats", &py_debug_quarantine_stats,
          "Quarantine depth / drain scan accounting (test/debug aid)");
#endif

    m.def("debug_batcher_state", &py_debug_batcher_state,
          "Effective batching parameters of the current loop (test/debug aid)");

    m.def("debug_native_aopen_count", &native_aopen_submission_count,
          "Number of opens submitted through the native backend (test/debug aid)");

    m.def("drain_buffer_pool", &pool_clear,
          "Drain all cached I/O buffers from the buffer pool");

    // ── 向后兼容的 worker count 入口 ──────────────────────────────────────
#ifdef _WIN32
    m.def("set_iocp_worker_count", &py_set_iocp_worker_count,
          "set iocp worker count");
#endif
    m.def("set_worker_count", &py_set_worker_count,
          "Set global IO worker count (cross-platform)", py::arg("count"));

    // ════════════════════════════════════════════════════════════════════════
    // 统一配置 API (推荐使用)
    // ════════════════════════════════════════════════════════════════════════

    m.def("configure", &py_configure,
          R"doc(Configure ayafileio with a dictionary of options.

Options:
    io_worker_count (int): IO worker threads, 0=auto (default 0, max 128)
    buffer_pool_max (int): Max cached buffers (default 512)
    buffer_size (int): Buffer size in bytes (default 65536)
    close_timeout_ms (int): Close timeout in ms (default 4000)
    io_uring_queue_depth (int): io_uring queue depth (Linux, default 256)
    io_uring_sqpoll (bool): Enable SQPOLL mode (Linux, default False)
    enable_debug_log (bool): Enable debug logging (default False)

Deprecated:
    handle_pool_max_per_key / handle_pool_max_total: accepted but ignored
    (the handle pool was removed; IOCP-associated handles cannot be reused)

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
        completion_model, batch_harvest
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
