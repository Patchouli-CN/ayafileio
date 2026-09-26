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
};

} // namespace ayafileio

#endif // __APPLE__