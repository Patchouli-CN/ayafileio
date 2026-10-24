#include "windows_io_backend.hpp"
#include "./utils/file_mode.hpp"
#include <algorithm>
#include <filesystem>
#include <fcntl.h>   // _O_BINARY / _O_RDWR …
#include <io.h>      // _open_osfhandle / _close / _get_osfhandle

namespace ayafileio {

namespace {

// 作用域结束自动 Py_DECREF。构造函数里有多条 throw 出口，手写 DECREF 极容易
// 漏——1.15.0 之前的「每次 open 泄漏一个 loop 引用」正是这么漏出来的。
struct PyRef {
    PyObject *p = nullptr;
    PyRef() = default;
    explicit PyRef(PyObject *o) : p(o) {}
    ~PyRef() { Py_XDECREF(p); }
    PyRef(const PyRef &) = delete;
    PyRef &operator=(const PyRef &) = delete;
    PyObject *get() const { return p; }
};

// CRT 的 invalid-parameter 默认 handler 是 _invoke_watson → __fastfail，
// 进程直接 0xC0000409。而 _get_osfhandle()/_close() 对**已经被关掉**的 fd
// 正好会走这条校验（文档：fd 不是有效描述符时调用 invalid parameter handler），
// 于是「调用方自己 os.close(f.fileno()) 之后再关文件」会当场把进程打死。
// 换成一个只返回的 handler，这些调用退化成返回 -1 / errno=EBADF。
// thread_local 版本：不影响进程里其它线程，也不改动全局策略。
void __cdecl ignore_invalid_parameter(const wchar_t *, const wchar_t *,
                                      const wchar_t *, unsigned int, uintptr_t) {}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// WindowsIOBackend — constructor (path)
// ════════════════════════════════════════════════════════════════════════════

WindowsIOBackend::WindowsIOBackend(const std::string &path, const std::string &mode) {
    // ── get event loop + create_future ────────────────────────────────────
    // 两者都是本作用域独占的引用：Session 在 create_session 里自行 INCREF，
    // 所以出口处必须归还（PyRef 负责，异常路径也一样）。
    PyRef loop(PyObject_CallNoArgs(g_get_running_loop));
    if (!loop.get()) throw py::python_error();
    PyRef create_future(PyObject_GetAttr(loop.get(), g_str_create_future));
    if (!create_future.get()) throw py::python_error();

    init_from_path(path, mode, loop.get(), create_future.get());
}

// open_async 专用：loop/create_future 由调用方在事件循环线程预取注入
// （工作线程上 asyncio.get_running_loop() 无 running loop 可用）。引用
// 由调用方持有并归还；create_session 内部会自行 INCREF 需要留存的部分。
WindowsIOBackend::WindowsIOBackend(const std::string &path, const std::string &mode,
                                   PyObject *loop, PyObject *create_future) {
    init_from_path(path, mode, loop, create_future);
}

void WindowsIOBackend::init_from_path(const std::string &path, const std::string &mode,
                                      PyObject *loop, PyObject *create_future) {
    auto &cfg = ayafileio::config();

    // ── parse mode ────────────────────────────────────────────────────────
    DWORD access = 0, disp = OPEN_EXISTING;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        throw py::value_error(e.what());
    }
    bool canRead    = mi.canRead;
    bool canWrite   = mi.canWrite;
    bool appendMode = mi.appendMode;

    if (mi.hasR) { disp = OPEN_EXISTING; }
    if (mi.hasW) { disp = CREATE_ALWAYS; }
    if (mi.hasA) { appendMode = true; disp = OPEN_ALWAYS; }
    if (mi.hasX) { disp = CREATE_NEW; }

    if (canRead)  access |= GENERIC_READ;
    if (canWrite) access |= GENERIC_WRITE;

    // ── open HANDLE ───────────────────────────────────────────────────────
    std::wstring wpath = std::filesystem::u8path(path).wstring();

    HANDLE h;
    {
        // 阻塞型系统调用：释放 GIL——open_async 的工作线程借此真正
        // 并行打开；同步 open 路径也不再冻结整个解释器
        GilRelease gr;
        h = CreateFileW(wpath.c_str(), access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, disp, FILE_FLAG_OVERLAPPED, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        win_throw_os_error(err, "Failed to open file", path.c_str());
    }

    // ── register with IOCPContext ─────────────────────────────────────────
    try {
        m_sessionId = IOCPContext::instance().create_session(
            h, loop, create_future, appendMode, access, /*owns_fd=*/true);
    } catch (...) {
        CloseHandle(h);
        throw;
    }

    // ── init file position (cachedFileSize already set in create_session) ───
    if (appendMode) {
        auto s = IOCPContext::instance().get_session(m_sessionId);
        if (s) {
            std::lock_guard<std::mutex> lk(s->posMtx);
            s->filePos = s->cachedFileSize;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// WindowsIOBackend — constructor (fd)
// ════════════════════════════════════════════════════════════════════════════

WindowsIOBackend::WindowsIOBackend(int fd, const std::string &mode, bool owns_fd) {
    auto &cfg = ayafileio::config();

    // ── get event loop + create_future ────────────────────────────────────
    // 同 path 构造：本作用域独占，Session 自行 INCREF，出口由 PyRef 归还
    PyRef loop(PyObject_CallNoArgs(g_get_running_loop));
    if (!loop.get()) throw py::python_error();
    PyRef create_future(PyObject_GetAttr(loop.get(), g_str_create_future));
    if (!create_future.get()) throw py::python_error();

    // ── parse mode ────────────────────────────────────────────────────────
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        throw py::value_error(e.what());
    }
    bool appendMode = mi.appendMode;

    // ── get raw HANDLE from fd ────────────────────────────────────────────
    HANDLE raw_handle = (HANDLE)_get_osfhandle(fd);
    if (raw_handle == INVALID_HANDLE_VALUE || raw_handle == NULL) {
        DWORD err = GetLastError();
        win_throw_os_error(err, "Failed to get handle from fd");
    }

    // Try to get file path
    std::wstring wpath(MAX_PATH, L'\0');
    DWORD path_len = GetFinalPathNameByHandleW(raw_handle, &wpath[0], MAX_PATH,
                                                FILE_NAME_NORMALIZED);

    // If taking ownership, close the original fd (avoids two handles)
    if (owns_fd) {
        _close(fd);
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    bool ownsFd = true;

    if (path_len > 0 && path_len <= MAX_PATH) {
        // ──方案 A: have path, reopen with OVERLAPPED ───────────────────────
        wpath.resize(path_len);
        if (wpath.compare(0, 4, L"\\\\?\\") == 0) {
            wpath = wpath.substr(4);
        }

        DWORD access = GENERIC_READ | GENERIC_WRITE;
        DWORD disp = OPEN_EXISTING;
        if (mi.hasW)      disp = CREATE_ALWAYS;
        else if (mi.hasA) disp = OPEN_ALWAYS;
        else if (mi.hasX) disp = CREATE_NEW;

        h = CreateFileW(wpath.c_str(), access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, disp, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            win_throw_os_error(err, "Failed to reopen fd with OVERLAPPED flag");
        }
    } else {
        // ──方案 B: no path (socket/pipe), duplicate handle ─────────────────
        HANDLE proc = GetCurrentProcess();
        if (!DuplicateHandle(proc, raw_handle, proc, &h,
                             GENERIC_READ | GENERIC_WRITE, FALSE, 0)) {
            if (!DuplicateHandle(proc, raw_handle, proc, &h,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                DWORD err = GetLastError();
                win_throw_os_error(err, "Failed to duplicate handle from fd");
            }
        }
    }

    // ── register with IOCPContext ─────────────────────────────────────────
    try {
        // fd 路径 access 未知，传 0 → 挂 read IOCP（与历史行为一致）
        m_sessionId = IOCPContext::instance().create_session(
            h, loop.get(), create_future.get(), appendMode, /*access=*/0, ownsFd);
    } catch (...) {
        if (ownsFd) CloseHandle(h);
        throw;
    }

    // ── init file position (cachedFileSize already set in create_session) ───
    if (appendMode) {
        auto s = IOCPContext::instance().get_session(m_sessionId);
        if (s) {
            std::lock_guard<std::mutex> lk(s->posMtx);
            s->filePos = s->cachedFileSize;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Destructor
// ════════════════════════════════════════════════════════════════════════════

WindowsIOBackend::~WindowsIOBackend() {
    close_impl();
}

// ════════════════════════════════════════════════════════════════════════════
// I/O methods — all delegate to IOCPContext
// ════════════════════════════════════════════════════════════════════════════

PyObject *WindowsIOBackend::read(int64_t size) {
    return IOCPContext::instance().submit_read(m_sessionId, size);
}

PyObject *WindowsIOBackend::read_at(int64_t offset, int64_t size) {
    return IOCPContext::instance().submit_read_at(m_sessionId, offset, size);
}

PyObject *WindowsIOBackend::write(Py_buffer *view, int64_t position) {
    return IOCPContext::instance().submit_write(m_sessionId, view, position);
}

PyObject *WindowsIOBackend::seek(int64_t offset, int whence) {
    return IOCPContext::instance().submit_seek(m_sessionId, offset, whence);
}

PyObject *WindowsIOBackend::flush() {
    return IOCPContext::instance().submit_flush(m_sessionId);
}

PyObject *WindowsIOBackend::close() {
    PyObject *future = IOCPContext::instance().submit_close(m_sessionId);
    // submit_close removes the session synchronously. Do not try to close
    // it again when Python destroys this object after its loop has stopped.
    if (future && !IOCPContext::instance().get_session(m_sessionId)) {
        m_sessionId = 0;
    }
    return future;
}

PyObject *WindowsIOBackend::tell() {
    return IOCPContext::instance().submit_tell(m_sessionId);
}

PyObject *WindowsIOBackend::truncate(int64_t size) {
    return IOCPContext::instance().submit_truncate(m_sessionId, size);
}

PyObject *WindowsIOBackend::readinto(PyObject *buf) {
    return IOCPContext::instance().submit_readinto(m_sessionId, buf);
}

// 把会话的文件句柄暴露成 CRT 描述符。
//
// 契约（对齐 io.FileIO.fileno）：同一个对象每次返回**同一个** fd，fd 由本对象
// 拥有、在 close() 时释放，调用方不需要也不应该 close 它。
//
// 之前是直接 _open_osfhandle(s->handle)：每调一次就新建一个 CRT 描述符而不释放
// （泄漏 fd 表槽位），而且这个描述符拥有的是**会话自己的** HANDLE——调用方
// os.close(f.fileno()) 会把文件对象的句柄一起关掉，之后所有 I/O 全部
// EBADF（实测 ReadFile failed, errno 6）。现在复制一份句柄再包装：
// 描述符与句柄互相独立，关掉它不影响文件对象本身。
int WindowsIOBackend::fileno() const {
    if (m_cached_fileno >= 0) return m_cached_fileno;

    auto s = IOCPContext::instance().get_session(m_sessionId);
    if (!s || s->handle == INVALID_HANDLE_VALUE) return -1;

    HANDLE proc = GetCurrentProcess();
    HANDLE dup  = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(proc, s->handle, proc, &dup,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        return -1;

    int flags = _O_BINARY;
    const DWORD access = s->access;   // fd 构造的会话未知，按可读写处理
    if (access == 0 || ((access & GENERIC_READ) && (access & GENERIC_WRITE)))
        flags |= _O_RDWR;
    else if (access & GENERIC_WRITE)
        flags |= _O_WRONLY;
    else
        flags |= _O_RDONLY;   // _O_RDONLY == 0，此处纯属自说明

    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(dup), flags);
    if (fd == -1) { CloseHandle(dup); return -1; }

    m_cached_fileno     = fd;
    m_cached_dup_handle = dup;
    return fd;
}

void WindowsIOBackend::close_impl() {
    // 放掉 fileno() 暴露的那份复制句柄。
    //
    // 两个坑都得防：
    //  1. 调用方可能自己 os.close() 掉这个描述符（标准库里属于未定义用法，
    //     但不能让它把进程搞崩）——直接 _get_osfhandle/_close 会走 CRT 的
    //     invalid-parameter 校验并 __fastfail，所以整段在非致命 handler 下跑。
    //  2. fd 号可能已经被后开的描述符复用，光看「有效性」会把别人的描述符
    //     当成自己的（实测会关掉 asyncio 的 self-pipe socketpair）。
    //     因此必须比对句柄身份：槽位里仍是我们那一份复制句柄才关。
    if (m_cached_fileno >= 0) {
        const int  fd   = m_cached_fileno;
        const auto mine = m_cached_dup_handle;
        m_cached_fileno     = -1;
        m_cached_dup_handle = INVALID_HANDLE_VALUE;

        auto old = _set_thread_local_invalid_parameter_handler(ignore_invalid_parameter);
        const intptr_t slot = _get_osfhandle(fd);
        if (slot != -1 && reinterpret_cast<HANDLE>(slot) == mine) {
            _close(fd);                // 连带关闭复制出来的句柄
        }
        _set_thread_local_invalid_parameter_handler(old);
    }
    if (m_sessionId != 0) {
        uint64_t sid = m_sessionId;
        m_sessionId = 0;
        // Emergency shutdown may also have removed this session. The
        // missing-session submit_close path requires a running loop.
        if (!IOCPContext::instance().get_session(sid)) return;
        PyObject *future = IOCPContext::instance().submit_close(sid);
        Py_XDECREF(future);
    }
}

} // namespace ayafileio
