#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ayafileio::core {

struct ReadPlan {
    uint64_t offset{0};
    std::size_t size{0};
    bool eof{true};
};

class FileState {
public:
    explicit FileState(bool append = false) : append_mode_(append) {}

    template<typename Refresh>
    ReadPlan plan_read(int64_t requested, Refresh&& refresh) {
        std::lock_guard lock(mutex_);
        if (remaining_unlocked(file_pos_) <= 0) if (auto refreshed = refresh(); refreshed) cached_size_ = *refreshed;
        return plan_read_locked(file_pos_, requested, true);
    }

    template<typename Refresh>
    ReadPlan plan_read_at(int64_t offset, int64_t requested, Refresh&& refresh) {
        if (offset < 0) return {};
        std::lock_guard lock(mutex_);
        if (remaining_unlocked(static_cast<uint64_t>(offset)) <= 0) if (auto refreshed = refresh(); refreshed) cached_size_ = *refreshed;
        return plan_read_locked(static_cast<uint64_t>(offset), requested, false);
    }

    uint64_t reserve_write(int64_t position, std::size_t size);
    void apply_truncate(uint64_t size);

    template<typename Refresh>
    std::optional<uint64_t> seek(int64_t offset, int whence, Refresh&& refresh) {
        std::lock_guard lock(mutex_);
        int64_t target = 0;
        if (whence == 0) {
            target = offset;
        } else if (whence == 1) {
            target = static_cast<int64_t>(file_pos_) + offset;
        } else if (whence == 2) {
            if (auto refreshed = refresh(); refreshed) cached_size_ = *refreshed;
            target = static_cast<int64_t>(cached_size_) + offset;
        } else {
            return std::nullopt;
        }
        if (target < 0) return std::nullopt;
        file_pos_ = static_cast<uint64_t>(target);
        return file_pos_;
    }

    uint64_t tell() const;
    uint64_t cached_size() const;
    void set_cached_size(uint64_t size);
    void set_file_pos(uint64_t position);
    bool append_mode() const noexcept { return append_mode_; }

private:
    int64_t remaining_unlocked(uint64_t offset) const {
        return static_cast<int64_t>(cached_size_) - static_cast<int64_t>(offset);
    }

    ReadPlan plan_read_locked(uint64_t offset, int64_t requested, bool sequential) {
        const int64_t remaining = remaining_unlocked(offset);
        if (remaining <= 0) return {offset, 0, true};
        std::size_t size = requested < 0
            ? static_cast<std::size_t>(remaining)
            : std::min<std::size_t>(static_cast<std::size_t>(requested), static_cast<std::size_t>(remaining));
        if (size == 0) return {offset, 0, true};
        if (sequential) file_pos_ = offset + size;
        return {offset, size, false};
    }

    mutable std::mutex mutex_;
    uint64_t file_pos_{0};
    uint64_t cached_size_{0};
    bool append_mode_{false};
};

} // namespace ayafileio::core
