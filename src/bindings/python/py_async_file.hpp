#pragma once

#include "../../core/file_handle.hpp"
#include "py_error.hpp"
#include "py_future.hpp"

#include <memory>
#include <string>

namespace ayafileio::python {

class PyAsyncFile {
public:
    explicit PyAsyncFile(const char* path, const char* mode = "rb");
    explicit PyAsyncFile(std::unique_ptr<core::FileHandle> handle)
        : handle_(std::move(handle)) {}
    PyAsyncFile(int fd, const char* mode = "rb", bool owns_fd = false);
    ~PyAsyncFile();

    nb::object read(int64_t size = -1);
    nb::object read_at(int64_t offset, int64_t size = -1);
    nb::object write(nb::object data);
    nb::object write_at(int64_t offset, nb::object data);
    nb::object seek(int64_t offset, int whence = 0);
    nb::object flush();
    nb::object close();
    nb::object tell();
    nb::object truncate(int64_t size);
    nb::object readinto(nb::object buffer);

    int fileno() const { return handle_->fileno(); }
    void close_impl() { handle_->close_impl(); }

private:
    std::shared_ptr<LoopContext> loop();
    std::shared_ptr<PyOperation> operation(
        core::ReqType type,
        PyOperation::OpenFactory factory = {});

    template<typename F>
    nb::object invoke(core::ReqType type, F&& call) {
        auto op = operation(type);
        try {
            call(op);
        } catch (...) {
            raise_current_exception();
        }
        return op->future_object();
    }

    std::unique_ptr<core::FileHandle> handle_;
    std::shared_ptr<LoopContext> loop_;
};

} // namespace ayafileio::python
