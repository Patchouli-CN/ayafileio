#include "iocp_request.hpp"

#include <new>
#include <vector>

namespace ayafileio::backend::windows {
namespace {

std::vector<void*>& freelist() {
    thread_local std::vector<void*> list;
    return list;
}

core::RequestLifecycleHooks& hooks() {
    static core::RequestLifecycleHooks value;
    return value;
}

} // namespace

IocpRequest* iocp_req_alloc_impl(const char* file, int line, const char* func) {
    auto& list = freelist();
    void* memory = nullptr;
    if (list.empty()) {
        memory = ::operator new(sizeof(IocpRequest));
    } else {
        memory = list.back();
        list.pop_back();
    }
    auto* request = new (memory) IocpRequest;
    core::register_allocation(request, sizeof(IocpRequest), file, line, func);
    return request;
}

void iocp_req_free_impl(IocpRequest* request, const char* file, int line, const char* func) {
    if (!request) return;
    if (!core::release_allocation(request, file, line, func)) {
        return;
    }
    request->~IocpRequest();
    auto& list = freelist();
    if (list.size() < 64) {
        list.push_back(request);
        return;
    }
    ::operator delete(request);
}

} // namespace ayafileio::backend::windows
