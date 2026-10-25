#ifdef HAVE_IO_URING

#include "io_uring_backend.hpp"
#include "native_open.hpp"
#include "../../core/config.hpp"
#include "../../core/file_mode.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ayafileio::backend::linux {
namespace {

core::ModeInfo parse_native_mode(const std::string& mode) {
    try {
        return core::parse_mode(mode);
    } catch (const std::invalid_argument& error) {
        throw core::IoException(core::IoError::value(error.what()));
    }
}

int open_flags(const core::ModeInfo& info) {
    int flags = O_RDONLY;
    if (info.hasW) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (info.hasA) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (info.hasX) flags = O_WRONLY | O_CREAT | O_EXCL;
    if (info.plus) flags = (flags & ~O_ACCMODE) | O_RDWR;
    return flags;
}

} // namespace

IOUringBackend::IOUringBackend(const std::string& path, const std::string& mode)
    : state_(parse_native_mode(mode).appendMode) {
    const auto info = parse_native_mode(mode);
    auto& cfg = core::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();

    fd_ = ::open(path.c_str(), open_flags(info), 0644);
    if (fd_ == -1) {
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(errno), "Failed to open file: " + path));
    }
    running_.store(true, std::memory_order_release);

    struct stat st {};
    if (fstat(fd_, &st) == 0) {
        state_.set_cached_size(static_cast<uint64_t>(st.st_size));
        if (info.appendMode) state_.set_file_pos(static_cast<uint64_t>(st.st_size));
    }
}

IOUringBackend::IOUringBackend(int fd, const std::string& mode, bool owns_fd)
    : state_(parse_native_mode(mode).appendMode) {
    fd_ = fd;
    m_owns_fd = owns_fd;
    auto& cfg = core::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    running_.store(true, std::memory_order_release);

    struct stat st {};
    if (fstat(fd_, &st) == 0) {
        state_.set_cached_size(static_cast<uint64_t>(st.st_size));
        if (state_.append_mode()) state_.set_file_pos(static_cast<uint64_t>(st.st_size));
    }
}

IOUringBackend::~IOUringBackend() {
    close_impl();
}

std::optional<uint64_t> IOUringBackend::refresh_size() {
    struct stat st {};
    if (fstat(fd_, &st) != 0 || st.st_size < 0) return std::nullopt;
    return static_cast<uint64_t>(st.st_size);
}

void IOUringBackend::refresh_after_short_read() {
    if (auto size = refresh_size()) state_.set_cached_size(*size);
}

bool IOUringBackend::closed() const noexcept {
    return !running_.load(std::memory_order_acquire) || fd_ == -1;
}

void IOUringBackend::complete_closed(const core::OperationPtr& operation, core::ReqType type) {
    operation->complete_inline(core::Completion::failure(
        type, core::IoError::value("I/O operation on closed file.")));
}

void IOUringBackend::ensure_uring(const core::OperationPtr& operation) {
    if (uring_) return;
    auto& cfg = core::config();
    uring_ = uring_manager().acquire(
        operation->scheduler_context(),
        &IOUringBackend::reaper_loop_entry,
        cfg.io_uring_queue_depth(),
        cfg.io_uring_flags(),
        cfg.io_uring_sqpoll());
    if (!uring_) throw core::IoException(core::IoError::runtime("Failed to acquire io_uring instance"));
    uring_manager().start_reaper(uring_);
}

void IOUringBackend::reaper_loop_entry(UringInstance* instance) {
    io_uring_cqe* cqe = nullptr;
    while (!instance->reaper_stop.load(std::memory_order_acquire)) {
        const int result = io_uring_wait_cqe(&instance->ring, &cqe);
        if (result == -EEXIST || result == -EINTR) continue;
        if (result < 0) break;

        unsigned head = 0;
        unsigned count = 0;
        bool wakeup = false;
        std::vector<std::pair<core::IORequest*, int>> completed;
        {
            std::lock_guard lock(instance->cq_mtx);
            io_uring_for_each_cqe(&instance->ring, head, cqe) {
                auto* request = static_cast<core::IORequest*>(io_uring_cqe_get_data(cqe));
                if (request) completed.emplace_back(request, cqe->res);
                else wakeup = true;
                ++count;
            }
            if (count > 0) io_uring_cq_advance(&instance->ring, count);
        }
        uring_drain_overflow(instance);

        if (wakeup && !instance->reaper_stop.load(std::memory_order_acquire)) {
            std::lock_guard lock(instance->submit_mtx);
            io_uring_sqe* sqe = io_uring_get_sqe(&instance->ring);
            if (sqe) {
                io_uring_prep_poll_add(sqe, instance->event_fd, POLLIN);
                io_uring_sqe_set_data(sqe, nullptr);
                io_uring_submit(&instance->ring);
            }
        }

        for (auto [request, result] : completed) {
            if (request->type == core::ReqType::Open) {
                complete_native_aopen(request, result, false);
                continue;
            }
            auto* file = static_cast<IOUringBackend*>(request->file);
            if (result >= 0) file->complete_ok(request, static_cast<std::size_t>(result));
            else file->complete_error(request, core::IoError::native(
                static_cast<uint32_t>(-result), "I/O operation failed"));
        }
    }
}

void IOUringBackend::try_harvest_inline() {
    if (!uring_) return;
    std::unique_lock lock(uring_->cq_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return;

    std::pair<core::IORequest*, int> completed[32];
    std::size_t count = 0;
    unsigned head = 0;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&uring_->ring, head, cqe) {
        auto* request = static_cast<core::IORequest*>(io_uring_cqe_get_data(cqe));
        if (!request || count >= 32) break;
        completed[count++] = {request, cqe->res};
    }
    if (count == 0) return;
    io_uring_cq_advance(&uring_->ring, static_cast<unsigned>(count));
    lock.unlock();

    for (std::size_t index = 0; index < count; ++index) {
        auto* request = completed[index].first;
        const auto result = completed[index].second;
        if (request->type == core::ReqType::Open) {
            complete_native_aopen(request, result, true);
        } else {
            static_cast<IOUringBackend*>(request->file)->complete_inline(request, result);
        }
    }
}

void IOUringBackend::submit_io(
    core::IORequest* request,
    int op,
    std::span<const std::byte> data,
    uint64_t offset) {
    if (!uring_ || (op != IORING_OP_READ && op != IORING_OP_WRITE && op != IORING_OP_FSYNC)) {
        complete_error(request, core::IoError::native(EINVAL, "Invalid io_uring operation"));
        return;
    }
    uring_submit_or_queue(uring_.get(), PendingSubmit{
        request, op, fd_, data.data(), static_cast<unsigned>(data.size()), offset});
    try_harvest_inline();
}

void IOUringBackend::read(int64_t size, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    ensure_uring(operation);
    const auto plan = state_.plan_read(size, [&] { return refresh_size(); });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    auto* request = make_req_read_bytes(plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(request, IORING_OP_READ,
              std::as_bytes(std::span{request->buf(), plan.size}), plan.offset);
}

void IOUringBackend::read_at(int64_t offset, int64_t size, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    if (offset < 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Read, core::IoError::value("negative offset not allowed")));
        return;
    }
    ensure_uring(operation);
    const auto plan = state_.plan_read_at(offset, size, [&] { return refresh_size(); });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    auto* request = make_req_read_bytes(plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(request, IORING_OP_READ,
              std::as_bytes(std::span{request->buf(), plan.size}), static_cast<uint64_t>(offset));
}

void IOUringBackend::write(core::Buffer view, int64_t position, core::OperationPtr operation) {
    if (position >= 0 && (state_.append_mode() || (fcntl(fd_, F_GETFL) & O_APPEND))) {
        throw core::IoException(core::IoError::value("write_at() does not support append mode"));
    }
    if (view.size() > UINT32_MAX) {
        throw core::IoException(core::IoError::overflow("write buffer exceeds 4 GiB - 1"));
    }
    if (closed()) return complete_closed(operation, core::ReqType::Write);
    ensure_uring(operation);
    if (view.size() == 0) {
        operation->complete_inline(core::Completion::success(core::ReqType::Write, 0));
        return;
    }
    const auto offset = state_.reserve_write(position, view.size());
    auto* request = make_req_held_write(std::move(view), operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(request, IORING_OP_WRITE,
              std::as_bytes(std::span{request->buf(), request->reqSize}), offset);
}

void IOUringBackend::seek(int64_t offset, int whence, core::OperationPtr operation) {
    if (whence < 0 || whence > 2) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::value("Invalid whence value")));
        return;
    }
    const auto position = state_.seek(offset, whence, [&] { return refresh_size(); });
    if (!position) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::value("negative seek position")));
        return;
    }
    operation->complete_inline(core::Completion::counted(core::ReqType::Other, *position));
}

void IOUringBackend::flush(core::OperationPtr operation) {
    if (closed()) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::native(0, "flush on closed file")));
        return;
    }
    ensure_uring(operation);
    auto* request = make_req(0, operation, core::ReqType::Other);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(request, IORING_OP_FSYNC, std::span<const std::byte>{}, 0);
}

void IOUringBackend::close(core::OperationPtr operation) {
    close_impl();
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void IOUringBackend::tell(core::OperationPtr operation) {
    operation->complete_inline(core::Completion::counted(core::ReqType::Other, state_.tell()));
}

void IOUringBackend::truncate(int64_t size, core::OperationPtr operation) {
    if (size < 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::value("negative size not allowed")));
        return;
    }
    if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::native(
                static_cast<uint32_t>(errno), "truncate failed")));
        return;
    }
    state_.apply_truncate(static_cast<uint64_t>(size));
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void IOUringBackend::readinto(core::Buffer buffer, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    if (buffer.size() == 0) {
        auto result = core::Completion::success(core::ReqType::Read, 0);
        result.return_count = true;
        operation->complete_inline(std::move(result));
        return;
    }
    ensure_uring(operation);
    const auto plan = state_.plan_read(static_cast<int64_t>(buffer.size()), [&] {
        return refresh_size();
    });
    if (plan.eof) {
        auto result = core::Completion::success(core::ReqType::Read, 0);
        result.return_count = true;
        operation->complete_inline(std::move(result));
        return;
    }
    auto* request = make_req_readinto(std::move(buffer), plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_io(request, IORING_OP_READ,
              std::as_bytes(std::span{request->buf(), plan.size}), plan.offset);
}

void IOUringBackend::close_impl() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(m_cached_close_timeout_ms);
    while (m_pending.load(std::memory_order_acquire) > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) break;
        m_close_wake.try_acquire_for(remaining);
    }
    uring_.reset();
    if (m_owns_fd && fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace ayafileio::backend::linux

#endif
