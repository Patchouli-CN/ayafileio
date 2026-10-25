#include "discover.hpp"

#ifndef _WIN32

#include <sys/utsname.h>

namespace ayafileio::discovery {

std::string os_version() {
    utsname info{};
    if (uname(&info) != 0) return "unknown";
    return std::string(info.sysname) + " " + info.release;
}

} // namespace ayafileio::discovery

#endif
