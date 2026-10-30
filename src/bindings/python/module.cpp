#include "py_api.hpp"
#include "py_async_file.hpp"
#include "../../core/buffer_pool.hpp"

namespace core = ayafileio::core;
namespace python = ayafileio::python;

NB_MODULE(_ayafileio, module) {
    module.doc() = "Cross-platform async file I/O module";
    python::py_init_module();

    nb::class_<python::PyAsyncFile>(module, "AsyncFile")
        .def(nb::init<const char*, const char*>(), nb::arg("path"), nb::arg("mode") = "rb")
        .def(nb::init<int, const char*, bool>(), nb::arg("fd"), nb::arg("mode") = "rb", nb::arg("owns_fd") = false)
        .def("read", &python::PyAsyncFile::read, nb::arg("size") = -1)
        .def("read_at", &python::PyAsyncFile::read_at, nb::arg("offset"), nb::arg("size") = -1)
        .def("read_many", &python::PyAsyncFile::read_many, nb::arg("spans"))
        .def("write", &python::PyAsyncFile::write)
        .def("write_at", &python::PyAsyncFile::write_at, nb::arg("offset"), nb::arg("data"))
        .def("seek", &python::PyAsyncFile::seek, nb::arg("offset"), nb::arg("whence") = 0)
        .def("flush", &python::PyAsyncFile::flush)
        .def("close", &python::PyAsyncFile::close)
        .def("tell", &python::PyAsyncFile::tell)
        .def("truncate", &python::PyAsyncFile::truncate, nb::arg("size"))
        .def("readinto", &python::PyAsyncFile::readinto, nb::arg("buf"))
        .def("fileno", &python::PyAsyncFile::fileno)
        .def("_close_impl", &python::PyAsyncFile::close_impl);

    module.def("cleanup", &python::py_cleanup);
    module.def("open_async", &python::py_open_async, nb::arg("path"), nb::arg("mode") = "rb");
    module.def("read_whole_async", &python::py_read_whole_async, nb::arg("path"));
    module.def("debug_batcher_outstanding", &python::py_debug_batcher_outstanding);
#ifdef _WIN32
    module.def("debug_quarantine_stats", &python::py_debug_quarantine_stats);
    module.def("set_iocp_worker_count", &python::py_set_iocp_worker_count, nb::arg("count"));
#endif
    module.def("debug_batcher_state", &python::py_debug_batcher_state);
    module.def("debug_pool_workers", &python::py_debug_pool_workers);
    module.def("debug_native_aopen_count", &python::native_aopen_submission_count);
    module.def("drain_buffer_pool", &core::pool_clear);
    module.def("set_worker_count", &python::py_set_worker_count, nb::arg("count"));
    module.def("configure", &python::py_configure, nb::arg("options"));
    module.def("get_config", &python::py_get_config);
    module.def("reset_config", &python::py_reset_config);
    module.def("get_backend_info", &python::py_get_backend_info);
    module.def("get_capabilities", &python::py_get_capabilities);
}
