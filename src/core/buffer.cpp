#include "buffer.hpp"

#include "buffer_pool.hpp"

#include <cstdlib>
#include <new>
#include <utility>

namespace ayafileio::core {

Buffer::Buffer(Buffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      pooled_(std::exchange(other.pooled_, nullptr)),
      heap_owned_(std::exchange(other.heap_owned_, false)),
      owner_(std::move(other.owner_)),
      result_handle_(std::exchange(other.result_handle_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        pooled_ = std::exchange(other.pooled_, nullptr);
        heap_owned_ = std::exchange(other.heap_owned_, false);
        owner_ = std::move(other.owner_);
        result_handle_ = std::exchange(other.result_handle_, nullptr);
    }
    return *this;
}

Buffer::~Buffer() {
    reset();
}

Buffer Buffer::pool(std::size_t size) {
    Buffer result;
    result.pooled_ = pool_acquire_with_size(size);
    result.data_ = result.pooled_->data;
    result.size_ = result.pooled_->size;
    return result;
}

Buffer Buffer::heap(std::size_t size) {
    Buffer result;
    result.data_ = static_cast<char*>(std::malloc(size));
    if (!result.data_) throw std::bad_alloc();
    result.size_ = size;
    result.heap_owned_ = true;
    return result;
}

Buffer Buffer::external(
    char* data,
    std::size_t size,
    std::shared_ptr<void> owner,
    void* result_handle) {
    Buffer result;
    result.data_ = data;
    result.size_ = size;
    result.owner_ = std::move(owner);
    result.result_handle_ = result_handle;
    return result;
}

char* Buffer::release_ownership() {
    char* data = data_;
    if (pooled_) {
        pooled_->data = nullptr;
        delete pooled_;
        pooled_ = nullptr;
    }
    heap_owned_ = false;
    owner_.reset();
    data_ = nullptr;
    size_ = 0;
    result_handle_ = nullptr;
    return data;
}

void Buffer::reset() {
    if (pooled_) {
        pool_release(pooled_);
        pooled_ = nullptr;
    } else if (heap_owned_) {
        std::free(data_);
    }
    heap_owned_ = false;
    owner_.reset();
    data_ = nullptr;
    size_ = 0;
    result_handle_ = nullptr;
}

} // namespace ayafileio::core
