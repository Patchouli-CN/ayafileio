#pragma once

#include <cstddef>
#include <memory>

namespace ayafileio::core {

struct PoolBuf;

class Buffer {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    static Buffer pool(std::size_t size);
    static Buffer heap(std::size_t size);
    static Buffer external(
        char* data,
        std::size_t size,
        std::shared_ptr<void> owner,
        void* result_handle = nullptr);

    char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    void* result_handle() const noexcept { return result_handle_; }
    bool has_result() const noexcept { return result_handle_ != nullptr; }
    explicit operator bool() const noexcept { return data_ != nullptr; }

    char* release_ownership();
    void reset();

private:
    char* data_{nullptr};
    std::size_t size_{0};
    PoolBuf* pooled_{nullptr};
    bool heap_owned_{false};
    std::shared_ptr<void> owner_{};
    void* result_handle_{nullptr};
};

} // namespace ayafileio::core
