#pragma once
#ifdef HAVE_IO_URING

#include "../io_backend.hpp"
#include "../debug_log.hpp"
#include "uring_pool.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

class IOUringBackend : public IOBackendBase {
public:
    IOUringBackend(const std::string& path, const std::string& mode);
    IOUringBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~IOUringBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* write(Py_buffer* view) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    void close_impl() override;

    void complete_ok(IORequest* req, size_t bytes) override;
    void complete_error(IORequest* req, DWORD err) override;

    // reaper 循环入口（由池调用）
    static void reaper_loop_entry(UringInstance* inst);

private:
    int m_fd = -1;
    std::atomic<bool> m_running{false};
    std::atomic<long> m_pending{0};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    bool m_appendMode = false;
    
    // 事件循环相关
    bool m_loop_initialized = false;
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;
    LoopHandle* m_loop_handle = nullptr;
    
    // io_uring 实例（从池中获取）
    std::shared_ptr<UringInstance> m_uring;
    
    void ensure_loop_initialized();
    void submit_io(IORequest* req, int op, int fd, const void* buf, size_t len, off_t offset);
    
    IORequest* make_req(size_t size, PyObject* future, ReqType type) override;
    void complete_error_inline(IORequest* req, DWORD err) override;
    
    // 缓存的配置值
    size_t m_cached_buffer_size = 65536;
    size_t m_cached_buffer_pool_max = 512;
    unsigned m_cached_close_timeout_ms = 4000;
};

#endif // HAVE_IO_URING