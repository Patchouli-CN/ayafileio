#pragma once
#include "globals.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace ayafileio {

// ─────────────────────────────────────────────────────────────────────────────
// BatchEntry — one pending future result
// ─────────────────────────────────────────────────────────────────────────────
struct BatchEntry {
    PyObject *set_fn;  // owned (future.set_result or future.set_exception)
    PyObject *val;     // owned (result value or exception object)
};

// ─────────────────────────────────────────────────────────────────────────────
// ResultBatcher — adaptive dual-trigger completion batching
//
//   1. Count threshold: flush when batch.size() >= m_threshold (adaptive)
//   2. Idle timeout:    flush when oldest entry older than idle_timeout_ms
//
// Adaptive mode (enabled by default):
//   - Tracks inter-completion intervals via a 128-entry ring buffer
//   - Dynamically adjusts the effective threshold based on the
//     rolling median interval between completed I/O operations:
//
//       threshold = target_latency_us / median_interval_us
//
//   - Fast disk (NVMe, ~10μs intervals): threshold rises → batch more, fewer
//     call_soon_threadsafe calls, lower GIL pressure
//   - Slow disk (HDD, ~10ms intervals): threshold drops → flush sooner,
//     no wasted waiting for completions that haven't arrived yet
//
//   The idle timeout also adapts: min(5ms, median_interval * 4) for fast disks,
//   capped at the configured idle_timeout_ms for slow disks.
//
// Thread safety: push() and flush() must be called with GIL held.
// has_pending(), idle_expired(), get_timeout_ms() are lock-free safe.
// ─────────────────────────────────────────────────────────────────────────────
class ResultBatcher {
public:
    ResultBatcher(PyObject *loop, size_t threshold = 64,
                  unsigned idle_timeout_ms = 5);
    ~ResultBatcher();

    // ── adaptive tuning ────────────────────────────────────────────────────
    void set_adaptive(bool enabled)          { m_adaptive.store(enabled, std::memory_order_relaxed); }
    bool adaptive() const                    { return m_adaptive.load(std::memory_order_relaxed); }
    void set_target_latency_us(unsigned us)  { m_target_latency_us.store(us, std::memory_order_relaxed); }
    unsigned target_latency_us() const       { return m_target_latency_us.load(std::memory_order_relaxed); }
    // 运行期更新上界（configure() 热更新用）。写者取 m_mtx，因为
    // m_threshold_max / m_idle_timeout_max_ms 也在 m_mtx 下被 push() 与
    // update_adaptive_locked() 读取；current_* 立即夹到新上界，避免
    // 「调小之后旧的更大值还生效」。
    void set_threshold_max(size_t value);
    size_t threshold_max() const { return m_threshold_max.load(std::memory_order_relaxed); }
    void set_idle_timeout_max_ms(unsigned value);
    unsigned idle_timeout_max_ms() const { return m_idle_timeout_max_ms.load(std::memory_order_relaxed); }

    // ── data path (GIL required) ──────────────────────────────────────────
    bool push(PyObject *set_fn, PyObject *val);
    void flush();
    void flush_if_idle();   // 单次持锁完成 判空+判超时+置位，等价于
                            // has_pending() && idle_expired() ? flush() : no-op

    // ── queries for worker thread (no GIL, thread-safe) ───────────────────
    bool has_pending() const;
    bool idle_expired() const;
    DWORD get_timeout_ms() const;

    // ── in-flight op tracking (IOCP path) ─────────────────────────────────
    // 提交时 +1、完成处理时 -1。最后一个在飞 op 完成（计数归 0）说明
    // 短期内不会再有新完成到达，调用方应立即 flush —— 串行/小并发负载
    // 不再为空闲超时买单，每-op 延迟从 ~idle_timeout_ms 降到事件循环
    // 唤醒级别。⚠ 计数失衡（任何只加不减的路径）会永久关闭该优化：
    // m_outstanding 永不归零后，异步完成的 op 全部退化为等空闲清扫
    // （实测单 op 延迟从亚毫秒涨到 ~7ms，整体掉两个数量级）。
    // 新增 submit 路径务必配平；`debug_batcher_outstanding()` +
    // tests/test_batcher_accounting.py 守着这条不变量。
    void op_submitted() { m_outstanding.fetch_add(1, std::memory_order_relaxed); }
    bool op_completed() { return m_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1; }

    // ── stats (for debugging / monitoring) ─────────────────────────────────
    size_t current_threshold() const { return m_current_threshold.load(std::memory_order_relaxed); }
    unsigned current_idle_ms()  const { return m_current_idle_ms.load(std::memory_order_relaxed); }
    // 在飞 op 记账（op_submitted/op_completed 之差）。回归测试用它断言
    // 同步完成路径没有记账泄漏
    long outstanding() const { return m_outstanding.load(std::memory_order_relaxed); }
    double median_interval_us() const;

    // 弱引用是否仍指向 *loop（新事件循环对象复用了旧 loop 的地址时为假）。
    // batcher 按 loop 缓存，缓存键是裸 PyObject*，所以这个判断是必需的：
    // 不比较就会把旧 loop 的 batcher 交给新 loop，而它的弱引用已经失效，
    // 于是新 loop 的所有完成结果都会被静默丢掉。
    bool loop_is(PyObject *loop) const;

private:
    // 事件循环的**弱**引用。刻意不缓存 `loop.call_soon_threadsafe` 这个绑定
    // 方法：那是强引用，会让 IOCPContext::m_batchers 把任何做过一次 I/O 的
    // loop 钉到进程退出（每 asyncio.run() 一次就永久留下一个 loop 对象）。
    // 代价是每次 flush 一次属性查找 —— flush 是批量投递，不在每个 op 上。
    PyObject *m_loop_ref    = nullptr;
    PyObject *m_drain_cb    = nullptr;

    mutable std::mutex m_mtx;
    std::vector<BatchEntry> m_batch;
    std::atomic<bool> m_dispatch_pending{false};
    std::chrono::steady_clock::time_point m_first_push;  // guarded by m_mtx
    std::chrono::steady_clock::time_point m_last_push;   // guarded by m_mtx
    bool m_has_pending{false};                            // guarded by m_mtx

    // ── fixed config ──────────────────────────────────────────────────────
    // 原子是因为 configure() 可以在任意线程改它们，而 push() /
    // update_adaptive_locked() 只持 m_mtx 读取，不加锁读会构成数据竞争。
    std::atomic<size_t>   m_threshold_max;        // upper bound from config (e.g., 256)
    std::atomic<unsigned> m_idle_timeout_max_ms;  // upper bound from config (e.g., 5ms)

    // ── adaptive state ────────────────────────────────────────────────────
    std::atomic<bool>     m_adaptive{true};
    std::atomic<unsigned> m_target_latency_us{1000};  // target max extra latency
    std::atomic<size_t>   m_current_threshold{64};
    std::atomic<unsigned> m_current_idle_ms{5};
    std::atomic<long>     m_outstanding{0};  // 本 loop 在飞 IOCP 请求数

    static constexpr size_t RING_SIZE = 128;
    static constexpr size_t ADAPTIVE_UPDATE_INTERVAL = 16;  // 每 N 次 push 重算一次中位数
    std::array<uint64_t, RING_SIZE> m_intervals_us{};  // ring buffer, guarded by m_mtx
    size_t   m_ring_pos{0};
    size_t   m_ring_count{0};
    size_t   m_pushes_since_update{0};                 // guarded by m_mtx

    // ── internal ──────────────────────────────────────────────────────────
    void update_adaptive_locked();
    void schedule_drain();  // call_soon_threadsafe(m_drain_cb)，锁外调用
};

// ── Global batcher registry (one per event loop, for non-IOCP backends) ─────
ResultBatcher *get_or_create_batcher(PyObject *loop);
void clear_batchers();

// 把当前 config() 重新应用到所有已存在的 batcher。
// batcher 在创建时缓存 iocp_batch_size / adaptive_batch /
// adaptive_target_latency_us，没有这一步的话，进程里跑过第一个异步操作之后
// configure() 对这些键就是静默无效的（只有新建的 event loop 才会拿到新值）。
// 只改原子量，不做任何 Python 调用，因此调用方持有 GIL 也无死锁风险。
void refresh_batchers_config();

} // namespace ayafileio
