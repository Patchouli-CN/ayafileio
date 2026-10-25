#include "iocp_context.hpp"

#include "../../core/config.hpp"
#include "../../core/result.hpp"

#include <algorithm>
#include <chrono>

namespace ayafileio::backend::windows {

IOCPContext& IOCPContext::instance() {
    static IOCPContext context;
    return context;
}

IOCPContext::~IOCPContext() {
    if (running()) shutdown();
}

void IOCPContext::init() {
    if (running()) return;

    read_iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!read_iocp_) {
        throw core::IoException(core::IoError::runtime("Failed to create read IOCP"));
    }
    write_iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!write_iocp_) {
        CloseHandle(read_iocp_);
        read_iocp_ = nullptr;
        throw core::IoException(core::IoError::runtime("Failed to create write IOCP"));
    }

    running_.store(true, std::memory_order_release);
    read_worker_ = std::thread(&IOCPContext::worker_proc, this, read_iocp_);
    write_worker_ = std::thread(&IOCPContext::worker_proc, this, write_iocp_);
}

void IOCPContext::shutdown() {
    if (!running()) return;
    close_all_sessions();
    running_.store(false, std::memory_order_release);
    if (read_iocp_) PostQueuedCompletionStatus(read_iocp_, 0, 0, nullptr);
    if (write_iocp_) PostQueuedCompletionStatus(write_iocp_, 0, 0, nullptr);
    if (read_worker_.joinable()) read_worker_.join();
    if (write_worker_.joinable()) write_worker_.join();
    if (read_iocp_) {
        CloseHandle(read_iocp_);
        read_iocp_ = nullptr;
    }
    if (write_iocp_) {
        CloseHandle(write_iocp_);
        write_iocp_ = nullptr;
    }
    std::unique_lock lock(sessions_mutex_);
    sessions_.clear();
}

uint64_t IOCPContext::create_session(
    HANDLE handle,
    bool append_mode,
    DWORD access,
    bool owns_fd) {
    auto& cfg = core::config();
    auto session = std::make_shared<Session>(append_mode);
    session->id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    session->handle = handle;
    session->appendMode = append_mode;
    session->access = access;
    session->owns_fd = owns_fd;
    session->cached_buffer_size = cfg.buffer_size();
    session->cached_buffer_pool_max = cfg.buffer_pool_max();
    session->cached_close_timeout_ms = cfg.close_timeout_ms();

    HANDLE iocp = (access & GENERIC_WRITE) && !(access & GENERIC_READ)
        ? write_iocp_
        : read_iocp_;
    if (!CreateIoCompletionPort(handle, iocp, static_cast<ULONG_PTR>(session->id), 0)) {
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(GetLastError()), "Failed to associate handle with IOCP"));
    }
    session->skipCPOnSuccess = SetFileCompletionNotificationModes(
        handle, FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0;

    if (append_mode) {
        if (auto size = refresh_session_size(*session)) {
            session->state.set_cached_size(*size);
            session->state.set_file_pos(*size);
        }
    }
    session->running.store(true, std::memory_order_release);

    const auto id = session->id;
    {
        std::unique_lock lock(sessions_mutex_);
        sessions_[id] = std::move(session);
    }
    return id;
}

std::shared_ptr<Session> IOCPContext::get_session(uint64_t id) const {
    std::shared_lock lock(sessions_mutex_);
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

void IOCPContext::remove_session(uint64_t id) {
    std::shared_ptr<Session> victim;
    {
        std::unique_lock lock(sessions_mutex_);
        const auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            victim = std::move(it->second);
            sessions_.erase(it);
        }
    }
}

HANDLE IOCPContext::iocp_for_session(const std::shared_ptr<Session>& session) const {
    return (session->access & GENERIC_WRITE) && !(session->access & GENERIC_READ)
        ? write_iocp_
        : read_iocp_;
}

void IOCPContext::close_all_sessions() {
    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::shared_lock lock(sessions_mutex_);
        snapshot.reserve(sessions_.size());
        for (const auto& entry : sessions_) snapshot.push_back(entry.second);
    }

    for (auto& session : snapshot) {
        bool expected = true;
        if (!session->running.compare_exchange_strong(expected, false)) continue;
        if (session->handle != INVALID_HANDLE_VALUE) {
            if (session->pending.load(std::memory_order_acquire) > 0) {
                CancelIoEx(session->handle, nullptr);
                int wait_ms = 1;
                int elapsed = 0;
                while (elapsed < 500 && session->pending.load(std::memory_order_acquire) > 0) {
                    Sleep(wait_ms);
                    elapsed += wait_ms;
                    wait_ms = (std::min)(wait_ms * 2, 32);
                }
            }
            if (session->owns_fd) CloseHandle(session->handle);
            session->handle = INVALID_HANDLE_VALUE;
        }
    }

    std::unique_lock lock(sessions_mutex_);
    sessions_.clear();
}

} // namespace ayafileio::backend::windows
