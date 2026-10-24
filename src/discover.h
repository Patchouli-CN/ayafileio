#pragma once

// ════════════════════════════════════════════════════════════════════════════
// discover.h — 运行时能力发现
//
// 原则：探测能力，不解析版本号。能问内核的（io_uring PROBE / 试建 ring）
// 直接问内核；只有无处可问时才退回 uname / RtlGetVersion 报版本字符串。
//
// 所有探测结果进程内缓存（static），重复调用零开销。
// ════════════════════════════════════════════════════════════════════════════

#include "globals.hpp"
#include "config.hpp"
#include <string>

namespace ayafileio {

// ── 后端解析 ────────────────────────────────────────────────────────────────
// effective_backend(): 把 configure({"backend": ...}) 的强制值解析成实际要用的
// 后端——Auto 走平台探测，强制值再按可用性兜一次底（configure 已经校验过，
// 这里兜底是为了「配置是旧的/平台不支持」时不至于构造出不存在的东西）。
BackendKind effective_backend();

// 后端名（与 Python 侧的字符串一致：iocp / io_uring / dispatch_io / thread_pool）
const char *backend_name(BackendKind kind);

// 名字 → 枚举；未知名字返回 false
bool parse_backend_name(const std::string &name, BackendKind &out);

// 该后端在**本构建**里是否存在且运行时可用
bool backend_available(BackendKind kind);

// ── OS / 内核版本字符串 ─────────────────────────────────────────────────────
// Windows: RtlGetVersion（GetVersionEx 会受 manifest 兼容模式欺骗）
// POSIX:   uname(2) → "Linux 6.5.0-..." / "Darwin 24.5.0"
std::string os_version();

#ifdef HAVE_IO_URING
// io_uring 运行时可用性（试建一个 ring；结果缓存）
bool io_uring_available();

// SQPOLL 实测支持（试建 SQPOLL ring；结果缓存）。
// 5.11 之前需要特权，版本号说了不算，直接建一个最诚实
bool io_uring_sqpoll_supported();

// io_uring 能力明细：ring.features 特性位 + opcode PROBE + SQPOLL 实测。
// 调用前需确认 io_uring_available() 为 true。
py::dict io_uring_detail();
#endif

#ifdef __APPLE__
// Dispatch I/O 运行时可用性（建 queue + 临时文件 channel 实测；结果缓存）
bool gcd_dispatch_io_available();

// GCD 后端能力明细
py::dict gcd_detail();
#endif

// ── 当前后端的能力明细（跨平台汇总入口）─────────────────────────────────────
// 返回 dict 的键随后端不同而不同：
//   iocp:        completion_model / batch_harvest
//   io_uring:    available / completion_model / features / opcodes / sqpoll
//   dispatch_io: dispatch_io / completion_model / kernel_managed_workqueue / ...
//   thread_pool: completion_model=false + note
py::dict backend_detail();

// ── 后端身份/能力（原 bindings.cpp 的 py_get_backend_info 一族）────────────
py::dict py_get_backend_info();
py::dict py_get_capabilities();

} // namespace ayafileio
