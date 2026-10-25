#include "windows_io_backend.hpp"

#include "../../core/file_mode.hpp"
#include "iocp_context.hpp"

#include <filesystem>
#include <fcntl.h>
#include <io.h>

namespace ayafileio::backend::windows {
namespace {

core::ModeInfo parse_native_mode(const std::string& mode) {
    try {
        return core::parse_mode(mode);
    } catch (const std::invalid_argument& error) {
        throw core::IoException(core::IoError::value(error.what()));
    }
}

void __cdecl ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

} // namespace

WindowsIOBackend::WindowsIOBackend(const std::string& path, const std::string& mode) {
    init_from_path(path, mode);
}

void WindowsIOBackend::init_from_path(const std::string& path, const std::string& mode) {
    const auto info = parse_native_mode(mode);
    DWORD access = 0;
    DWORD disposition = OPEN_EXISTING;
    if (info.hasR) disposition = OPEN_EXISTING;
    if (info.hasW) disposition = CREATE_ALWAYS;
    if (info.hasA) disposition = OPEN_ALWAYS;
    if (info.hasX) disposition = CREATE_NEW;
    if (info.canRead) access |= GENERIC_READ;
    if (info.canWrite) access |= GENERIC_WRITE;

    const std::wstring wide_path = std::filesystem::u8path(path).wstring();
    HANDLE handle = CreateFileW(
        wide_path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, disposition, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(GetLastError()), "Failed to open file: " + path));
    }

    try {
        session_id_ = IOCPContext::instance().create_session(
            handle, info.appendMode, access, true);
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
}

WindowsIOBackend::WindowsIOBackend(int fd, const std::string& mode, bool owns_fd) {
    const auto info = parse_native_mode(mode);
    HANDLE raw = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (raw == INVALID_HANDLE_VALUE || raw == nullptr) {
        throw core::IoException(core::IoError::native(
            static_cast<uint32_t>(GetLastError()), "Failed to get handle from fd"));
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD path_length = GetFinalPathNameByHandleW(
        raw, path.data(), MAX_PATH, FILE_NAME_NORMALIZED);
    if (owns_fd) _close(fd);

    HANDLE handle = INVALID_HANDLE_VALUE;
    if (path_length > 0 && path_length <= MAX_PATH) {
        path.resize(path_length);
        if (path.compare(0, 4, L"\\\\?\\") == 0) path = path.substr(4);
        DWORD disposition = OPEN_EXISTING;
        if (info.hasW) disposition = CREATE_ALWAYS;
        else if (info.hasA) disposition = OPEN_ALWAYS;
        else if (info.hasX) disposition = CREATE_NEW;
        handle = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, disposition, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw core::IoException(core::IoError::native(
                static_cast<uint32_t>(GetLastError()), "Failed to reopen fd with OVERLAPPED flag"));
        }
    } else {
        HANDLE process = GetCurrentProcess();
        if (!DuplicateHandle(process, raw, process, &handle, GENERIC_READ | GENERIC_WRITE, FALSE, 0) &&
            !DuplicateHandle(process, raw, process, &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            throw core::IoException(core::IoError::native(
                static_cast<uint32_t>(GetLastError()), "Failed to duplicate handle from fd"));
        }
    }

    try {
        session_id_ = IOCPContext::instance().create_session(
            handle, info.appendMode, 0, true);
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
}

WindowsIOBackend::~WindowsIOBackend() {
    close_impl();
}

void WindowsIOBackend::read(int64_t size, core::OperationPtr operation) {
    IOCPContext::instance().submit_read(session_id_, size, std::move(operation));
}

void WindowsIOBackend::read_at(int64_t offset, int64_t size, core::OperationPtr operation) {
    IOCPContext::instance().submit_read_at(session_id_, offset, size, std::move(operation));
}

void WindowsIOBackend::write(core::Buffer view, int64_t position, core::OperationPtr operation) {
    IOCPContext::instance().submit_write(session_id_, std::move(view), position, std::move(operation));
}

void WindowsIOBackend::seek(int64_t offset, int whence, core::OperationPtr operation) {
    IOCPContext::instance().submit_seek(session_id_, offset, whence, std::move(operation));
}

void WindowsIOBackend::flush(core::OperationPtr operation) {
    IOCPContext::instance().submit_flush(session_id_, std::move(operation));
}

void WindowsIOBackend::close(core::OperationPtr operation) {
    IOCPContext::instance().submit_close(session_id_, operation);
    if (!IOCPContext::instance().get_session(session_id_)) session_id_ = 0;
}

void WindowsIOBackend::tell(core::OperationPtr operation) {
    IOCPContext::instance().submit_tell(session_id_, std::move(operation));
}

void WindowsIOBackend::truncate(int64_t size, core::OperationPtr operation) {
    IOCPContext::instance().submit_truncate(session_id_, size, std::move(operation));
}

void WindowsIOBackend::readinto(core::Buffer buffer, core::OperationPtr operation) {
    IOCPContext::instance().submit_readinto(session_id_, std::move(buffer), std::move(operation));
}

int WindowsIOBackend::fileno() const {
    if (cached_fileno_ >= 0) return cached_fileno_;
    auto session = IOCPContext::instance().get_session(session_id_);
    if (!session || session->handle == INVALID_HANDLE_VALUE) return -1;

    HANDLE process = GetCurrentProcess();
    HANDLE duplicated = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(process, session->handle, process, &duplicated,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }

    int flags = _O_BINARY;
    const DWORD access = session->access;
    if (access == 0 || ((access & GENERIC_READ) && (access & GENERIC_WRITE))) {
        flags |= _O_RDWR;
    } else if (access & GENERIC_WRITE) {
        flags |= _O_WRONLY;
    } else {
        flags |= _O_RDONLY;
    }

    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicated), flags);
    if (fd == -1) {
        CloseHandle(duplicated);
        return -1;
    }
    cached_fileno_ = fd;
    cached_dup_handle_ = duplicated;
    return fd;
}

void WindowsIOBackend::close_impl() {
    if (cached_fileno_ >= 0) {
        const int fd = cached_fileno_;
        const auto mine = cached_dup_handle_;
        cached_fileno_ = -1;
        cached_dup_handle_ = INVALID_HANDLE_VALUE;

        auto old = _set_thread_local_invalid_parameter_handler(ignore_invalid_parameter);
        const intptr_t slot = _get_osfhandle(fd);
        if (slot != -1 && reinterpret_cast<HANDLE>(slot) == mine) _close(fd);
        _set_thread_local_invalid_parameter_handler(old);
    }
    if (session_id_ != 0) {
        const auto id = session_id_;
        session_id_ = 0;
        if (IOCPContext::instance().get_session(id)) {
            IOCPContext::instance().submit_close(id, core::make_noop_operation());
        }
    }
}

} // namespace ayafileio::backend::windows
