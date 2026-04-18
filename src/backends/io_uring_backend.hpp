#pragma once
#ifdef HAVE_IO_URING

#include "../io_backend.hpp"
#include <liburing.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>

// ════════════════════════════════════════════════════════════════════════════
// §9  Linux io_uring Backend - 真异步，高性能
// ════════════════════════════════════════════════════════════════════════════

class IOUringBackend : public IOBackendBase {
public:
    IOUringBackend(const std::string& path, const std::string& mode);
    ~IOUringBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    void close_impl() override;

    void complete_ok(IORequest* req, size_t bytes) override;
    void complete_error(IORequest* req, DWORD err) override;

private:
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::atomic<long> m_pending{0};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    LoopHandle* m_loop_handle = nullptr;
    
    // io_uring 相关
    struct io_uring m_ring{};
    std::thread m_reaper_thread;
    std::atomic<bool> m_reaper_stop{false};
    
    void reaper_loop();
    void submit_io(IORequest* req, int op, int fd, const void* buf, size_t len, off_t offset);
    bool setup_uring();
    void teardown_uring();
    
    IORequest* make_req(size_t size, PyObject* future, ReqType type) override;
    void complete_error_inline(IORequest* req, DWORD err) override;
    
    // 缓存的 io_uring 配置（避免频繁加锁）
    unsigned m_cached_io_uring_queue_depth = 256;
    unsigned m_cached_io_uring_flags = 0;
    bool m_cached_io_uring_sqpoll = false;
};

#endif // HAVE_IO_URING