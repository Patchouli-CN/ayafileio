#include "iocp_context.hpp"

#include "../../core/completion.hpp"
#include "../../core/config.hpp"

#include <algorithm>
#include <chrono>
#include <deque>

namespace ayafileio::backend::windows {
namespace {

#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) \
    ((type*)((char*)(address) - (uintptr_t)(&((type*)0)->field)))
#endif

constexpr unsigned quarantine_ms = 1000;
// 注意：这里**不再有**积压上限。原因见下方 quarantine_drain 的注释：
// 任何按数量强制回收都会把 1 秒的时间保证压缩成「上限 / 吞吐」。
struct QuarantineEntry {
    IocpRequest* request;
    std::chrono::steady_clock::time_point when;
};

// 回收队列是 thread_local，而**两个线程都会用**：
//   * worker 线程 —— 完成包路径收尾；
//   * 提交（事件循环）线程 —— 同步成功 / 同步失败的收尾。
// 每个线程只推自己的队列、也只排空自己的队列，所以既不需要锁，也不会有人饿死。
// （曾经只有 worker 用它，那时提交线程根本不交还请求 —— 正是这一点让"立即释放"
// 看起来是必要的，而那正是崩溃的来源。）
thread_local std::deque<QuarantineEntry> quarantine;

std::atomic<uint64_t> q_live{0};
std::atomic<uint64_t> q_pushes{0};
std::atomic<uint64_t> q_scanned{0};
std::atomic<uint64_t> q_drains{0};
std::atomic<uint64_t> q_peak{0};

void note_push() {
    const auto live = q_live.fetch_add(1, std::memory_order_relaxed) + 1;
    q_pushes.fetch_add(1, std::memory_order_relaxed);
    auto peak = q_peak.load(std::memory_order_relaxed);
    while (live > peak && !q_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
}

// 唯一规则：**只回收已满 quarantine_ms 的条目，任何情况下都不越过它。**
//
// 这里原本还有一条"积压达到上限就按 FIFO 强制回收、忽略年龄"的分支，用来把内存钳住。
// 那是错的：它把 1 秒的时间保证改写成了「上限 / 吞吐」，而 1.4.2 记下的重复完成包形态
// 是**跨批次、相隔 10–15 ms**。该分支一旦把窗口压到毫秒级，重复包就会落在已释放的
// 内存上 —— 实测抓到的 `[Yuyuko] USE-AFTER-FREE ... process_one()` 就是它
// （约 12 次全套里出现 2 次）。
//
// 内存上界因此改由吞吐决定（稳态深度 ≈ 吞吐 × 1 s）。这是**安全**的失败模式：
// 最坏是多占几 MB，而不是踩到已释放的内存。
void quarantine_drain(std::chrono::steady_clock::time_point now) {
    uint64_t expired = 0;
    while (!quarantine.empty() &&
           now - quarantine.front().when >= std::chrono::milliseconds(quarantine_ms)) {
        // 归还之前不动它：~IocpRequest 会析构 Buffer 与 shared_ptr，其中可能进
        // CPython 拿 GIL。
        IOCP_REQ_FREE(quarantine.front().request);
        quarantine.pop_front();
        q_live.fetch_sub(1, std::memory_order_relaxed);
        ++expired;
    }
    q_scanned.fetch_add(expired, std::memory_order_relaxed);
    q_drains.fetch_add(1, std::memory_order_relaxed);
}

void quarantine_push(IocpRequest* request) {
    const auto now = std::chrono::steady_clock::now();
    quarantine.push_back({request, now});
    note_push();
    // 提交线程没有 worker 那种周期性唤醒，所以"到期"必须挂在 push 上，否则低吞吐下
    // 队首会一直不走。这里**不看积压量** —— 年龄是唯一条件。
    quarantine_drain(now);
}

DWORD quarantine_timeout() {
    if (quarantine.empty()) return INFINITE;
    const auto deadline = quarantine.front().when + std::chrono::milliseconds(quarantine_ms);
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (remaining < 1) remaining = 1;
    return static_cast<DWORD>(remaining + 1);
}

} // namespace

void release_request(IocpRequest* request) {
    // 每个请求恰好被收尾一次，谁先到谁负责。
    //
    // 完成包可能迟到、也可能重复，而完成包里**不带任何身份信息**（只有
    // OVERLAPPED* 和 completion key），所以"这个包属于哪一轮"在包到达时无法判断 ——
    // 唯一可靠的防线是时间隔离：请求内存只在这条路径上归还，且延迟 quarantine_ms。
    // 期间到达的陈旧包会看到仍然有效、但 state 已被改写的内存，于是它的 CAS 失败、
    // 安全走开，而不是按 IORequest 去解引用一块已经被 Python 字符串占用的内存
    // （实测崩溃现场 rcx = 0x006f006800740079，就是这种情况）。
    //
    // exchange 保证不会出现双重回收：那会把同一个地址两次压进 freelist，于是两个
    // 在飞请求共享一块内存。
    if (request->reaped.exchange(true, std::memory_order_acq_rel)) return;
    quarantine_push(request);
}

QuarantineStats quarantine_stats() {
    return {
        q_live.load(std::memory_order_relaxed),
        q_pushes.load(std::memory_order_relaxed),
        q_scanned.load(std::memory_order_relaxed),
        q_drains.load(std::memory_order_relaxed),
        q_peak.load(std::memory_order_relaxed),
    };
}

void IOCPContext::worker_proc(HANDLE iocp) {
    core::WorkerThreadAnchor thread_anchor;   // 锚定宿主线程状态（见 completion.hpp）
    auto batch_cap = core::config().iocp_batch_size();
    if (batch_cap < 1) batch_cap = 1;
    if (batch_cap > 256) batch_cap = 256;
    std::vector<OVERLAPPED_ENTRY> batch(batch_cap);

    while (running()) {
        DWORD timeout = INFINITE;
        if (auto* scheduler = core::completion_scheduler()) {
            timeout = scheduler->next_timeout_ms();
        }
        timeout = (std::min)(timeout, quarantine_timeout());
        ULONG count = 0;
        const BOOL ok = GetQueuedCompletionStatusEx(
            iocp, batch.data(), static_cast<ULONG>(batch.size()), &count, timeout, FALSE);
        if (count == 0) {
            quarantine_drain(std::chrono::steady_clock::now());
            if (auto* scheduler = core::completion_scheduler()) scheduler->flush_idle();
            if (!ok && !running()) break;
            continue;
        }

        for (ULONG index = 0; index < count; ++index) {
            const auto& entry = batch[index];
            if (!entry.lpOverlapped || entry.lpCompletionKey == 0) continue;
            auto* request = CONTAINING_RECORD(entry.lpOverlapped, IocpRequest, ov);
            process_one(
                static_cast<uint64_t>(entry.lpCompletionKey),
                request,
                entry.dwNumberOfBytesTransferred,
                entry.Internal == 0 ? 0 : static_cast<DWORD>(entry.Internal));
        }
        quarantine_drain(std::chrono::steady_clock::now());
        if (auto* scheduler = core::completion_scheduler()) scheduler->flush_idle();
    }
}

void IOCPContext::process_one(uint64_t id, IocpRequest* request, DWORD bytes, DWORD error) {
    // 这个完成包对应的是一次**尚未收尾**的请求，还是提交线程已经交付过的？
    // 用 state 的 CAS 做唯一裁决 —— state 本来就是 atomic，不需要额外标志。
    //
    //   CAS 成功  —— 该由这里交付结果
    //   CAS 失败  —— state 已被提交线程改成 RESOLVED/REJECTED，说明同步路径
    //                已经交付过了。这个包只是（可能是迟到/重复的）收尾信号：
    //                绝不能再交付一次，也不能立刻释放 —— 交给 release_request，
    //                它与提交线程那边的调用由 reaped 的 exchange 互斥，
    //                谁先谁负责，且都是延迟释放。
    // 陈旧包识别。完成包只携带 completion key（= 会话 id），本身不带身份；而这个
    // OVERLAPPED 可能已经属于**另一个会话**的新请求（地址被复用）。若照常往下走，
    // `get_session(id)` 会因为旧会话已关闭而返回空，于是把
    // "I/O operation completed after the file was closed"（Errno 995）投给这个
    // **好的新请求** —— 实测的偶发失败正是它（test_many_and_buffers 里那次写被凭空判死）。
    //
    // 两点刻意为之：
    //   1) **放在 CAS 之前**，且**只读不写**。这里面对的可能是已释放的内存，写一个字
    //      就可能污染堆（早先在此处 store(PENDING) 换来过 0xC0000005）。
    //   2) 不恢复 state。跨会话不匹配意味着这个请求属于别人，它真正的包还没来；
    //      我们什么都没改，它就仍能被正常交付。
    if (request->session_id != id) {
        g_packets_stale_session.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto expected = core::IOState::PENDING;
    if (!request->state.compare_exchange_strong(expected, core::IOState::RESOLVED)) {
        release_request(request);
        return;
    }
    if (!core::request_access_valid(request, __FILE__, __LINE__, __FUNCTION__)) return;

    auto session = get_session(id);
    if (!session) {
        request->operation->complete(core::Completion::failure(
            request->type,
            core::IoError::native(
                error ? error : ERROR_OPERATION_ABORTED,
                "I/O operation completed after the file was closed")));
        release_request(request);
        return;
    }

    session->complete_request();
    if (error == 0) {
        request->operation->complete(core::make_io_completion(request, bytes));
    } else {
        request->operation->complete(core::Completion::failure(
            request->type,
            core::IoError::native(error, "I/O operation failed")));
    }
    release_request(request);
}

} // namespace ayafileio::backend::windows

