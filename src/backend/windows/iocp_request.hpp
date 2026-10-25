#pragma once

#include "../../core/io_request.hpp"

#include <windows.h>

namespace ayafileio::backend::windows {

struct IocpRequest final : core::IORequest {
    OVERLAPPED ov{};
};

IocpRequest* iocp_req_alloc_impl(const char* file, int line, const char* func);
void iocp_req_free_impl(IocpRequest* request, const char* file, int line, const char* func);

} // namespace ayafileio::backend::windows

#define IOCP_REQ_ALLOC() ayafileio::backend::windows::iocp_req_alloc_impl(__FILE__, __LINE__, __FUNCTION__)
#define IOCP_REQ_FREE(p) ayafileio::backend::windows::iocp_req_free_impl((p), __FILE__, __LINE__, __FUNCTION__)
