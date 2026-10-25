#pragma once

#include "io_backend.hpp"

#include <memory>
#include <string>

namespace ayafileio::core {

class FileHandle {
public:
    FileHandle(const std::string& path, const std::string& mode);
    FileHandle(int fd, const std::string& mode, bool owns_fd = false);
    ~FileHandle() = default;

    void read(int64_t size, OperationPtr operation) {
        backend_->read(size, std::move(operation));
    }
    void read_at(int64_t offset, int64_t size, OperationPtr operation) {
        backend_->read_at(offset, size, std::move(operation));
    }
    void write(Buffer view, int64_t position, OperationPtr operation) {
        backend_->write(std::move(view), position, std::move(operation));
    }
    void seek(int64_t offset, int whence, OperationPtr operation) {
        backend_->seek(offset, whence, std::move(operation));
    }
    void flush(OperationPtr operation) { backend_->flush(std::move(operation)); }
    void close(OperationPtr operation) { backend_->close(std::move(operation)); }
    void tell(OperationPtr operation) { backend_->tell(std::move(operation)); }
    void truncate(int64_t size, OperationPtr operation) {
        backend_->truncate(size, std::move(operation));
    }
    void readinto(Buffer buffer, OperationPtr operation) {
        backend_->readinto(std::move(buffer), std::move(operation));
    }

    int fileno() const { return backend_->fileno(); }
    void close_impl() { backend_->close_impl(); }

private:
    std::unique_ptr<IOBackendBase> backend_;
};

} // namespace ayafileio::core
