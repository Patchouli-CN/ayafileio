#pragma once
#include "globals.hpp"
#include "io_backend.hpp"
#include <string>
#include <cstdint>

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// §6  FileHandle
// ════════════════════════════════════════════════════════════════════════════

class FileHandle {
public:
    FileHandle(const std::string& path, const std::string& mode);
    // open_async 专用（Windows）：loop/create_future 在事件循环线程预取注入；
    // POSIX 后端惰性绑定事件循环（首次 I/O 时），忽略这两个参数
    FileHandle(const std::string& path, const std::string& mode,
               PyObject* loop, PyObject* create_future);
    FileHandle(int fd, const std::string& mode, bool owns_fd = false);  // ← 新增
    ~FileHandle();

    PyObject* read(int64_t size = -1) { return m_backend->read(size); }
    PyObject* read_at(int64_t offset, int64_t size) { return m_backend->read_at(offset, size); }
    PyObject* write(Py_buffer* view, int64_t position = -1) { return m_backend->write(view, position); }
    PyObject* seek(int64_t offset, int whence = 0) { return m_backend->seek(offset, whence); }
    PyObject* flush() { return m_backend->flush(); }
    PyObject* close() { return m_backend->close(); }
    PyObject* tell() { return m_backend->tell(); }
    PyObject* truncate(int64_t size) { return m_backend->truncate(size); }
    PyObject* readinto(PyObject* buf) { return m_backend->readinto(buf); }
    int fileno() const { return m_backend->fileno(); }
    void      close_impl() { m_backend->close_impl(); }

    void complete_ok(IORequest* req, DWORD bytes) { m_backend->complete_ok(req, bytes); }
    void complete_error(IORequest* req, DWORD err) { m_backend->complete_error(req, err); }

private:
    IOBackendBase* m_backend = nullptr;
};

// Python 层 AsyncFile 的轻量包装。放在 FileHandle 旁，避免具体后端
// 依赖 bindings.cpp 中的绑定定义。
struct PyAsyncFile {
    FileHandle *fh;

    explicit PyAsyncFile(const char* path, const char* mode = "rb")
        : fh(new FileHandle(std::string(path), std::string(mode))) {}

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

// 平台分发的原生异步打开接口；不支持的平台返回 false 并回退线程池。
bool try_native_aopen(PyObject* loop, PyObject* future,
                      ResultBatcher* batcher,
                      const char* path, const char* mode);
uint64_t native_aopen_submission_count();

} // namespace ayafileio
