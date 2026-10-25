#ifdef HAVE_IO_URING

#include "native_open.hpp"
#include "io_uring_backend.hpp"
#include "../../core/config.hpp"
#include "../../core/file_mode.hpp"

#include <atomic>
#include <cstring>
#include <fcntl.h>

namespace ayafileio::backend::linux {
namespace {
std::atomic<uint64_t> native_aopen_submissions{0};
}

void complete_native_aopen(core::IORequest* request, int result, bool inline_completion) {
    core::Completion completion = result >= 0
        ? core::Completion::success(core::ReqType::Open, 0, result)
        : core::Completion::failure(
            core::ReqType::Open,
            core::IoError::native(static_cast<uint32_t>(-result), "Failed to open file"));
    if (inline_completion) request->operation->complete_inline(std::move(completion));
    else request->operation->complete(std::move(completion));
    REQ_FREE(request);
}

bool submit_native_aopen(
    const std::string& path,
    const std::string& mode,
    core::OperationPtr operation) {
    auto& cfg = core::config();
    auto instance = uring_manager().acquire(
        operation->scheduler_context(),
        &IOUringBackend::reaper_loop_entry,
        cfg.io_uring_queue_depth(),
        cfg.io_uring_flags(),
        cfg.io_uring_sqpoll());
    if (!instance) return false;
    uring_manager().start_reaper(instance);

    const auto info = core::parse_mode(mode);
    int flags = O_RDONLY;
    if (info.hasW) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (info.hasA) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (info.hasX) flags = O_WRONLY | O_CREAT | O_EXCL;
    if (info.plus) flags = (flags & ~O_ACCMODE) | O_RDWR;

    auto* request = REQ_ALLOC();
    request->type = core::ReqType::Open;
    request->operation = std::move(operation);
    request->operation->submitted();
    request->buffer = core::Buffer::heap(path.size() + 1);
    std::memcpy(request->buf(), path.c_str(), path.size() + 1);

    uring_submit_or_queue(instance.get(), PendingSubmit{
        request,
        IORING_OP_OPENAT,
        AT_FDCWD,
        request->buf(),
        static_cast<unsigned>(flags),
        static_cast<uint64_t>(0644)});
    native_aopen_submissions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

uint64_t native_aopen_submission_count() {
    return native_aopen_submissions.load(std::memory_order_relaxed);
}

} // namespace ayafileio::backend::linux

#endif
