// macos_gcd_backend.hpp
#pragma once
#ifdef __APPLE__

#include "../io_backend.hpp"
#include "../global_thread_pool.hpp"
#include "utils/debug_log.hpp"
#include <dispatch/dispatch.h>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

namespace ayafileio {

class MacOSGCDBackend : public IOBackendBase {
public:
    MacOSGCDBackend(const std::string& path, const std::string& mode);
    MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~MacOSGCDBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* read_at(int64_t offset, int64_t size) override;
    PyObject* write(Py_buffer* view, int64_t position = -1) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override { return m_fd; }
    void close_impl() override;

private:
    dispatch_io_t m_channel = nullptr;
    dispatch_queue_t m_queue = nullptr;
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    uint64_t m_cachedFileSize = 0;  // open 时缓存，写/截断路径乐观更新
    bool m_appendMode = false;
    std::string m_path;
    
    // Python 事件循环集成
    bool m_loop_initialized = false;
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;

    // 大请求线程快速路的 worker 数（0 = 未计算）
    unsigned m_num_workers = 0;

    void ensure_loop_initialized();

    // ── 大请求线程快速路 ─────────────────────────────────────────────────
    // dispatch_io 的 high_water 会把大请求切成 64KB 小块逐块回调（4MB =
    // 64 次回调 + 64 次 memcpy），大块顺序 I/O 被严重拖慢。因此按大小
    // 双模调度：>= LARGE_IO_THRESHOLD 的请求绕过 dispatch_io，直接在线程
    // 池里一发 pread/pwrite 进最终缓冲（读进 preResult PyBytes、写读持有
    // 的用户视图，双向零拷贝）；小请求维持 dispatch_io 低延迟路径。
    static constexpr size_t LARGE_IO_THRESHOLD = 256 * 1024;
    void submit_read_fast(IORequest* req, uint64_t offset, size_t size);
    void submit_write_fast(IORequest* req, uint64_t offset, size_t size);

    // ── 大读并行分块填充 ─────────────────────────────────────────────
    // ≥ PARALLEL_READ_THRESHOLD 的读拆成 PARALLEL_READ_CHUNK 大小的块，
    // 一次性全部提交全局线程池，各 worker 并发 pread 进同一预建缓冲
    // （位置写，零协调），最后一块完成时聚合收尾。单线程顺序读的页缓存
    // memcpy 是瓶颈（实测 64 MiB：单线程 ~8.9ms）；摊到多个 worker 后
    // 逼近内存带宽。turbofile 同款手法（其 64 MiB 并行填充 2.7ms）。
    // 任一块失败即整请求失败（与流水线路径一致：部分数据不可交付）。
    static constexpr size_t PARALLEL_READ_THRESHOLD = 1024 * 1024;
    static constexpr size_t PARALLEL_READ_CHUNK = 512 * 1024;
    void submit_read_parallel(IORequest* req, uint64_t offset, size_t size);

    // ── 小读 mincore 内联快车道 ─────────────────────────────────────────
    // dispatch_io 每 op 都要过队列交付（handler hop + 唤醒），缓存命中的
    // 小读改为内联完成：open 时 mmap 一份只读映射，读前 mincore 判驻留，
    // 全命中就在调用线程（持 GIL）直接 pread 进预建 PyBytes 并以
    // complete_inline 即刻 resolve——await 不让出事件循环。这是 macOS 侧
    // 对应 Windows FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 与 io_uring
    // COOP_TASKRUN 内联收割的快车道，补上三平台热路径的最后一块。
    //
    // 调用方须已完成 m_pending++（与异步路径同一记账）；返回 true 表示
    // future 已 resolve（调用方直接返回），false 表示落回 dispatch_io。
    // 注意：一旦通过驻留检查就必须在本方法内完成（含错误路径），不可
    // 落回，否则同一 future 将被双投递。
    //
    // 映射快照于 open 时的文件大小且只用于 mincore 探测（从不解引用），
    // 因此文件后续增长的区域不走快车道（范围检查落回），也不会因映射
    // 越界产生 SIGBUS。收缩/增长由 m_cachedFileSize 与范围检查共同保证。
    const void* m_mapBase = nullptr;
    size_t      m_mapSize = 0;
    bool try_inline_read(IORequest* req, uint64_t offset, size_t size);
};

} // namespace ayafileio

#endif // __APPLE__