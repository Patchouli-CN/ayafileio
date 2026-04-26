#pragma once
#ifdef __APPLE__

#include "../io_backend.hpp"
#include "../debug_log.hpp"
#include <dispatch/dispatch.h>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

class MacOSGCDBackend : public IOBackendBase {
public:
    MacOSGCDBackend(const std::string& path, const std::string& mode);
    MacOSGCDBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~MacOSGCDBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    void close_impl() override;

    void complete_ok(IORequest* req, size_t bytes) override;
    void complete_error(IORequest* req, DWORD err) override;

private:
    dispatch_io_t m_channel = nullptr;
    dispatch_queue_t m_queue = nullptr;
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::atomic<long> m_pending{0};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    std::string m_path;
    
    // Python 事件循环集成
    bool m_loop_initialized = false;
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    LoopHandle* m_loop_handle = nullptr;
    
    void ensure_loop_initialized();
    IORequest* make_req(size_t size, PyObject* future, ReqType type) override;
    void complete_error_inline(IORequest* req, DWORD err) override;
    
    // 缓存的配置值
    size_t m_cached_buffer_size = 65536;
    size_t m_cached_buffer_pool_max = 512;
    unsigned m_cached_close_timeout_ms = 4000;
    
    // 用于 seek/flush 的同步
    std::mutex m_barrierMtx;
    std::condition_variable m_barrierCv;
};

#endif // __APPLE__