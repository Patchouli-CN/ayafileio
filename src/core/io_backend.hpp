#pragma once

#include "io_request.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>

namespace ayafileio::core {

using OperationPtr = std::shared_ptr<Operation>;

// 读 / 写 / readinto 的成功结果构造。
//
// 这段逻辑有三个副本（IOBackendBase::complete_ok、iocp_submit.cpp、iocp_worker.cpp），
// 而它并不平凡：read 要把 Buffer 的所有权从 request 搬进 Completion（零拷贝交给
// Python），readinto 要改成回传计数而不是数据，write 只回计数。任何一个副本写错，
// 表现都是"数据看起来对但生命周期错了"，或者"偶尔拿不到 buffer"。
//
// 所以只留一份——而且刻意留在头文件里内联：它在每次 I/O 完成的路径上，跨 TU
// 调用会白丢一次内联机会。去重不该以热路径为代价。
inline Completion make_io_completion(IORequest* request, std::size_t bytes) {
    Completion result = Completion::success(request->type, bytes);
    result.return_count = request->isReadinto;
    if (request->type == ReqType::Read && !request->isReadinto) {
        result.buffer = std::move(request->buffer);
    }
    return result;
}

class IOBackendBase {
public:
    IOBackendBase() = default;
    virtual ~IOBackendBase() = default;

    virtual void read(int64_t size, OperationPtr operation) = 0;
    virtual void read_at(int64_t offset, int64_t size, OperationPtr operation) = 0;
    virtual void write(Buffer view, int64_t position, OperationPtr operation) = 0;
    virtual void seek(int64_t offset, int whence, OperationPtr operation) = 0;
    virtual void flush(OperationPtr operation) = 0;
    virtual void close(OperationPtr operation) = 0;
    virtual void tell(OperationPtr operation) = 0;
    virtual void truncate(int64_t size, OperationPtr operation) = 0;
    virtual void readinto(Buffer buffer, OperationPtr operation) = 0;
    virtual int fileno() const = 0;
    virtual void close_impl() = 0;

    virtual void complete_ok(IORequest* request, std::size_t bytes);
    virtual void complete_error(IORequest* request, IoError error);

protected:
    virtual IORequest* make_req(std::size_t size, OperationPtr operation, ReqType type);
    virtual IORequest* make_req_read_bytes(std::size_t size, OperationPtr operation);
    virtual IORequest* make_req_held_write(Buffer view, OperationPtr operation);
    virtual IORequest* make_req_readinto(Buffer view, std::size_t size, OperationPtr operation);
    virtual void complete_error_inline(IORequest* request, IoError error);
    virtual void complete_inline(IORequest* request, int64_t result);

    IORequest* make_shell(OperationPtr operation, ReqType type);

    std::atomic<long> m_pending{0};
    std::counting_semaphore<INT32_MAX> m_close_wake{0};

    std::size_t m_cached_buffer_size{65536};
    std::size_t m_cached_buffer_pool_max{512};
    unsigned m_cached_close_timeout_ms{4000};
    unsigned m_cached_io_uring_queue_depth{256};
    unsigned m_cached_io_uring_flags{0};
    bool m_cached_io_uring_sqpoll{false};
    bool m_owns_fd{true};
};

} // namespace ayafileio::core
