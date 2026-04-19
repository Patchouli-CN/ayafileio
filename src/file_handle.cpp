#include "file_handle.hpp"
#ifdef _WIN32
#include "backends/windows_io_backend.hpp"
#else
#ifdef HAVE_IO_URING
#include "backends/io_uring_backend.hpp"
#endif
#include "backends/thread_io_backend.hpp"
#endif

FileHandle::FileHandle(const std::string &path, const std::string &mode) {
#ifdef _WIN32
    m_backend = new WindowsIOBackend(path, mode);
#else
    // 优先使用 io_uring（如果可用）
#ifdef HAVE_IO_URING
    // 运行时检测 io_uring 是否真的可用
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
            // 记录警告（可选）
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