#pragma once

#include "../../core/io_backend.hpp"

#include <cstdint>
#include <string>
#include <windows.h>

namespace ayafileio::backend::windows {

class WindowsIOBackend final : public core::IOBackendBase {
public:
    WindowsIOBackend(const std::string& path, const std::string& mode);
    WindowsIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~WindowsIOBackend() override;

    void read(int64_t size, core::OperationPtr operation) override;
    void read_at(int64_t offset, int64_t size, core::OperationPtr operation) override;
    void write(core::Buffer view, int64_t position, core::OperationPtr operation) override;
    void seek(int64_t offset, int whence, core::OperationPtr operation) override;
    void flush(core::OperationPtr operation) override;
    void close(core::OperationPtr operation) override;
    void tell(core::OperationPtr operation) override;
    void truncate(int64_t size, core::OperationPtr operation) override;
    void readinto(core::Buffer buffer, core::OperationPtr operation) override;
    int fileno() const override;
    void close_impl() override;

private:
    void init_from_path(const std::string& path, const std::string& mode);

    uint64_t session_id_{0};
    mutable int cached_fileno_{-1};
    mutable HANDLE cached_dup_handle_{INVALID_HANDLE_VALUE};
};

} // namespace ayafileio::backend::windows
