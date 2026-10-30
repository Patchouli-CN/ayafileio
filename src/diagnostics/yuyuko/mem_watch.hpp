#pragma once
// ════════════════════════════════════════════════════════════════════════════
// mem_watch.hpp — 「谁写了这块内存」的监视器（Yuyuko 的一环）
//
// 与 mem_snapshot/mem_check（CRC，事后发现"内容变了"）互补：
// 这里回答的是 **哪条指令改的**。
//
// 做法：x64 数据断点。把被监视地址写进调试寄存器 Dr0..Dr3、Dr7 配成"写监视"，
// CPU 在这条写指令执行后立刻抛 STATUS_SINGLE_STEP；我们装一个
// Vectored Exception Handler 接住它，从 CONTEXT 里取 Rip 与 Dr6（哪一号槽命中），
// 打印模块名 + 偏移 + 线程 id。不需要调试器、不需要 PageHeap。
//
// 限制（x64 硬件特性，不是实现偷懒）：
//   * 最多同时 4 个监视（4 个调试寄存器）；
//   * 每个监视长度只能是 1/2/4/8 字节，且地址按长度对齐；
//   * 监视是**按线程**生效的：这里会给当前所有线程都装上，并给之后新建的线程
//     留一个 hook（ARM 后新起的线程需要在它第一次跑之前补装，见 mem_watch_arm_new_thread）。
// ════════════════════════════════════════════════════════════════════════════

#include <cstddef>

namespace ayafileio::diagnostics::yuyuko {

// 平台是否支持（目前仅 Windows x64）。
bool mem_watch_supported();

// 监视 [addr, addr + size) 的写入。size ∈ {1,2,4,8}，addr 需按 size 对齐。
// tag 只用于日志，建议写成 "ResultBatcher::m_mtx" 这种能直接定位的字符串。
// 返回 false 表示不支持 / 参数非法 / 4 个槽位已用完。
bool mem_watch(void* addr, std::size_t size, const char* tag);

// 清掉所有监视（把调试寄存器从所有线程上摘掉）。
void mem_watch_clear();

// 新线程需要补装监视时调用；返回已装上的监视条数。
std::size_t mem_watch_rearm_current_thread();

// 已安装的监视条数。
std::size_t mem_watch_count();

} // namespace ayafileio::diagnostics::yuyuko
