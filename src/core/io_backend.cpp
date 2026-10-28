#include "io_backend.hpp"

#include <utility>

namespace ayafileio::core {

void IOBackendBase::complete_ok(IORequest* request, std::size_t bytes) {
    m_pending.fetch_sub(1, std::memory_order_release);
    m_close_wake.release();
    request->operation->complete(make_io_completion(request, bytes));
    REQ_FREE(request);
}

void IOBackendBase::complete_error(IORequest* request, IoError error) {
    m_pending.fetch_sub(1, std::memory_order_release);
    m_close_wake.release();
    request->operation->complete(Completion::failure(request->type, std::move(error)));
    REQ_FREE(request);
}

void IOBackendBase::complete_inline(IORequest* request, int64_t result) {
    m_pending.fetch_sub(1, std::memory_order_release);
    m_close_wake.release();
    if (result >= 0) {
        request->operation->complete_inline(
            make_io_completion(request, static_cast<std::size_t>(result)));
    } else {
        request->operation->complete_inline(
            Completion::failure(request->type, IoError::native(
                static_cast<uint32_t>(-result), "I/O operation failed")));
    }
    REQ_FREE(request);
}

void IOBackendBase::complete_error_inline(IORequest* request, IoError error) {
    m_pending.fetch_sub(1, std::memory_order_release);
    m_close_wake.release();
    request->operation->complete_inline(
        Completion::failure(request->type, std::move(error)));
    REQ_FREE(request);
}

IORequest* IOBackendBase::make_shell(OperationPtr operation, ReqType type) {
    auto* request = REQ_ALLOC();
    request->file = this;
    request->operation = std::move(operation);
    request->type = type;
    request->operation->submitted();
    return request;
}

IORequest* IOBackendBase::make_req(std::size_t size, OperationPtr operation, ReqType type) {
    Buffer buffer = size <= m_cached_buffer_size ? Buffer::pool(size) : Buffer::heap(size);
    auto* request = make_shell(std::move(operation), type);
    request->reqSize = size;
    request->buffer = std::move(buffer);
    return request;
}

IORequest* IOBackendBase::make_req_read_bytes(std::size_t size, OperationPtr operation) {
    Buffer buffer = operation->allocate_read_buffer(size);
    if (!buffer) throw IoException(IoError::memory("Failed to allocate read result"));
    auto* request = make_shell(std::move(operation), ReqType::Read);
    request->reqSize = size;
    request->buffer = std::move(buffer);
    return request;
}

IORequest* IOBackendBase::make_req_held_write(Buffer view, OperationPtr operation) {
    const std::size_t size = view.size();
    auto* request = make_shell(std::move(operation), ReqType::Write);
    request->reqSize = size;
    request->holdsWriteBuf = true;
    request->buffer = std::move(view);
    return request;
}

IORequest* IOBackendBase::make_req_readinto(Buffer view, std::size_t size, OperationPtr operation) {
    auto* request = make_shell(std::move(operation), ReqType::Read);
    request->reqSize = size;
    request->isReadinto = true;
    request->buffer = std::move(view);
    return request;
}

} // namespace ayafileio::core
