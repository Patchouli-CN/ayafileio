#pragma once
// ════════════════════════════════════════════════════════════════════════════
// native_file.hpp — 跨平台文件原语（给 ThreadIOBackend 用）
//
// ThreadIOBackend 的定位是「所有平台都能显式选用的线程池后端」，所以它不能
// 直接调 POSIX 的 open/pread/pwrite。这里把需要的原语收成一小层：
//
//   POSIX   : open / pread / pwrite / fstat / fsync / ftruncate / close 直调
//   Windows : CreateFileW(FILE_FLAG_OVERLAPPED) + ReadFile/WriteFile(OVERLAPPED)
//             + GetOverlappedResult 做**定位** I/O
//
// 为什么 Windows 一定要开 FILE_FLAG_OVERLAPPED：非 overlapped 句柄上
// ReadFile 会**忽略** OVERLAPPED 里的偏移、改用文件指针，多 worker 并发读就
// 会互相踩。开了这个 flag 之后 ReadFile 是定位的，返回值只有两种：
// 同步完成，或者 ERROR_IO_PENDING 后 GetOverlappedResult(..., TRUE) 阻塞等到
// 完成 —— 对线程池来说正是想要的「阻塞式定位 I/O」。
//
// 错误码约定：`native_file_last_error()` 返回**平台原生**错误码
// （POSIX: errno / Windows: GetLastError），因为 IOBackendBase::complete_error
// 本身就是按平台映射的。
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <string>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ayafileio::platform {

// 句柄统一存成 intptr_t：POSIX 是 fd，Windows 是 HANDLE 强转
using file_handle_t = std::intptr_t;

#ifdef _WIN32
static constexpr file_handle_t kFileInvalid = static_cast<file_handle_t>(-1);
#else
static constexpr file_handle_t kFileInvalid = -1;
#endif

// 打开标志（不用 POSIX 的 O_*，避免在 Windows 上重定义一整套）
enum FileOpenFlags : int {
    kOpenRead      = 1 << 0,
    kOpenWrite     = 1 << 1,
    kOpenCreate    = 1 << 2,
    kOpenTruncate  = 1 << 3,
    kOpenAppend    = 1 << 4,
    kOpenExclusive = 1 << 5,
};

// 平台原生错误码（见文件头说明）
inline thread_local int g_native_file_error = 0;

inline int native_file_last_error() { return g_native_file_error; }

#ifdef _WIN32
// 每个线程一个 manual-reset 事件，供 OVERLAPPED.hEvent 使用。
//
// ⚠ 这一条是**必须**的，实测数据：同一 overlapped 句柄上 16 线程 × 300 次并发写，
//    “每次新建 OVERLAPPED + hEvent=NULL” 崩 7/8 次（0xC0000005）；给 OVERLAPPED
//    配线程私有事件后 0/8。
//
//   原因：hEvent == NULL 时 GetOverlappedResult(..., TRUE) 是拿**文件句柄**当等待
//   对象的，而对 FILE_FLAG_OVERLAPPED 的句柄来说那个信号并不按 op 一对一，等待会
//   提前返回；此时内核仍在往那块 OVERLAPPED（栈上，每次新建）里写完成状态，下一次
//   调用覆盖同一地址 → 访问冲突。
//
//   事件句柄随线程退出留一次（池里线程数有界，可忽略）。
inline HANDLE thread_overlapped_event() {
    static thread_local HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return ev;
}
#endif

inline file_handle_t file_open(const char *path, int flags) {
    g_native_file_error = 0;
#ifdef _WIN32
    DWORD access = 0;
    if (flags & kOpenRead)  access |= GENERIC_READ;
    if (flags & kOpenWrite) access |= GENERIC_WRITE;
    if (access == 0) access = GENERIC_READ;

    DWORD disp;
    if (flags & kOpenExclusive)      disp = CREATE_NEW;
    else if (flags & kOpenTruncate)  disp = CREATE_ALWAYS;
    else if (flags & kOpenCreate)    disp = OPEN_ALWAYS;
    else                             disp = OPEN_EXISTING;

    const std::wstring wpath = std::filesystem::u8path(path).wstring();
    HANDLE h = CreateFileW(wpath.c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_native_file_error = static_cast<int>(GetLastError());
        return kFileInvalid;
    }
    return static_cast<file_handle_t>(reinterpret_cast<intptr_t>(h));
#else
    int oflags = 0;
    const int acc = flags & (kOpenRead | kOpenWrite);
    if (acc == (kOpenRead | kOpenWrite)) oflags |= O_RDWR;
    else if (acc & kOpenWrite)           oflags |= O_WRONLY;
    else                                 oflags |= O_RDONLY;
    if (flags & kOpenCreate)    oflags |= O_CREAT;
    if (flags & kOpenTruncate)  oflags |= O_TRUNC;
    if (flags & kOpenAppend)    oflags |= O_APPEND;
    if (flags & kOpenExclusive) oflags |= O_CREAT | O_EXCL;

    int fd;
    do {
        fd = ::open(path, oflags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        g_native_file_error = errno;
        return kFileInvalid;
    }
    return fd;
#endif
}

// 返回读到的字节数；-1 表示失败（错误码见 native_file_last_error）。
// 读到 EOF 返回 0。
inline long long file_pread(file_handle_t h, void *buf, size_t n, long long offset) {
    g_native_file_error = 0;
#ifdef _WIN32
    const DWORD want = n > 0xFFFFFFFFull ? 0xFFFFFFFFul : static_cast<DWORD>(n);
    OVERLAPPED ov{};
    ov.hEvent     = thread_overlapped_event();
    ov.Offset     = static_cast<DWORD>(static_cast<unsigned long long>(offset) & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>((static_cast<unsigned long long>(offset) >> 32) & 0xFFFFFFFFull);
    ResetEvent(ov.hEvent);

    DWORD got = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(h), buf, want, &got, &ov)) {
        const DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) return 0;
        if (err == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(reinterpret_cast<HANDLE>(h), &ov, &got, TRUE)) {
                g_native_file_error = static_cast<int>(GetLastError());
                return -1;
            }
        } else {
            g_native_file_error = static_cast<int>(err);
            return -1;
        }
    }
    return static_cast<long long>(got);
#else
    ssize_t got;
    do {
        got = pread(static_cast<int>(h), buf, n, static_cast<off_t>(offset));
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        g_native_file_error = errno;
        return -1;
    }
    return static_cast<long long>(got);
#endif
}

inline long long file_pwrite(file_handle_t h, const void *buf, size_t n, long long offset) {
    g_native_file_error = 0;
#ifdef _WIN32
    const DWORD want = n > 0xFFFFFFFFull ? 0xFFFFFFFFul : static_cast<DWORD>(n);
    OVERLAPPED ov{};
    ov.hEvent     = thread_overlapped_event();
    ov.Offset     = static_cast<DWORD>(static_cast<unsigned long long>(offset) & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>((static_cast<unsigned long long>(offset) >> 32) & 0xFFFFFFFFull);
    ResetEvent(ov.hEvent);

    DWORD wrote = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(h), buf, want, &wrote, &ov)) {
        const DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(reinterpret_cast<HANDLE>(h), &ov, &wrote, TRUE)) {
                g_native_file_error = static_cast<int>(GetLastError());
                return -1;
            }
        } else {
            g_native_file_error = static_cast<int>(err);
            return -1;
        }
    }
    return static_cast<long long>(wrote);
#else
    ssize_t wrote;
    do {
        wrote = pwrite(static_cast<int>(h), buf, n, static_cast<off_t>(offset));
    } while (wrote < 0 && errno == EINTR);
    if (wrote < 0) {
        g_native_file_error = errno;
        return -1;
    }
    return static_cast<long long>(wrote);
#endif
}

// 文件当前大小；失败返回 false
inline bool file_size(file_handle_t h, unsigned long long *out) {
    g_native_file_error = 0;
#ifdef _WIN32
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(h), &li) || li.QuadPart < 0) {
        g_native_file_error = static_cast<int>(GetLastError());
        return false;
    }
    *out = static_cast<unsigned long long>(li.QuadPart);
    return true;
#else
    struct stat st;
    if (fstat(static_cast<int>(h), &st) != 0 || st.st_size < 0) {
        g_native_file_error = errno;
        return false;
    }
    *out = static_cast<unsigned long long>(st.st_size);
    return true;
#endif
}

inline int file_fsync(file_handle_t h) {
    g_native_file_error = 0;
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(h))) {
        g_native_file_error = static_cast<int>(GetLastError());
        return -1;
    }
    return 0;
#else
    if (fsync(static_cast<int>(h)) != 0) {
        g_native_file_error = errno;
        return -1;
    }
    return 0;
#endif
}

inline int file_truncate(file_handle_t h, unsigned long long size) {
    g_native_file_error = 0;
#ifdef _WIN32
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(reinterpret_cast<HANDLE>(h), FileEndOfFileInfo,
                                    &info, sizeof(info))) {
        g_native_file_error = static_cast<int>(GetLastError());
        return -1;
    }
    return 0;
#else
    if (ftruncate(static_cast<int>(h), static_cast<off_t>(size)) != 0) {
        g_native_file_error = errno;
        return -1;
    }
    return 0;
#endif
}

inline int file_close(file_handle_t h) {
#ifdef _WIN32
    return CloseHandle(reinterpret_cast<HANDLE>(h)) ? 0 : -1;
#else
    return ::close(static_cast<int>(h));
#endif
}

// 句柄是否带 APPEND 语义。
// Windows 上没有可查的 "append" 标志位（FILE_APPEND_DATA 不体现在文件模式里），
// 这里返回 false，调用方以 mode 解析出来的 appendMode 为准。
inline bool file_is_append(file_handle_t h) {
#ifdef _WIN32
    (void)h;
    return false;
#else
    const int fl = fcntl(static_cast<int>(h), F_GETFL);
    return fl >= 0 && (fl & O_APPEND) != 0;
#endif
}

} // namespace ayafileio::platform
