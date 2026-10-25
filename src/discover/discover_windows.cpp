#include "discover.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cstdio>

namespace ayafileio::discovery {

std::string os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (fn(&info) == 0) {
                char buffer[48];
                snprintf(
                    buffer,
                    sizeof(buffer),
                    "%lu.%lu.%lu",
                    static_cast<unsigned long>(info.dwMajorVersion),
                    static_cast<unsigned long>(info.dwMinorVersion),
                    static_cast<unsigned long>(info.dwBuildNumber));
                return buffer;
            }
        }
    }
    return "unknown";
}

} // namespace ayafileio::discovery

#endif
