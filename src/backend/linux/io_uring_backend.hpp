#pragma once
#ifdef HAVE_IO_URING

#include "../../core/file_state.hpp"
#include "../../core/io_backend.hpp"
#include "uring_pool.hpp"

#include <atomic>
#include <optional>
#include <span>
#include <string>

namespace ayafileio::backend::linux {

class IOUringBackend final : public core::IOBackendBase {
public:
    IOUringBackend(const std::string& path, const std::string& mode);
    IOUringBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~IOUringBackend() override;

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

    static void reaper_loop_entry(UringInstance* instance);

private:
    void ensure_uring(const core::OperationPtr& operation);
    void submit_io(core::IORequest* request, int op, std::span<const std::byte> data, uint64_t offset);
    void try_harvest_inline();
    std::optional<uint64_t> refresh_size();
    void refresh_after_short_read();
    bool closed() const noexcept;
    void complete_closed(const core::OperationPtr& operation, core::ReqType type);

    int fd_{-1};
    std::atomic<bool> running_{false};
    core::FileState state_;
    std::shared_ptr<UringInstance> uring_;
};

} // namespace ayafileio::backend::linux

#endif
