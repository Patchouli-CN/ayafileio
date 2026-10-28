#include "py_async_file.hpp"

#include "py_buffer.hpp"
#include "py_error.hpp"

#include <nanobind/stl/pair.h>

namespace ayafileio::python {
namespace {

std::unique_ptr<core::FileHandle> open_handle(const char* path, const char* mode) {
    try {
        GilRelease release;
        return std::make_unique<core::FileHandle>(path, mode);
    } catch (...) {
        raise_current_exception();
    }
}

std::unique_ptr<core::FileHandle> wrap_handle(int fd, const char* mode, bool owns_fd) {
    try {
        GilRelease release;
        return std::make_unique<core::FileHandle>(fd, mode, owns_fd);
    } catch (...) {
        raise_current_exception();
    }
}

} // namespace

PyAsyncFile::PyAsyncFile(const char* path, const char* mode)
    : handle_(open_handle(path, mode)) {}

PyAsyncFile::PyAsyncFile(int fd, const char* mode, bool owns_fd)
    : handle_(wrap_handle(fd, mode, owns_fd)) {}

PyAsyncFile::~PyAsyncFile() {
    GilRelease release;
    handle_->close_impl();
}

std::shared_ptr<LoopContext> PyAsyncFile::loop() {
    if (!loop_) loop_ = LoopContext::current();
    return loop_;
}

std::shared_ptr<PyOperation> PyAsyncFile::operation(
    core::ReqType type,
    PyOperation::OpenFactory factory) {
    return std::make_shared<PyOperation>(loop(), type, std::move(factory));
}

nb::object PyAsyncFile::read(int64_t size) {
    return invoke(core::ReqType::Read, [&](core::OperationPtr op) {
        handle_->read(size, std::move(op));
    });
}

nb::object PyAsyncFile::read_at(int64_t offset, int64_t size) {
    return invoke(core::ReqType::Read, [&](core::OperationPtr op) {
        handle_->read_at(offset, size, std::move(op));
    });
}

nb::object PyAsyncFile::read_many(nb::iterable spans) {
    std::vector<std::pair<int64_t, int64_t>> requests;
    for (auto span : spans) {
        requests.push_back(nb::cast<std::pair<int64_t, int64_t>>(span));
    }

    auto state = std::make_shared<PyBatchState>(loop(), requests.size());
    std::size_t index = 0;
    try {
        for (const auto& [offset, size] : requests) {
            auto operation = std::make_shared<PyBatchOperation>(state, index);
            handle_->read_at(offset, size, std::move(operation));
            ++index;
        }
    } catch (...) {
        PyObject* exception = fetch_current_exc();
        state->fail_from(index, exception);
        Py_DECREF(exception);
    }
    return state->future_object();
}

nb::object PyAsyncFile::write(nb::object data) {
    auto buffer = make_py_buffer(data.ptr(), PyBUF_SIMPLE);
    return invoke(core::ReqType::Write, [&](core::OperationPtr op) {
        handle_->write(std::move(buffer), -1, std::move(op));
    });
}

nb::object PyAsyncFile::write_at(int64_t offset, nb::object data) {
    if (offset < 0) throw nb::value_error("offset must be non-negative");
    auto buffer = make_py_buffer(data.ptr(), PyBUF_SIMPLE);
    if (offset > INT64_MAX - static_cast<int64_t>(buffer.size())) {
        PyErr_SetString(PyExc_OverflowError, "offset + buffer size exceeds INT64_MAX");
        throw nb::python_error();
    }
    return invoke(core::ReqType::Write, [&](core::OperationPtr op) {
        handle_->write(std::move(buffer), offset, std::move(op));
    });
}

nb::object PyAsyncFile::seek(int64_t offset, int whence) {
    return invoke(core::ReqType::Other, [&](core::OperationPtr op) {
        handle_->seek(offset, whence, std::move(op));
    });
}

nb::object PyAsyncFile::flush() {
    return invoke(core::ReqType::Other, [&](core::OperationPtr op) {
        handle_->flush(std::move(op));
    });
}

nb::object PyAsyncFile::close() {
    if (!loop_) {
        PyObject* current = PyObject_CallNoArgs(g_get_running_loop);
        if (!current) {
            PyErr_Clear();
            {
                GilRelease release;
                handle_->close_impl();
            }
            Py_INCREF(Py_None);
            return nb::steal<nb::object>(nb::handle(Py_None));
        }
        Py_DECREF(current);
    }
    auto op = operation(core::ReqType::Other);
    try {
        GilRelease release;
        handle_->close(op);
    } catch (...) {
        raise_current_exception();
    }
    return op->future_object();
}

nb::object PyAsyncFile::tell() {
    return invoke(core::ReqType::Other, [&](core::OperationPtr op) {
        handle_->tell(std::move(op));
    });
}

nb::object PyAsyncFile::truncate(int64_t size) {
    return invoke(core::ReqType::Other, [&](core::OperationPtr op) {
        handle_->truncate(size, std::move(op));
    });
}

nb::object PyAsyncFile::readinto(nb::object buffer) {
    auto view = make_py_buffer(buffer.ptr(), PyBUF_WRITABLE);
    return invoke(core::ReqType::Read, [&](core::OperationPtr op) {
        handle_->readinto(std::move(view), std::move(op));
    });
}

} // namespace ayafileio::python
