/*
 * bindings.cpp - pybind11 module entry point
 */
#include "globals.hpp"
#ifdef _WIN32
#include "iocp.hpp"
#endif
#include "file_handle.hpp"
#include "handle_pool.hpp"

// pybind11 bindings

struct PyAsyncFile {
    FileHandle *fh;
    explicit PyAsyncFile(const std::string &path, const std::string &mode)
        : fh(new FileHandle(path, mode)) {}
    ~PyAsyncFile() { delete fh; }

    py::object read(int64_t size = -1) {
        PyObject *r = fh->read(size);
        if (!r) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(r);
    }
    py::object write(py::object data) {
        Py_buffer view{};
        if (PyObject_GetBuffer(data.ptr(), &view, PyBUF_SIMPLE) < 0)
            throw py::error_already_set();
        PyObject *r = fh->write(&view);
        PyBuffer_Release(&view);
        if (!r) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(r);
    }
    py::object seek(int64_t offset, int whence = 0) {
        PyObject *r = fh->seek(offset, whence);
        if (!r) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(r);
    }
    py::object flush() {
        PyObject *r = fh->flush();
        if (!r) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(r);
    }
    py::object close() {
        PyObject *r = fh->close();
        if (!r) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(r);
    }
    void close_impl() { fh->close_impl(); }
};

PYBIND11_MODULE(_ayafileio, m) {
    m.doc() = "Cross-platform async file I/O module";

    cache_globals();
#ifdef _WIN32
    init_iocp();
#endif

    // 清理由 Python 层负责注册；在 C++ 层暴露一个可调用的 cleanup()
    m.def("cleanup", []() {
        // 总是先尝试 drain handle pool（跨平台）
        handle_pool_drain();
#ifdef _WIN32
        // Windows 特有：关闭所有打开文件并停止 IOCP
        close_all_files();
        shutdown_iocp();
#endif
    }, "Perform native cleanup (safe to call from Python atexit)");

    py::class_<PyAsyncFile>(m, "AsyncFile")
        .def(py::init<const std::string &, const std::string &>(),
             py::arg("path"), py::arg("mode") = "rb")
        .def("read",  &PyAsyncFile::read,  py::arg("size") = -1)
        .def("write", &PyAsyncFile::write)
        .def("seek",  &PyAsyncFile::seek,  py::arg("offset"), py::arg("whence") = 0)
        .def("flush", &PyAsyncFile::flush)
        .def("close", &PyAsyncFile::close)
        .def("close_impl", &PyAsyncFile::close_impl);

    m.def("set_handle_pool_limits", &set_handle_pool_limits,
        "Set handle pool max_per_key and max_total",
        py::arg("max_per_key"), py::arg("max_total"));

    m.def("get_handle_pool_limits", &get_handle_pool_limits,
        "Get current handle pool limits as (max_per_key, max_total)");

#ifdef _WIN32
    m.def("set_iocp_worker_count",&set_iocp_worker_count,
        "set iocp worker count");
#endif
}

