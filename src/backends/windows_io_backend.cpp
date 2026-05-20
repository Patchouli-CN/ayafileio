#include "windows_io_backend.hpp"
#include "./utils/file_mode.hpp"
#include <algorithm>
#include <filesystem>

// ════════════════════════════════════════════════════════════════════════════
// WindowsIOBackend — constructor (path)
// ════════════════════════════════════════════════════════════════════════════

WindowsIOBackend::WindowsIOBackend(const std::string &path, const std::string &mode) {
    auto &cfg = ayafileio::config();

    // ── get event loop + create_future ────────────────────────────────────
    PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    PyObject *create_future = PyObject_GetAttr(loop, g_str_create_future);
    if (!create_future) throw py::python_error();

    // ── parse mode ────────────────────────────────────────────────────────
    DWORD access = 0, disp = OPEN_EXISTING;
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        Py_DECREF(create_future);
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

    // ── open / acquire HANDLE ─────────────────────────────────────────────
    HANDLE h = INVALID_HANDLE_VALUE;
    PoolKey poolKey;
    bool canReuse = (disp == OPEN_EXISTING || disp == OPEN_ALWAYS);
    if (canReuse) {
        poolKey = make_pool_key(path, access, disp);
        h = handle_pool_acquire(poolKey);
    }

    std::wstring wpath = std::filesystem::u8path(path).wstring();

    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(wpath.c_str(), access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, disp, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            Py_DECREF(create_future);
            win_throw_os_error(err, "Failed to open file", path.c_str());
        }
    }

    // ── register with IOCPContext ─────────────────────────────────────────
    try {
        m_sessionId = IOCPContext::instance().create_session(
            h, loop, create_future, appendMode, poolKey, /*owns_fd=*/true);
    } catch (...) {
        CloseHandle(h);
        Py_DECREF(create_future);
        throw;
    }
    Py_DECREF(create_future);

    // ── init file position ────────────────────────────────────────────────
    if (appendMode) {
        auto s = IOCPContext::instance().get_session(m_sessionId);
        if (s) {
            LARGE_INTEGER li{};
            if (GetFileSizeEx(h, &li)) {
                std::lock_guard<std::mutex> lk(s->posMtx);
                s->filePos = (uint64_t)li.QuadPart;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// WindowsIOBackend — constructor (fd)
// ════════════════════════════════════════════════════════════════════════════

WindowsIOBackend::WindowsIOBackend(int fd, const std::string &mode, bool owns_fd) {
    auto &cfg = ayafileio::config();

    // ── get event loop + create_future ────────────────────────────────────
    PyObject *loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    PyObject *create_future = PyObject_GetAttr(loop, g_str_create_future);
    if (!create_future) throw py::python_error();

    // ── parse mode ────────────────────────────────────────────────────────
    ModeInfo mi;
    try {
        mi = parse_mode(mode);
    } catch (const std::invalid_argument &e) {
        Py_DECREF(create_future);
        throw py::value_error(e.what());
    }
    bool appendMode = mi.appendMode;

    // ── get raw HANDLE from fd ────────────────────────────────────────────
    HANDLE raw_handle = (HANDLE)_get_osfhandle(fd);
    if (raw_handle == INVALID_HANDLE_VALUE || raw_handle == NULL) {
        DWORD err = GetLastError();
        Py_DECREF(create_future);
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
    PoolKey poolKey;  // empty path → non-poolable

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
            Py_DECREF(create_future);
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
                Py_DECREF(create_future);
                win_throw_os_error(err, "Failed to duplicate handle from fd");
            }
        }
    }

    // ── register with IOCPContext ─────────────────────────────────────────
    try {
        m_sessionId = IOCPContext::instance().create_session(
            h, loop, create_future, appendMode, poolKey, ownsFd);
    } catch (...) {
        if (ownsFd) CloseHandle(h);
        Py_DECREF(create_future);
        throw;
    }
    Py_DECREF(create_future);

    // ── init file position ────────────────────────────────────────────────
    if (appendMode) {
        auto s = IOCPContext::instance().get_session(m_sessionId);
        if (s) {
            LARGE_INTEGER li{};
            if (GetFileSizeEx(h, &li)) {
                std::lock_guard<std::mutex> lk(s->posMtx);
                s->filePos = (uint64_t)li.QuadPart;
            }
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

PyObject *WindowsIOBackend::write(Py_buffer *view) {
    return IOCPContext::instance().submit_write(m_sessionId, view);
}

PyObject *WindowsIOBackend::seek(int64_t offset, int whence) {
    return IOCPContext::instance().submit_seek(m_sessionId, offset, whence);
}

PyObject *WindowsIOBackend::flush() {
    return IOCPContext::instance().submit_flush(m_sessionId);
}

PyObject *WindowsIOBackend::close() {
    return IOCPContext::instance().submit_close(m_sessionId);
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

int WindowsIOBackend::fileno() const {
    auto s = IOCPContext::instance().get_session(m_sessionId);
    if (!s || s->handle == INVALID_HANDLE_VALUE) return -1;
    return _open_osfhandle((intptr_t)s->handle, 0);
}

void WindowsIOBackend::close_impl() {
    if (m_sessionId != 0) {
        uint64_t sid = m_sessionId;
        m_sessionId = 0;
        PyObject *future = IOCPContext::instance().submit_close(sid);
        Py_XDECREF(future);
    }
}
