#include "discover.hpp"

#include "../core/backend_registry.hpp"

namespace ayafileio::discovery {

bool backend_available(core::BackendKind kind) {
    return core::BackendRegistry::instance().available(kind);
}

core::BackendInfo backend_info() {
    core::BackendInfo result;
#if defined(_WIN32)
    result.platform = "windows";
#elif defined(__APPLE__)
    result.platform = "macos";
#elif defined(__linux__)
    result.platform = "linux";
#else
    result.platform = "posix";
#endif
    result.os_version = os_version();

    const auto kind = core::BackendRegistry::instance().effective_backend();
    result.backend = core::backend_name(kind);
    switch (kind) {
        case core::BackendKind::Iocp:
            result.description = "I/O Completion Ports - native async I/O";
            break;
        case core::BackendKind::IoUring:
            result.description = "io_uring - native async I/O (Linux 5.1+)";
            break;
        case core::BackendKind::DispatchIo:
            result.description = "Dispatch I/O (GCD) - native async I/O";
            break;
        case core::BackendKind::ThreadPool:
        case core::BackendKind::Auto:
            result.description = "Thread pool - fallback mode";
            break;
    }
    return result;
}

core::Capabilities capabilities() {
    core::Capabilities result;
    const auto info = backend_info();
    static_cast<core::BackendInfo&>(result) = info;

    const auto kind = core::BackendRegistry::instance().effective_backend();
    if (kind == core::BackendKind::Iocp) {
        result.backend_detail["completion_model"] = true;
        result.backend_detail["batch_harvest"] = true;
    } else if (kind == core::BackendKind::ThreadPool) {
        result.backend_detail["completion_model"] = false;
        result.backend_detail["note"] = std::string("thread pool fallback");
    }
#ifdef HAVE_IO_URING
    if (kind == core::BackendKind::IoUring) {
        result.backend_detail = io_uring_detail();
    }
#endif
#ifdef __APPLE__
    if (kind == core::BackendKind::DispatchIo) {
        result.backend_detail = gcd_detail();
    }
#endif
    return result;
}

} // namespace ayafileio::discovery
