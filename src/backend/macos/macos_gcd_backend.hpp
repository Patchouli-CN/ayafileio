#pragma once
#ifdef __APPLE__

#include "../../core/file_state.hpp"
#include "../../core/io_backend.hpp"

#include <atomic>
#include <dispatch/dispatch.h>
#include <memory>
#include <optional>
#include <string>

namespace ayafileio::backend::macos {

class MacOSGCDBackend final : public core::IOBackendBase {
public:
    MacOSGCDBackend(const std::string& path, const std::string& mode);
    MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~MacOSGCDBackend() override;

    void read(int64_t size, core::OperationPtr operation) override;
    void read_at(int64_t offset, int64_t size, core::OperationPtr operation) override;
    void write(core::Buffer view, int64_t position, core::OperationPtr operation) override;
    void seek(int64_t offset, int whence, core::OperationPtr operation) override;
    void flush(core::OperationPtr operation) override;
    void close(core::OperationPtr operation) override;
    void tell(core::OperationPtr operation) override;
    void truncate(int64_t size, core::OperationPtr operation) override;
    void readinto(core::Buffer buffer, core::OperationPtr operation) override;
    int fileno() const override { return fd_; }
    void close_impl() override;

private:
    struct GcdFdCleanup {
        int fd{-1};
        dispatch_semaphore_t done{nullptr};
        ~GcdFdCleanup() { if (done) dispatch_release(done); }
    };

    static constexpr std::size_t large_io_threshold = 256 * 1024;
    static constexpr std::size_t parallel_read_threshold = 1024 * 1024;
    static constexpr std::size_t parallel_read_chunk = 512 * 1024;

    void init_channel();
    void ensure_pool();
    std::optional<uint64_t> refresh_size();
    void refresh_after_short_read();
    bool closed() const noexcept;
    void complete_closed(const core::OperationPtr& operation, core::ReqType type);
    void submit_read(core::IORequest* request, uint64_t offset, std::size_t size);
    void submit_read_fast(core::IORequest* request, uint64_t offset, std::size_t size);
    void submit_read_parallel(core::IORequest* request, uint64_t offset, std::size_t size);
    void submit_write_fast(core::IORequest* request, uint64_t offset, std::size_t size);
    bool try_inline_read(core::IORequest* request, uint64_t offset, std::size_t size);

    dispatch_io_t channel_{nullptr};
    dispatch_queue_t queue_{nullptr};
    int fd_{-1};
    std::shared_ptr<GcdFdCleanup> fd_cleanup_;
    std::atomic<bool> running_{false};
    core::FileState state_;
    const void* map_base_{nullptr};
    std::size_t map_size_{0};
};

} // namespace ayafileio::backend::macos

#endif
