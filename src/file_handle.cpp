#include "file_handle.hpp"
#include "discover.h"          // effective_backend()
#ifdef _WIN32
#include "backends/windows_io_backend.hpp"
#include "backends/thread_io_backend.hpp"
#elif defined(__APPLE__)
#include "backends/macos_gcd_backend.hpp"
#include "backends/thread_io_backend.hpp"
#else
#ifdef HAVE_IO_URING
#include "backends/io_uring_backend.hpp"
#endif
#include "backends/thread_io_backend.hpp"
#endif

namespace ayafileio {

// 从路径文件打开
FileHandle::FileHandle(const std::string &path, const std::string &mode)
    : FileHandle(path, mode, nullptr, nullptr) {}

FileHandle::FileHandle(const std::string &path, const std::string &mode,
                       PyObject *loop, PyObject *create_future) {
#ifndef _WIN32
    // POSIX 后端惰性绑定事件循环（首次 I/O 时），注入参数仅 Windows 使用
    (void)loop; (void)create_future;
#endif
    // 后端由 configure({"backend": ...}) 决定：Auto 走平台探测，强制值直接
    // 生效（可用性在 configure 时已校验，effective_backend 再兜一次底）。
    const BackendKind kind = effective_backend();

#ifdef _WIN32
    // Windows 目前只有 IOCP 后端：线程池后端的 Windows 移植还在收尾（原语层已
    // 就位，但全套用例下仍有崩溃/内存暴涨），configure({"backend": "thread_pool"})
    // 在 Windows 上会被 backend_available() 拒绝，走不到这里。
    (void)kind;
    if (loop) {
        // open_async 注入路径：工作线程无 running loop，用预取的 loop
        m_backend = new WindowsIOBackend(path, mode, loop, create_future);
    } else {
        m_backend = new WindowsIOBackend(path, mode);
    }
#else
#  ifdef __APPLE__
    if (kind == BackendKind::DispatchIo) {
        try {
            m_backend = new MacOSGCDBackend(path, mode);
            return;
        } catch (const std::exception &) {
            // GCD 后端创建失败 → 降级到线程池
        }
    }
#  endif
#  ifdef HAVE_IO_URING
    if (kind == BackendKind::IoUring) {
        try {
            m_backend = new IOUringBackend(path, mode);
            return;
        } catch (const std::exception &) {
            // io_uring 后端创建失败 → 降级到线程池
        }
    }
#  endif
    (void)kind;
    m_backend = new ThreadIOBackend(path, mode);
#endif
}

FileHandle::~FileHandle() {
    delete m_backend;
}

// 从 fd 打开
FileHandle::FileHandle(int fd, const std::string& mode, bool owns_fd) {
    const BackendKind kind = effective_backend();
#ifdef _WIN32
    (void)kind;
    m_backend = new WindowsIOBackend(fd, mode, owns_fd);
#else
#  ifdef __APPLE__
    if (kind == BackendKind::DispatchIo) {
        try {
            m_backend = new MacOSGCDBackend(fd, mode, owns_fd);
            return;
        } catch (const std::exception&) {
            // 降级到线程池
        }
    }
#  endif
#  ifdef HAVE_IO_URING
    if (kind == BackendKind::IoUring) {
        try {
            m_backend = new IOUringBackend(fd, mode, owns_fd);
            return;
        } catch (const std::exception&) {
            // 降级到线程池
        }
    }
#  endif
    (void)kind;
    m_backend = new ThreadIOBackend(fd, mode, owns_fd);
#endif
}

bool try_native_aopen(PyObject* loop, PyObject* future,
                      ResultBatcher* batcher,
                      const char* path, const char* mode) {
#ifdef HAVE_IO_URING
    // 后端被强制成 thread_pool 时不能悄悄走 io_uring 的原生 OPENAT：
    // 否则 configure({"backend": "thread_pool"}) 之后 aopen 仍然用 io_uring，
    // 能力矩阵和实际行为对不上，CI 的 thread_pool 覆盖也就落空了。
    if (effective_backend() != BackendKind::IoUring) return false;
    return submit_native_aopen(loop, future, batcher, path, mode);
#else
    (void)loop;
    (void)future;
    (void)batcher;
    (void)path;
    (void)mode;
    return false;
#endif
}

uint64_t native_aopen_submission_count() {
#ifdef HAVE_IO_URING
    return io_uring_native_aopen_submission_count();
#else
    return 0;
#endif
}

} // namespace ayafileio