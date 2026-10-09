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
#include <string>

namespace ayafileio {

// ── OS / 内核版本字符串 ─────────────────────────────────────────────────────
// Windows: RtlGetVersion（GetVersionEx 会受 manifest 兼容模式欺骗）
// POSIX:   uname(2) → "Linux 6.5.0-..." / "Darwin 24.5.0"
std::string os_version();

#ifdef HAVE_IO_URING
// io_uring 运行时可用性（试建一个 ring；结果缓存）
bool io_uring_available();

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
//   iocp:        completion_model / batch_harvest / handle_pool
//   io_uring:    available / completion_model / features / opcodes / sqpoll
//   dispatch_io: dispatch_io / completion_model / kernel_managed_workqueue / ...
//   thread_pool: completion_model=false + note
py::dict backend_detail();

} // namespace ayafileio
