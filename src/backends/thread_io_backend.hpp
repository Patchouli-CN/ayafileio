// thread_io_backend.hpp
#pragma once
#include "../io_backend.hpp"
#include "../global_thread_pool.hpp"
#include "../utils/native_file.hpp"   // 跨平台 open/pread/pwrite/…（POSIX 直调，Windows 走 OVERLAPPED）
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <functional>
#include <cstdint>
#include <condition_variable>
#include <vector>

namespace ayafileio {

class ThreadIOBackend : public IOBackendBase {
public:
    ThreadIOBackend(const std::string& path, const std::string& mode);
    ThreadIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~ThreadIOBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* read_at(int64_t offset, int64_t size) override;
    PyObject* write(Py_buffer* view, int64_t position = -1) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    // POSIX 直接返回真实 fd；Windows 返回一份**复制句柄**包装出来的 CRT 描述符
    // （与 IOCP 后端同一套语义：稳定、可独立 close、不牵连文件对象）
    int fileno() const override;
    void close_impl() override;

private:
    file_handle_t m_fd = kFileInvalid;   // POSIX: fd；Windows: HANDLE
    std::atomic<bool> m_running{false};
    std::mutex m_posMtx;
    uint64_t m_filePos = 0;
    uint64_t m_cachedFileSize = 0;  // open 时缓存，写/截断路径乐观更新
    bool m_appendMode = false;

#ifdef _WIN32
    // fileno() 在 Windows 上要暴露一份**复制**出来的句柄（见 .cpp 的实现说明）
    mutable std::mutex m_filenoMtx;
    mutable int m_cachedFileno = -1;
    mutable HANDLE m_cachedDupHandle = INVALID_HANDLE_VALUE;
#endif

    // 事件循环相关成员 - 延迟初始化
    std::atomic<bool> m_loop_initialized{false};
    std::mutex m_loop_init_mtx;
    PyObject* m_loop = nullptr;
    PyObject* m_create_future = nullptr;

    unsigned m_num_workers = 0;

    // 初始化方法
    void ensure_loop_initialized();
    void enqueue_task(std::function<void()> task);

    // 用内核真值刷新 m_cachedFileSize。缓存是 open 时的快照，之后只由本会话的
    // 写入/截断更新——别的进程（或同进程另一个句柄）追加的数据这里看不到，
    // 把旧长度当真相就会静默少读。只在「已到 EOF 判决点」或「发生短读」时调用，
    // 热路径一次 syscall 都不多加。
    void refresh_cached_size_locked();   // 调用方须持 m_posMtx
    void refresh_cached_size();          // 自行加锁（供 worker 线程的完成路径用）
};

} // namespace ayafileio