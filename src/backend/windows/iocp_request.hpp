#pragma once

#include "../../core/io_request.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ayafileio::backend::windows {

struct IocpRequest final : core::IORequest {
    OVERLAPPED ov{};
    // 提交时所属会话。完成包只带 completion key（= 会话 id），本身不带身份；当这个
    // 请求对象被释放、地址又被另一个会话的新请求复用时，一个来自**已关闭会话**的
    // 陈旧包就会落到那个好请求上。比对两个 id 就能认出它并丢弃，而不是把
    // "文件已关闭"的错误投给这个请求。见 process_one。
    std::uint64_t session_id{0};
};

IocpRequest* iocp_req_alloc_impl(const char* file, int line, const char* func);
void iocp_req_free_impl(IocpRequest* request, const char* file, int line, const char* func);

// 提交线程与 worker 线程都会调用，纯粹计数，用于确认"同一 OVERLAPPED 被投了
// 多少次包"这个前提本身。
inline std::atomic<std::uint64_t> g_packets_total{0};
inline std::atomic<std::uint64_t> g_packets_after_resolved{0};
inline std::atomic<std::uint64_t> g_packets_rejected_uaf{0};
inline std::atomic<std::uint64_t> g_packets_stale_session{0};

} // namespace ayafileio::backend::windows

#define IOCP_REQ_ALLOC() ayafileio::backend::windows::iocp_req_alloc_impl(__FILE__, __LINE__, __FUNCTION__)
#define IOCP_REQ_FREE(p) ayafileio::backend::windows::iocp_req_free_impl((p), __FILE__, __LINE__, __FUNCTION__)
