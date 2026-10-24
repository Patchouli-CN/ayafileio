// windows_io_backend.hpp
#pragma once
#include "../io_backend.hpp"
#include "../iocp_context.hpp"
#include <string>

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// §7  Windows IO Backend — lightweight forwarding layer
//
// All I/O state lives in IOCPContext::Session. WindowsIOBackend only holds
// a sessionId and delegates every operation to IOCPContext::submit_*().
// ════════════════════════════════════════════════════════════════════════════

class WindowsIOBackend : public IOBackendBase {
public:
    WindowsIOBackend(const std::string& path, const std::string& mode);
    // open_async 专用：loop/create_future 由调用方在事件循环线程预先取好
    // 注入（工作线程上 asyncio.get_running_loop() 无 running loop 可用）。
    // 引用由调用方持有并归还（见 bindings.cpp open_async）。
    WindowsIOBackend(const std::string& path, const std::string& mode,
                     PyObject* loop, PyObject* create_future);
    WindowsIOBackend(int fd, const std::string& mode, bool owns_fd = false);
    ~WindowsIOBackend() override;

    PyObject* read(int64_t size = -1) override;
    PyObject* read_at(int64_t offset, int64_t size) override;
    PyObject* write(Py_buffer* view, int64_t position = -1) override;
    PyObject* seek(int64_t offset, int whence = 0) override;
    PyObject* flush() override;
    PyObject* close() override;
    PyObject* tell() override;
    PyObject* truncate(int64_t size) override;
    PyObject* readinto(PyObject* buf) override;
    int fileno() const override;
    void close_impl() override;

    // Completion handlers — unused; IOCPContext workers process completions
    // directly via sessionId lookup. Override as defensive no-ops.
    void complete_ok(IORequest* req, size_t /*bytes*/) override {
        if (req) delete req;
    }
    void complete_error(IORequest* req, DWORD /*err*/) override {
        if (req) delete req;
    }

private:
    // 两个 path 构造函数的公共实现；不负责 loop/create_future 的引用记账
    void init_from_path(const std::string& path, const std::string& mode,
                        PyObject* loop, PyObject* create_future);

    uint64_t m_sessionId = 0;

    // fileno() 暴露给 Python 的 CRT 描述符：**复制**出来的句柄 + 只创建一次。
    // m_cached_fileno < 0 表示尚未创建。由 close_impl()/析构负责关闭。
    //
    // 为什么要连 m_cached_dup_handle 一起记：调用方可能自己 os.close() 掉这个
    // 描述符（标准库里这是明确不支持的用法，但不能让它把进程搞崩）。关掉之后
    // fd 号会被后开的描述符复用，光凭 _get_osfhandle(fd) != -1 判断「还在」
    // 会把别人的描述符当成自己的——实测那样会关掉 asyncio 的 self-pipe
    // socketpair，进程在事件循环收尾时 fastfail（0xC0000409）。所以必须比
    // 对**句柄身份**：只有槽位里仍然是我们那一份复制句柄时才由我们关。
    mutable int m_cached_fileno = -1;
    mutable HANDLE m_cached_dup_handle = INVALID_HANDLE_VALUE;
};

} // namespace ayafileio
