#include "file_handle.hpp"
#ifdef _WIN32
#include "backends/windows_io_backend.hpp"
#elif defined(__APPLE__)
#include "backends/macos_gcd_backend.hpp"
#include "backends/thread_io_backend.hpp"
#else
#ifdef HAVE_IO_URING
#include "backends/io_uring_backend.hpp"
#endif
#include "backends/thread_io_backend.hpp"
#endif

// 从路径文件打开
FileHandle::FileHandle(const std::string &path, const std::string &mode) {
#ifdef _WIN32
    m_backend = new WindowsIOBackend(path, mode);
#elif defined(__APPLE__)
    // macOS: 优先使用 Dispatch I/O 实现真异步
    try {
        m_backend = new MacOSGCDBackend(path, mode);
    } catch (const std::exception& e) {
        // 如果 GCD 后端失败，降级到线程池
        m_backend = new ThreadIOBackend(path, mode);
    }
#else
    // Linux/其他 POSIX: 优先使用 io_uring
#ifdef HAVE_IO_URING
    static bool has_io_uring = []() {
        struct io_uring ring;
        int ret = io_uring_queue_init(8, &ring, 0);
        if (ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    
    if (has_io_uring) {
        try {
            m_backend = new IOUringBackend(path, mode);
            return;
        } catch (const std::exception& e) {
            // io_uring 后端创建失败，降级到线程池
        }
    }
#endif
    // 降级到线程池后端
    m_backend = new ThreadIOBackend(path, mode);
#endif
}

FileHandle::~FileHandle() {
    delete m_backend;
}

// 从 fd 打开
FileHandle::FileHandle(int fd, const std::string& mode, bool owns_fd) {
#ifdef _WIN32
    m_backend = new WindowsIOBackend(fd, mode, owns_fd);
#elif defined(__APPLE__)
    try {
        m_backend = new MacOSGCDBackend(fd, mode, owns_fd);
    } catch (const std::exception&) {
        m_backend = new ThreadIOBackend(fd, mode, owns_fd);
    }
#else
#ifdef HAVE_IO_URING
    static bool has_io_uring = []() {
        struct io_uring ring;
        int ret = io_uring_queue_init(8, &ring, 0);
        if (ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    
    if (has_io_uring) {
        try {
            m_backend = new IOUringBackend(fd, mode, owns_fd);
            return;
        } catch (const std::exception&) {}
    }
#endif
    m_backend = new ThreadIOBackend(fd, mode, owns_fd);
#endif
}