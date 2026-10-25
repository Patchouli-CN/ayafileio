#pragma once

#include "../../core/file_state.hpp"
#include "../../core/io_backend.hpp"
#include "../../platform/native_file.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace ayafileio::backend::thread {

class ThreadIOBackend final : public core::IOBackendBase {
public:
    ThreadIOBackend(const std::string& path, const std::string& mode);
    ThreadIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~ThreadIOBackend() override;

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
    std::optional<uint64_t> refresh_size();
    void refresh_after_short_read();
    void enqueue_task(std::function<void()> task);
    bool closed() const noexcept;
    void complete_closed(const core::OperationPtr& operation, core::ReqType type);

    platform::file_handle_t fd_{platform::kFileInvalid};
    std::atomic<bool> running_{false};
    core::FileState state_;

#ifdef _WIN32
    mutable std::mutex fileno_mutex_;
    mutable int cached_fileno_{-1};
    mutable HANDLE cached_dup_handle_{INVALID_HANDLE_VALUE};
#endif
};

} // namespace ayafileio::backend::thread
