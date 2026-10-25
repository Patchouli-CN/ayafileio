#include "io_request.hpp"

#include <new>
#include <vector>

namespace ayafileio::core {
namespace {

RequestLifecycleHooks g_hooks{};

std::vector<void*>& request_freelist() {
    thread_local std::vector<void*> list;
    return list;
}

void* request_mem_acquire() {
    constexpr std::size_t max_entries = 64;
    auto& list = request_freelist();
    if (!list.empty()) {
        void* memory = list.back();
        list.pop_back();
        return memory;
    }
    (void)max_entries;
    return ::operator new(sizeof(IORequest));
}

void request_mem_release(void* memory) {
    constexpr std::size_t max_entries = 64;
    auto& list = request_freelist();
    if (list.size() < max_entries) {
        list.push_back(memory);
        return;
    }
    ::operator delete(memory);
}

} // namespace

void set_request_lifecycle_hooks(RequestLifecycleHooks hooks) {
    g_hooks = hooks;
}

void register_allocation(void* request, std::size_t size, const char* file, int line, const char* func) {
    if (g_hooks.register_allocation) g_hooks.register_allocation(request, size, file, line, func);
}

bool release_allocation(void* request, const char* file, int line, const char* func) {
    return !g_hooks.release_allocation || g_hooks.release_allocation(request, file, line, func);
}

bool request_access_valid(IORequest* request, const char* file, int line, const char* func) {
    return !g_hooks.check_access || g_hooks.check_access(request, file, line, func);
}

IORequest* req_alloc_impl(const char* file, int line, const char* func) {
    auto* request = new (request_mem_acquire()) IORequest;
    if (g_hooks.register_allocation) {
        g_hooks.register_allocation(request, sizeof(IORequest), file, line, func);
    }
    return request;
}

void req_free_impl(IORequest* request, const char* file, int line, const char* func) {
    if (!request) return;
    if (g_hooks.release_allocation && !g_hooks.release_allocation(request, file, line, func)) {
        return;
    }
    request->~IORequest();
    request_mem_release(request);
}

} // namespace ayafileio::core
