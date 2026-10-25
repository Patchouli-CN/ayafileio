#include "file_state.hpp"

namespace ayafileio::core {

uint64_t FileState::reserve_write(int64_t position, std::size_t size) {
    std::lock_guard lock(mutex_);
    const uint64_t offset = position < 0
        ? (append_mode_ ? cached_size_ : file_pos_)
        : static_cast<uint64_t>(position);
    if (position < 0) file_pos_ = offset + size;
    if (offset + size > cached_size_) cached_size_ = offset + size;
    return offset;
}

void FileState::apply_truncate(uint64_t size) {
    std::lock_guard lock(mutex_);
    cached_size_ = size;
    if (file_pos_ > size) file_pos_ = size;
}

uint64_t FileState::tell() const {
    std::lock_guard lock(mutex_);
    return file_pos_;
}

uint64_t FileState::cached_size() const {
    std::lock_guard lock(mutex_);
    return cached_size_;
}

void FileState::set_cached_size(uint64_t size) {
    std::lock_guard lock(mutex_);
    cached_size_ = size;
}

void FileState::set_file_pos(uint64_t position) {
    std::lock_guard lock(mutex_);
    file_pos_ = position;
}

} // namespace ayafileio::core
