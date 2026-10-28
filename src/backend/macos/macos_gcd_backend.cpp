#ifdef __APPLE__

#include "macos_gcd_backend.hpp"
#include "../../core/config.hpp"
#include "../../core/file_lifecycle.hpp"
#include "../../core/file_mode.hpp"
#include "../../core/global_thread_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ayafileio::backend::macos {
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

struct ParallelReadGroup {
    core::IORequest* request;
    std::atomic<std::size_t> remaining;
    std::atomic<std::size_t> copied{0};
    std::atomic<int> first_errno{0};
};

} // namespace

MacOSGCDBackend::MacOSGCDBackend(const std::string& path, const std::string& mode)
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
    init_channel();
}

MacOSGCDBackend::MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd)
    : state_(parse_native_mode(mode).appendMode) {
    fd_ = fd;
    m_owns_fd = owns_fd;
    auto& cfg = core::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    init_channel();
}

void MacOSGCDBackend::init_channel() {
    const int gcd_fd = dup(fd_);
    if (gcd_fd == -1) {
        const int error = errno;
        if (m_owns_fd) ::close(fd_);
        fd_ = -1;
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(error), "Failed to dup file descriptor for GCD"));
    }
    queue_ = dispatch_queue_create(
        "com.ayafileio.gcd",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0));
    auto cleanup = std::make_shared<GcdFdCleanup>();
    cleanup->fd = gcd_fd;
    cleanup->done = dispatch_semaphore_create(0);
    channel_ = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        gcd_fd,
        queue_,
        ^(int) {
            ::close(cleanup->fd);
            dispatch_semaphore_signal(cleanup->done);
        });
    if (!channel_) {
        ::close(gcd_fd);
        if (m_owns_fd) ::close(fd_);
        fd_ = -1;
        throw core::IoException(core::IoError::runtime("Failed to create dispatch I/O channel"));
    }
    fd_cleanup_ = std::move(cleanup);
    dispatch_io_set_high_water(channel_, m_cached_buffer_size);
    dispatch_io_set_low_water(channel_, m_cached_buffer_size / 4);
    running_.store(true, std::memory_order_release);

    struct stat st {};
    if (fstat(fd_, &st) == 0) {
        state_.set_cached_size(static_cast<uint64_t>(st.st_size));
        if (state_.append_mode()) state_.set_file_pos(static_cast<uint64_t>(st.st_size));
    }
    if (state_.cached_size() > 0) {
        void* mapping = mmap(nullptr, state_.cached_size(), PROT_READ, MAP_SHARED, fd_, 0);
        if (mapping != MAP_FAILED) {
            map_base_ = mapping;
            map_size_ = state_.cached_size();
        }
    }
}

MacOSGCDBackend::~MacOSGCDBackend() {
    close_impl();
}

void MacOSGCDBackend::ensure_pool() {
    core::GlobalThreadPool::instance().ensure_started(core::effective_io_worker_count());
}

std::optional<uint64_t> MacOSGCDBackend::refresh_size() {
    struct stat st {};
    if (fstat(fd_, &st) != 0 || st.st_size < 0) return std::nullopt;
    return static_cast<uint64_t>(st.st_size);
}

void MacOSGCDBackend::refresh_after_short_read() {
    if (auto size = refresh_size()) state_.set_cached_size(*size);
}

bool MacOSGCDBackend::closed() const noexcept {
    return !running_.load(std::memory_order_acquire) || fd_ == -1;
}

void MacOSGCDBackend::complete_closed(const core::OperationPtr& operation, core::ReqType type) {
    operation->complete_inline(core::Completion::failure(
        type, core::IoError::value("I/O operation on closed file.")));
}

void MacOSGCDBackend::submit_read_fast(core::IORequest* request, uint64_t offset, std::size_t size) {
    ensure_pool();
    core::GlobalThreadPool::instance().enqueue([this, request, offset, size]() {
        const auto got = pread(fd_, request->buf(), size, static_cast<off_t>(offset));
        if (got >= 0) complete_ok(request, static_cast<std::size_t>(got));
        else complete_error(request, core::IoError::native(
            static_cast<uint32_t>(errno), "I/O operation failed"));
    });
}

void MacOSGCDBackend::submit_write_fast(core::IORequest* request, uint64_t offset, std::size_t size) {
    ensure_pool();
    core::GlobalThreadPool::instance().enqueue([this, request, offset, size]() {
        const auto wrote = pwrite(fd_, request->buf(), size, static_cast<off_t>(offset));
        if (wrote >= 0) complete_ok(request, static_cast<std::size_t>(wrote));
        else complete_error(request, core::IoError::native(
            static_cast<uint32_t>(errno), "I/O operation failed"));
    });
}

void MacOSGCDBackend::submit_read_parallel(core::IORequest* request, uint64_t offset, std::size_t size) {
    ensure_pool();
    std::size_t chunk_size = parallel_read_chunk;
    while ((size + chunk_size - 1) / chunk_size > 256) chunk_size *= 2;
    const std::size_t chunks = (size + chunk_size - 1) / chunk_size;
    auto* group = new ParallelReadGroup{request, chunks};
    const int fd = fd_;
    for (std::size_t index = 0; index < chunks; ++index) {
        const std::size_t inner_offset = index * chunk_size;
        const std::size_t length = std::min(chunk_size, size - inner_offset);
        core::GlobalThreadPool::instance().enqueue([this, group, fd, offset, inner_offset, length]() {
            ssize_t got;
            do {
                got = pread(fd_, group->request->buf() + inner_offset, length,
                            static_cast<off_t>(offset + inner_offset));
            } while (got < 0 && errno == EINTR);
            if (got < 0) {
                int expected = 0;
                group->first_errno.compare_exchange_strong(expected, errno);
            } else {
                group->copied.fetch_add(static_cast<std::size_t>(got));
            }
            if (group->remaining.fetch_sub(1) == 1) {
                auto* request = group->request;
                const auto total = group->copied.load();
                const auto error = group->first_errno.load();
                delete group;
                if (error) complete_error(request, core::IoError::native(
                    static_cast<uint32_t>(error), "I/O operation failed"));
                else complete_ok(request, total);
            }
        });
    }
}

void MacOSGCDBackend::submit_read(core::IORequest* request, uint64_t offset, std::size_t size) {
    if (size >= parallel_read_threshold) return submit_read_parallel(request, offset, size);
    if (size >= large_io_threshold) return submit_read_fast(request, offset, size);
    if (!request->isReadinto && try_inline_read(request, offset, size)) return;

    __block std::size_t total = 0;
    dispatch_io_read(channel_, offset, size, queue_, ^(bool done, dispatch_data_t data, int error) {
        if (error) {
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(error), "I/O operation failed"));
            return;
        }
        if (data) {
            const auto chunk_size = dispatch_data_get_size(data);
            dispatch_data_apply(data, ^bool(dispatch_data_t, std::size_t offset, const void* buffer, std::size_t length) {
                std::memcpy(request->buf() + total + offset, buffer, length);
                return true;
            });
            total += chunk_size;
        }
        if (done) complete_ok(request, total);
    });
}

void MacOSGCDBackend::read(int64_t size, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    const auto plan = state_.plan_read(size, [&] { return refresh_size(); });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    auto* request = make_req_read_bytes(plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_read(request, plan.offset, plan.size);
}

void MacOSGCDBackend::read_at(int64_t offset, int64_t size, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    if (offset < 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Read, core::IoError::value("negative offset not allowed")));
        return;
    }
    const auto plan = state_.plan_read_at(offset, size, [&] { return refresh_size(); });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    auto* request = make_req_read_bytes(plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    submit_read(request, static_cast<uint64_t>(offset), plan.size);
}

void MacOSGCDBackend::write(core::Buffer view, int64_t position, core::OperationPtr operation) {
    if (position >= 0 && (state_.append_mode() || (fcntl(fd_, F_GETFL) & O_APPEND))) {
        throw core::IoException(core::IoError::value("write_at() does not support append mode"));
    }
    if (view.size() > UINT32_MAX) {
        throw core::IoException(core::IoError::overflow("write buffer exceeds 4 GiB - 1"));
    }
    if (closed()) return complete_closed(operation, core::ReqType::Write);
    if (view.size() == 0) {
        operation->complete_inline(core::Completion::success(core::ReqType::Write, 0));
        return;
    }

    const auto offset = state_.reserve_write(position, view.size());
    const auto size = view.size();
    if (size >= large_io_threshold) {
        auto* request = make_req_held_write(std::move(view), operation);
        m_pending.fetch_add(1, std::memory_order_relaxed);
        submit_write_fast(request, offset, size);
        return;
    }

    auto* request = make_req(size, operation, core::ReqType::Write);
    std::memcpy(request->buf(), view.data(), size);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    dispatch_data_t write_data = dispatch_data_create(
        request->buf(), size, queue_, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    request->release_buffer_ownership();
    dispatch_io_write(channel_, offset, write_data, queue_, ^(bool done, dispatch_data_t, int error) {
        if (error) {
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(error), "I/O operation failed"));
            return;
        }
        if (done) complete_ok(request, size);
    });
}

void MacOSGCDBackend::seek(int64_t offset, int whence, core::OperationPtr operation) {
    operation->complete_inline(core::seek_completion(
        state_, offset, whence, [&] { return refresh_size(); }));
}

void MacOSGCDBackend::flush(core::OperationPtr operation) {
    if (closed()) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::native(0, "flush on closed file")));
        return;
    }
    if (fsync(fd_) != 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::native(
                static_cast<uint32_t>(errno), "fsync failed")));
        return;
    }
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void MacOSGCDBackend::close(core::OperationPtr operation) {
    close_impl();
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void MacOSGCDBackend::tell(core::OperationPtr operation) {
    operation->complete_inline(core::tell_completion(state_));
}

void MacOSGCDBackend::truncate(int64_t size, core::OperationPtr operation) {
    if (auto rejected = core::truncate_guard(size)) {
        operation->complete_inline(std::move(*rejected));
        return;
    }
    if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        operation->complete_inline(core::truncate_failed(static_cast<uint32_t>(errno)));
        return;
    }
    operation->complete_inline(core::truncate_done(state_, size));
}

void MacOSGCDBackend::readinto(core::Buffer buffer, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);
    if (buffer.size() == 0) {
        auto result = core::Completion::success(core::ReqType::Read, 0);
        result.return_count = true;
        operation->complete_inline(std::move(result));
        return;
    }
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
    submit_read(request, plan.offset, plan.size);
}

bool MacOSGCDBackend::try_inline_read(core::IORequest* request, uint64_t offset, std::size_t size) {
    const char* base = static_cast<const char*>(map_base_);
    if (!base || map_size_ == 0) return false;
    if (offset > map_size_ || size > map_size_ - offset) return false;

    static const std::size_t page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const std::size_t first_page = offset / page_size;
    const std::size_t pages = (offset + size - 1) / page_size - first_page + 1;
    if (pages > 64) return false;
    char vec[64] = {0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const int result = mincore(
        const_cast<void*>(static_cast<const void*>(base + first_page * page_size)),
        pages * page_size,
        vec);
#pragma clang diagnostic pop
    if (result != 0) return false;
    for (std::size_t index = 0; index < pages; ++index) {
        if (!(vec[index] & 1)) return false;
    }

    ssize_t got;
    do {
        got = pread(fd_, request->buf(), size, static_cast<off_t>(offset));
    } while (got < 0 && errno == EINTR);
    complete_inline(request, got);
    return true;
}

void MacOSGCDBackend::close_impl() {
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

    if (map_base_) {
        munmap(const_cast<void*>(map_base_), map_size_);
        map_base_ = nullptr;
        map_size_ = 0;
    }

    dispatch_io_t channel = nullptr;
    if (channel_) {
        dispatch_io_close(channel_, DISPATCH_IO_STOP);
        channel = channel_;
        channel_ = nullptr;
    }
    if (channel) dispatch_release(channel);

    if (fd_cleanup_) {
        auto cleanup = fd_cleanup_;
        const auto timeout = static_cast<int64_t>(m_cached_close_timeout_ms) * 1000000LL;
        dispatch_semaphore_wait(cleanup->done, dispatch_time(DISPATCH_TIME_NOW, timeout));
        fd_cleanup_.reset();
    }
    if (queue_) {
        dispatch_barrier_sync(queue_, ^{});
        dispatch_release(queue_);
        queue_ = nullptr;
    }
    if (m_owns_fd && fd_ != -1) ::close(fd_);
    fd_ = -1;
}

} // namespace ayafileio::backend::macos

#endif
