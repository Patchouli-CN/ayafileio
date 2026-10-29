#pragma once

#include "../../core/io_backend.hpp"
#include "iocp_request.hpp"
#include "iocp_session.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ayafileio::backend::windows {

struct QuarantineStats {
    uint64_t live{0};
    uint64_t pushes{0};
    uint64_t scanned{0};
    uint64_t drains{0};
    uint64_t peak{0};
};

class IOCPContext {
public:
    static IOCPContext& instance();

    void init();
    void shutdown();

    uint64_t create_session(
        HANDLE handle,
        bool append_mode,
        DWORD access,
        bool owns_fd);
    std::shared_ptr<Session> get_session(uint64_t id) const;
    void remove_session(uint64_t id);

    void submit_read(uint64_t id, int64_t size, core::OperationPtr operation);
    void submit_read_at(uint64_t id, int64_t offset, int64_t size, core::OperationPtr operation);
    void submit_write(uint64_t id, core::Buffer view, int64_t position, core::OperationPtr operation);
    void submit_seek(uint64_t id, int64_t offset, int whence, core::OperationPtr operation);
    void submit_flush(uint64_t id, core::OperationPtr operation);
    void submit_close(uint64_t id, core::OperationPtr operation);
    void submit_tell(uint64_t id, core::OperationPtr operation);
    void submit_truncate(uint64_t id, int64_t size, core::OperationPtr operation);
    void submit_readinto(uint64_t id, core::Buffer buffer, core::OperationPtr operation);

    void trigger_ctrlc() { ctrlc_.store(true, std::memory_order_relaxed); }
    bool is_ctrlc_triggered() const { return ctrlc_.load(std::memory_order_relaxed); }
    void close_all_sessions();

    HANDLE read_iocp() const { return read_iocp_; }
    HANDLE write_iocp() const { return write_iocp_; }
    HANDLE iocp_for_session(const std::shared_ptr<Session>& session) const;
    bool running() const { return running_.load(std::memory_order_acquire); }

    void worker_proc(HANDLE iocp);
    void process_one(uint64_t id, IocpRequest* request, DWORD bytes, DWORD error);

private:
    IOCPContext() = default;
    ~IOCPContext();

    HANDLE read_iocp_{nullptr};
    HANDLE write_iocp_{nullptr};
    std::atomic<bool> running_{false};
    std::thread read_worker_;
    std::thread write_worker_;

    mutable std::shared_mutex sessions_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;
    std::atomic<uint64_t> next_session_id_{1};
    std::atomic<bool> ctrlc_{false};
};

QuarantineStats quarantine_stats();

// 请求收尾：每个请求恰好被释放一次，且一律延迟 quarantine_ms 之后再归还内存。
// 迟到的完成包因此在内存仍有效的窗口内到达，靠 state/reaped 两步判断安全走开。
void release_request(IocpRequest* request);

} // namespace ayafileio::backend::windows
