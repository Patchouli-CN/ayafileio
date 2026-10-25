#pragma once

#include "../../core/file_state.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <windows.h>

namespace ayafileio::backend::windows {

struct Session {
    uint64_t id{0};
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::atomic<bool> running{false};
    core::FileState state;
    bool appendMode{false};
    std::atomic<long> pending{0};
    std::counting_semaphore<INT32_MAX> close_wake{0};
    bool owns_fd{true};
    DWORD access{0};
    bool skipCPOnSuccess{false};
    std::size_t cached_buffer_size{65536};
    std::size_t cached_buffer_pool_max{512};
    unsigned cached_close_timeout_ms{4000};

    explicit Session(bool append = false) : state(append), appendMode(append) {}

    long complete_request() {
        const long previous = pending.fetch_sub(1, std::memory_order_release);
        close_wake.release();
        return previous;
    }
};

std::optional<uint64_t> refresh_session_size(Session& session);
void refresh_session_size_after_short_io(Session& session);

} // namespace ayafileio::backend::windows
