#include "iocp_context.hpp"

#include "../../core/config.hpp"
#include "../../core/file_lifecycle.hpp"
#include "../../core/global_thread_pool.hpp"

#include <utility>

namespace ayafileio::backend::windows {
namespace {

IocpRequest* make_shell(core::OperationPtr operation, core::ReqType type) {
    auto* request = IOCP_REQ_ALLOC();
    request->operation = std::move(operation);
    request->type = type;
    request->operation->submitted();
    return request;
}

IocpRequest* make_read_request(std::size_t size, core::OperationPtr operation) {
    auto buffer = operation->allocate_read_buffer(size);
    if (!buffer) throw core::IoException(core::IoError::memory("Failed to allocate read result"));
    auto* request = make_shell(std::move(operation), core::ReqType::Read);
    request->reqSize = size;
    request->buffer = std::move(buffer);
    return request;
}

IocpRequest* make_write_request(core::Buffer view, core::OperationPtr operation) {
    const auto size = view.size();
    auto* request = make_shell(std::move(operation), core::ReqType::Write);
    request->reqSize = size;
    request->holdsWriteBuf = true;
    request->buffer = std::move(view);
    return request;
}

IocpRequest* make_readinto_request(core::Buffer view, std::size_t size, core::OperationPtr operation) {
    auto* request = make_shell(std::move(operation), core::ReqType::Read);
    request->reqSize = size;
    request->isReadinto = true;
    request->buffer = std::move(view);
    return request;
}

core::Completion failure(core::ReqType type, DWORD error, const char* message) {
    return core::Completion::failure(
        type, core::IoError::native(static_cast<uint32_t>(error), message));
}

bool unavailable(
    const std::shared_ptr<Session>& session,
    const core::OperationPtr& operation,
    core::ReqType type) {
    if (!session) {
        operation->complete_inline(core::Completion::failure(
            type, core::IoError::value("File not open.")));
        return true;
    }
    if (!session->running.load(std::memory_order_relaxed) ||
        session->handle == INVALID_HANDLE_VALUE) {
        operation->complete_inline(core::Completion::failure(
            type, core::IoError::value("I/O operation on closed file.")));
        return true;
    }
    if (IOCPContext::instance().is_ctrlc_triggered()) {
        operation->complete_inline(core::Completion::failure(
            type, core::IoError::interrupted("interrupted")));
        return true;
    }
    return false;
}

void start_read(
    const std::shared_ptr<Session>& session,
    const core::ReadPlan& plan,
    core::OperationPtr operation,
    core::Buffer user_buffer,
    bool readinto) {
    auto* request = readinto
        ? make_readinto_request(std::move(user_buffer), plan.size, operation)
        : make_read_request(plan.size, operation);
    request->ov.Offset = static_cast<DWORD>(plan.offset & 0xFFFFFFFFull);
    request->ov.OffsetHigh = static_cast<DWORD>(plan.offset >> 32);

    session->pending.fetch_add(1, std::memory_order_relaxed);
    DWORD got = 0;
    const auto ok = ReadFile(
        session->handle,
        request->buf(),
        static_cast<DWORD>(plan.size),
        &got,
        &request->ov);
    if (ok) {
        if (static_cast<std::size_t>(got) < plan.size) {
            refresh_session_size_after_short_io(*session);
        }
        // 同步成功有两条完全不同的收尾路径，**只能走其中一条**：
        //
        //   skipCPOnSuccess == true  —— FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 生效，
        //       内核承诺不为这次成功投递完成包。那就只能在这里交付、在这里释放。
        //
        //   skipCPOnSuccess == false —— 内核仍会为这个 OVERLAPPED 补投一个完成包。
        //       交付与释放都交给 process_one，与异步路径共用同一套收尾逻辑。
        //
        // 早先的实现是两条都交付，再用一个非原子 bool（inline_completed）告诉 worker
        // "提交线程已经交付过了"。那既是数据竞争（提交线程写、worker 读，无
        // release/acquire），也是真实的双重交付：worker 若读到 false，就会再调一次
        // make_io_completion —— 而它已经把 request->buffer 搬走了，第二次只能拿到空
        // buffer，回退成 PyBytes_FromStringAndSize(nullptr, n) 造一块未初始化的
        // bytes，再对已完成的 Future set_result 一次。
        //
        // 现在不需要那个标志：谁负责收尾由内核模式唯一决定，不必跨线程协商。
        if (session->skipCPOnSuccess) {
            session->complete_request();
            request->operation->complete_inline(core::make_io_completion(request, got));
            IOCP_REQ_FREE(request);
        }
        return;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) return;
    session->complete_request();
    request->state.store(core::IOState::REJECTED, std::memory_order_relaxed);
    request->operation->complete_inline(failure(request->type, error, "ReadFile failed"));
    IOCP_REQ_FREE(request);
}

void start_write(
    const std::shared_ptr<Session>& session,
    core::Buffer view,
    uint64_t offset,
    core::OperationPtr operation) {
    const auto size = view.size();
    auto* request = make_write_request(std::move(view), operation);
    request->ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    request->ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    session->pending.fetch_add(1, std::memory_order_relaxed);
    DWORD wrote = 0;
    const auto ok = WriteFile(
        session->handle,
        request->buf(),
        static_cast<DWORD>(size),
        &wrote,
        &request->ov);
    if (ok) {
        if (static_cast<std::size_t>(wrote) != size) {
            refresh_session_size_after_short_io(*session);
        }
        // 同 start_read：有完成包保证时（!skipCPOnSuccess）一律交给 process_one 收尾，
        // 提交线程只负责那條"内核承诺不投包"的分支。
        if (session->skipCPOnSuccess) {
            session->complete_request();
            request->operation->complete_inline(core::make_io_completion(request, wrote));
            IOCP_REQ_FREE(request);
        }
        return;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) return;
    refresh_session_size_after_short_io(*session);
    session->complete_request();
    request->state.store(core::IOState::REJECTED, std::memory_order_relaxed);
    request->operation->complete_inline(failure(request->type, error, "WriteFile failed"));
    IOCP_REQ_FREE(request);
}

} // namespace

void IOCPContext::submit_read(uint64_t id, int64_t size, core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Read)) return;
    const auto plan = session->state.plan_read(size, [&] {
        return refresh_session_size(*session);
    });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    start_read(session, plan, std::move(operation), core::Buffer{}, false);
}

void IOCPContext::submit_read_at(
    uint64_t id,
    int64_t offset,
    int64_t size,
    core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Read)) return;
    if (offset < 0) {
        operation->complete_inline(core::Completion::failure(
            core::ReqType::Read, core::IoError::value("negative offset not allowed")));
        return;
    }
    const auto plan = session->state.plan_read_at(offset, size, [&] {
        return refresh_session_size(*session);
    });
    if (plan.eof) {
        operation->complete_inline(core::Completion::read(core::Buffer{}, 0));
        return;
    }
    start_read(session, plan, std::move(operation), core::Buffer{}, false);
}

void IOCPContext::submit_readinto(
    uint64_t id,
    core::Buffer buffer,
    core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Read)) return;
    if (buffer.size() == 0) {
        auto result = core::Completion::success(core::ReqType::Read, 0);
        result.return_count = true;
        operation->complete_inline(std::move(result));
        return;
    }
    const auto plan = session->state.plan_read(static_cast<int64_t>(buffer.size()), [&] {
        return refresh_session_size(*session);
    });
    if (plan.eof) {
        auto result = core::Completion::success(core::ReqType::Read, 0);
        result.return_count = true;
        operation->complete_inline(std::move(result));
        return;
    }
    start_read(session, plan, std::move(operation), std::move(buffer), true);
}

void IOCPContext::submit_write(
    uint64_t id,
    core::Buffer view,
    int64_t position,
    core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Write)) return;
    if (position >= 0 && session->appendMode) {
        throw core::IoException(core::IoError::value("write_at() does not support append mode"));
    }
    if (view.size() > UINT32_MAX) {
        throw core::IoException(core::IoError::overflow("write buffer exceeds 4 GiB - 1"));
    }
    if (view.size() == 0) {
        operation->complete_inline(core::Completion::success(core::ReqType::Write, 0));
        return;
    }
    const auto offset = session->state.reserve_write(position, view.size());
    start_write(session, std::move(view), offset, std::move(operation));
}

void IOCPContext::submit_seek(
    uint64_t id,
    int64_t offset,
    int whence,
    core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Other)) return;
    operation->complete_inline(core::seek_completion(
        session->state, offset, whence, [&] { return refresh_session_size(*session); }));
}

void IOCPContext::submit_tell(uint64_t id, core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Other)) return;
    operation->complete_inline(core::tell_completion(session->state));
}

void IOCPContext::submit_truncate(
    uint64_t id,
    int64_t size,
    core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Other)) return;
    if (auto rejected = core::truncate_guard(size)) {
        operation->complete_inline(std::move(*rejected));
        return;
    }

    LARGE_INTEGER previous{};
    if (!SetFilePointerEx(session->handle, {0}, &previous, FILE_CURRENT)) {
        const auto error = GetLastError();
        operation->complete_inline(failure(core::ReqType::Other, error, "SetFilePointerEx failed"));
        return;
    }
    LARGE_INTEGER target{};
    target.QuadPart = size;
    if (!SetFilePointerEx(session->handle, target, nullptr, FILE_BEGIN)) {
        const auto error = GetLastError();
        SetFilePointerEx(session->handle, previous, nullptr, FILE_BEGIN);
        operation->complete_inline(failure(core::ReqType::Other, error, "SetFilePointerEx failed"));
        return;
    }
    if (!SetEndOfFile(session->handle)) {
        const auto error = GetLastError();
        SetFilePointerEx(session->handle, previous, nullptr, FILE_BEGIN);
        operation->complete_inline(failure(core::ReqType::Other, error, "SetEndOfFile failed"));
        return;
    }
    SetFilePointerEx(session->handle, previous, nullptr, FILE_BEGIN);
    operation->complete_inline(core::truncate_done(session->state, size));
}

void IOCPContext::submit_flush(uint64_t id, core::OperationPtr operation) {
    auto session = get_session(id);
    if (unavailable(session, operation, core::ReqType::Other)) return;

    operation->submitted();
    session->pending.fetch_add(1, std::memory_order_relaxed);
    const HANDLE handle = session->handle;
    auto& pool = core::GlobalThreadPool::instance();
    pool.ensure_started(core::effective_io_worker_count());
    pool.enqueue([session, handle, operation]() mutable {
        const BOOL ok = FlushFileBuffers(handle);
        const DWORD error = ok ? 0 : GetLastError();
        session->complete_request();
        if (ok) {
            operation->complete(core::Completion::success(core::ReqType::Other));
        } else {
            operation->complete(failure(core::ReqType::Other, error, "FlushFileBuffers failed"));
        }
    });
}

void IOCPContext::submit_close(uint64_t id, core::OperationPtr operation) {
    auto session = get_session(id);
    if (!session) {
        operation->complete_inline(core::Completion::success(core::ReqType::Other));
        return;
    }

    bool expected = true;
    if (!session->running.compare_exchange_strong(expected, false)) {
        operation->complete_inline(core::Completion::success(core::ReqType::Other));
        return;
    }

    if (session->handle != INVALID_HANDLE_VALUE) {
        if (session->pending.load(std::memory_order_acquire) > 0) {
            CancelIoEx(session->handle, nullptr);
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(session->cached_close_timeout_ms);
            while (session->pending.load(std::memory_order_acquire) > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                if (remaining.count() <= 0) break;
                session->close_wake.try_acquire_for(remaining);
            }
        }
        if (session->owns_fd) CloseHandle(session->handle);
        session->handle = INVALID_HANDLE_VALUE;
    }
    remove_session(id);
    operation->complete_inline(core::Completion::success(core::ReqType::Other));
}

} // namespace ayafileio::backend::windows
