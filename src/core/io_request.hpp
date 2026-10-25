#pragma once

#include "buffer.hpp"
#include "completion.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ayafileio::core {

class IOBackendBase;
struct IORequest;

enum class IOState : uint8_t { PENDING, RESOLVED, REJECTED };

struct RequestLifecycleHooks {
    void (*register_allocation)(void*, std::size_t, const char*, int, const char*){nullptr};
    bool (*release_allocation)(void*, const char*, int, const char*){nullptr};
    bool (*check_access)(void*, const char*, int, const char*){nullptr};
};

void set_request_lifecycle_hooks(RequestLifecycleHooks hooks);
void register_allocation(void* request, std::size_t size, const char* file, int line, const char* func);
bool release_allocation(void* request, const char* file, int line, const char* func);
bool request_access_valid(IORequest* request, const char* file, int line, const char* func);

struct IORequest {
    IOBackendBase* file{nullptr};
    std::shared_ptr<Operation> operation{};
    Buffer buffer{};
    std::size_t reqSize{0};
    ReqType type{ReqType::Other};
    std::atomic<IOState> state{IOState::PENDING};
    bool isReadinto{false};
    bool holdsWriteBuf{false};
    bool inline_completed{false};

    char* buf() noexcept { return buffer.data(); }
    const char* buf() const noexcept { return buffer.data(); }
    char* release_buffer_ownership() { return buffer.release_ownership(); }
};

IORequest* req_alloc_impl(const char* file, int line, const char* func);
void req_free_impl(IORequest* request, const char* file, int line, const char* func);

} // namespace ayafileio::core

#define REQ_ALLOC() ayafileio::core::req_alloc_impl(__FILE__, __LINE__, __FUNCTION__)
#define REQ_FREE(p) ayafileio::core::req_free_impl((p), __FILE__, __LINE__, __FUNCTION__)
