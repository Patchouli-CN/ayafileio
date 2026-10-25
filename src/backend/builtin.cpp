#include "builtin.hpp"

#include "../core/backend_registry.hpp"
#include "thread/thread_io_backend.hpp"
#ifdef _WIN32
#include "windows/iocp.hpp"
#include "windows/windows_io_backend.hpp"
#endif
#ifdef HAVE_IO_URING
#include "../discover/discover.hpp"
#include "linux/io_uring_backend.hpp"
#include "linux/native_open.hpp"
#endif
#ifdef __APPLE__
#include "../discover/discover.hpp"
#include "macos/macos_gcd_backend.hpp"
#endif

namespace ayafileio::backend {

void register_builtin_backends() {
    auto& registry = core::BackendRegistry::instance();
    registry.clear();
    registry.register_backend(
        core::BackendKind::ThreadPool,
        core::BackendEntry{
            [](const std::string& path, const std::string& mode) {
                return std::make_unique<thread::ThreadIOBackend>(path, mode);
            },
            [](int fd, const std::string& mode, bool owns_fd) {
                return std::make_unique<thread::ThreadIOBackend>(fd, mode, owns_fd);
            },
        });
#ifdef _WIN32
    registry.register_backend(
        core::BackendKind::Iocp,
        core::BackendEntry{
            [](const std::string& path, const std::string& mode) {
                return std::make_unique<windows::WindowsIOBackend>(path, mode);
            },
            [](int fd, const std::string& mode, bool owns_fd) {
                return std::make_unique<windows::WindowsIOBackend>(fd, mode, owns_fd);
            },
        });
    windows::init_iocp();
    registry.set_auto_order({core::BackendKind::Iocp, core::BackendKind::ThreadPool});
#elif defined(HAVE_IO_URING)
    registry.register_backend(
        core::BackendKind::IoUring,
        core::BackendEntry{
            [](const std::string& path, const std::string& mode) {
                return std::make_unique<linux::IOUringBackend>(path, mode);
            },
            [](int fd, const std::string& mode, bool owns_fd) {
                return std::make_unique<linux::IOUringBackend>(fd, mode, owns_fd);
            },
            [] { return discovery::io_uring_available(); },
        });
    registry.set_auto_order({core::BackendKind::IoUring, core::BackendKind::ThreadPool});
#elif defined(__APPLE__)
    registry.register_backend(
        core::BackendKind::DispatchIo,
        core::BackendEntry{
            [](const std::string& path, const std::string& mode) {
                return std::make_unique<macos::MacOSGCDBackend>(path, mode);
            },
            [](int fd, const std::string& mode, bool owns_fd) {
                return std::make_unique<macos::MacOSGCDBackend>(fd, mode, owns_fd);
            },
            [] { return discovery::gcd_dispatch_io_available(); },
        });
    registry.set_auto_order({core::BackendKind::DispatchIo, core::BackendKind::ThreadPool});
#else
    registry.set_auto_order({core::BackendKind::ThreadPool});
#endif
}

void shutdown_builtin_backends() {
#ifdef _WIN32
    windows::shutdown_iocp();
#endif
#ifdef HAVE_IO_URING
    linux::uring_cleanup_all();
#endif
}

bool try_native_aopen(
    const std::string& path,
    const std::string& mode,
    core::OperationPtr operation) {
#ifdef HAVE_IO_URING
    if (core::BackendRegistry::instance().effective_backend() != core::BackendKind::IoUring) {
        return false;
    }
    return linux::submit_native_aopen(path, mode, std::move(operation));
#else
    (void)path;
    (void)mode;
    (void)operation;
    return false;
#endif
}

uint64_t native_aopen_submission_count() {
#ifdef HAVE_IO_URING
    return linux::native_aopen_submission_count();
#else
    return 0;
#endif
}

} // namespace ayafileio::backend
