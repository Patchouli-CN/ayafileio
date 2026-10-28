#include "thread_io_backend.hpp"

#include "../../core/config.hpp"
#include "../../core/file_lifecycle.hpp"
#include "../../core/file_mode.hpp"
#include "../../core/global_thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace ayafileio::backend::thread {
namespace {

core::ModeInfo parse_native_mode(const std::string& mode) {
    try {
        return core::parse_mode(mode);
    } catch (const std::invalid_argument& error) {
        throw core::IoException(core::IoError::value(error.what()));
    }
}

#ifdef _WIN32
void __cdecl ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}
#endif

} // namespace

ThreadIOBackend::ThreadIOBackend(const std::string& path, const std::string& mode)
    : state_(parse_native_mode(mode).appendMode) {
    auto& cfg = core::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();

    const auto info = parse_native_mode(mode);
    int flags = platform::kOpenRead;
    if (info.hasW) flags = platform::kOpenWrite | platform::kOpenCreate | platform::kOpenTruncate;
    else if (info.hasA) flags = platform::kOpenWrite | platform::kOpenCreate | platform::kOpenAppend;
    else if (info.hasX) flags = platform::kOpenWrite | platform::kOpenCreate | platform::kOpenExclusive;
    if (info.plus) {
        flags = platform::kOpenRead | platform::kOpenWrite |
                (flags & ~(platform::kOpenRead | platform::kOpenWrite));
    }

    fd_ = platform::file_open(path.c_str(), flags);
    if (fd_ == platform::kFileInvalid) {
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(platform::native_file_last_error()),
            "Failed to open file: " + path));
    }

    running_.store(true, std::memory_order_release);
    if (info.appendMode) {
        unsigned long long size = 0;
        if (platform::file_size(fd_, &size)) {
            state_.set_cached_size(size);
            state_.set_file_pos(size);
        }
    }
}

ThreadIOBackend::ThreadIOBackend(int fd, const std::string& mode, bool owns_fd)
    : state_(parse_native_mode(mode).appendMode) {
#ifdef _WIN32
    (void)fd;
    (void)mode;
    (void)owns_fd;
    throw core::IoException(core::IoError::value(
        "ThreadIOBackend does not support wrapping an existing fd on Windows"));
#else
    fd_ = fd;
    m_owns_fd = owns_fd;
    const auto info = parse_native_mode(mode);
    auto& cfg = core::config();
    m_cached_buffer_size = cfg.buffer_size();
    m_cached_buffer_pool_max = cfg.buffer_pool_max();
    m_cached_close_timeout_ms = cfg.close_timeout_ms();
    running_.store(true, std::memory_order_release);

    unsigned long long size = 0;
    if (platform::file_size(fd_, &size)) {
        state_.set_cached_size(size);
        if (info.appendMode) state_.set_file_pos(size);
    }
#endif
}

ThreadIOBackend::~ThreadIOBackend() {
    close_impl();
}

std::optional<uint64_t> ThreadIOBackend::refresh_size() {
    unsigned long long size = 0;
    if (!platform::file_size(fd_, &size)) return std::nullopt;
    return static_cast<uint64_t>(size);
}

void ThreadIOBackend::refresh_after_short_read() {
    if (auto size = refresh_size()) state_.set_cached_size(*size);
}

void ThreadIOBackend::enqueue_task(std::function<void()> task) {
    auto& pool = core::GlobalThreadPool::instance();
    pool.ensure_started(core::effective_io_worker_count());
    pool.enqueue(std::move(task));
}

bool ThreadIOBackend::closed() const noexcept {
    return !running_.load(std::memory_order_acquire) || fd_ == platform::kFileInvalid;
}

void ThreadIOBackend::complete_closed(const core::OperationPtr& operation, core::ReqType type) {
    operation->complete_inline(core::Completion::failure(
        type, core::IoError::value("I/O operation on closed file.")));
}

void ThreadIOBackend::read(int64_t size, core::OperationPtr operation) {
    if (closed()) return complete_closed(operation, core::ReqType::Read);

    const auto plan = state_.plan_read(size, [&] { return refresh_size(); });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }

    auto* request = make_req_read_bytes(plan.size, operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    const auto offset = plan.offset;
    const auto read_size = plan.size;
    enqueue_task([this, request, offset, read_size]() {
        const auto got = platform::file_pread(
            fd_, request->buf(), read_size, static_cast<long long>(offset));
        if (got >= 0) {
            if (static_cast<std::size_t>(got) < read_size) refresh_after_short_read();
            complete_ok(request, static_cast<std::size_t>(got));
        } else {
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(platform::native_file_last_error()),
                "I/O operation failed"));
        }
    });
}

void ThreadIOBackend::read_at(int64_t offset, int64_t size, core::OperationPtr operation) {
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
    const auto read_size = plan.size;
    enqueue_task([this, request, offset, read_size]() {
        const auto got = platform::file_pread(
            fd_, request->buf(), read_size, offset);
        if (got >= 0) {
            if (static_cast<std::size_t>(got) < read_size) refresh_after_short_read();
            complete_ok(request, static_cast<std::size_t>(got));
        } else {
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(platform::native_file_last_error()),
                "I/O operation failed"));
        }
    });
}

void ThreadIOBackend::write(core::Buffer view, int64_t position, core::OperationPtr operation) {
    if (position >= 0 && (state_.append_mode() || platform::file_is_append(fd_))) {
        throw core::IoException(core::IoError::value("write_at() does not support append mode"));
    }
    if (view.size() > UINT32_MAX) {
        throw core::IoException(core::IoError::overflow("write buffer exceeds 4 GiB - 1"));
    }
    if (closed()) return complete_closed(operation, core::ReqType::Write);

    const std::size_t size = view.size();
    if (size == 0) {
        operation->complete_inline(core::Completion::success(core::ReqType::Write, 0));
        return;
    }

    const auto offset = state_.reserve_write(position, size);
    auto* request = make_req_held_write(std::move(view), operation);
    m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue_task([this, request, offset, size]() {
        const auto wrote = platform::file_pwrite(
            fd_, request->buf(), size, static_cast<long long>(offset));
        if (wrote >= 0) {
            complete_ok(request, static_cast<std::size_t>(wrote));
        } else {
            refresh_after_short_read();
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(platform::native_file_last_error()),
                "I/O operation failed"));
        }
    });
}

void ThreadIOBackend::seek(int64_t offset, int whence, core::OperationPtr operation) {
    operation->complete_inline(core::seek_completion(
        state_, offset, whence, [&] { return refresh_size(); }));
}

void ThreadIOBackend::flush(core::OperationPtr operation) {
    if (closed()) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other, core::IoError::native(0, "flush on closed file")));
        return;
    }
    if (platform::file_fsync(fd_) != 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Other,
            core::IoError::native(
                static_cast<uint32_t>(platform::native_file_last_error()), "fsync failed")));
        return;
    }
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void ThreadIOBackend::close(core::OperationPtr operation) {
    close_impl();
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

void ThreadIOBackend::tell(core::OperationPtr operation) {
    operation->complete_inline(core::tell_completion(state_));
}

void ThreadIOBackend::truncate(int64_t size, core::OperationPtr operation) {
    if (auto rejected = core::truncate_guard(size)) {
        operation->complete_inline(std::move(*rejected));
        return;
    }
    if (platform::file_truncate(fd_, static_cast<unsigned long long>(size)) != 0) {
        operation->complete_inline(core::truncate_failed(static_cast<uint32_t>(platform::native_file_last_error())));
        return;
    }
    operation->complete_inline(core::truncate_done(state_, size));
}

void ThreadIOBackend::readinto(core::Buffer buffer, core::OperationPtr operation) {
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
    const auto offset = plan.offset;
    const auto read_size = plan.size;
    enqueue_task([this, request, offset, read_size]() {
        const auto got = platform::file_pread(
            fd_, request->buf(), read_size, static_cast<long long>(offset));
        if (got >= 0) {
            if (static_cast<std::size_t>(got) < read_size) refresh_after_short_read();
            complete_ok(request, static_cast<std::size_t>(got));
        } else {
            complete_error(request, core::IoError::native(
                static_cast<uint32_t>(platform::native_file_last_error()),
                "I/O operation failed"));
        }
    });
}

int ThreadIOBackend::fileno() const {
#ifdef _WIN32
    std::lock_guard lock(fileno_mutex_);
    if (cached_fileno_ >= 0) return cached_fileno_;
    if (fd_ == platform::kFileInvalid) return -1;

    HANDLE duplicated = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(fd_),
                         GetCurrentProcess(), &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicated), _O_RDWR | _O_BINARY);
    if (fd == -1) {
        CloseHandle(duplicated);
        return -1;
    }
    cached_fileno_ = fd;
    cached_dup_handle_ = duplicated;
    return fd;
#else
    return closed() ? -1 : static_cast<int>(fd_);
#endif
}

void ThreadIOBackend::close_impl() {
#ifdef _WIN32
    {
        std::lock_guard lock(fileno_mutex_);
        if (cached_fileno_ >= 0) {
            auto old = _set_thread_local_invalid_parameter_handler(ignore_invalid_parameter);
            const intptr_t slot = _get_osfhandle(cached_fileno_);
            if (slot != -1 && reinterpret_cast<HANDLE>(slot) == cached_dup_handle_) {
                _close(cached_fileno_);
            }
            _set_thread_local_invalid_parameter_handler(old);
            cached_fileno_ = -1;
            cached_dup_handle_ = INVALID_HANDLE_VALUE;
        }
    }
#endif

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

    if (m_owns_fd && fd_ != platform::kFileInvalid) {
        platform::file_close(fd_);
        fd_ = platform::kFileInvalid;
    }
}

} // namespace ayafileio::backend::thread
