#pragma once

#include "../core/capabilities.hpp"
#include "../core/config.hpp"

#include <string>

namespace ayafileio::discovery {

std::string os_version();
core::BackendInfo backend_info();
core::Capabilities capabilities();
bool backend_available(core::BackendKind kind);

#ifdef HAVE_IO_URING
bool io_uring_available();
bool io_uring_sqpoll_supported();
std::unordered_map<std::string, core::CapabilityValue> io_uring_detail();
#endif

#ifdef __APPLE__
bool gcd_dispatch_io_available();
std::unordered_map<std::string, core::CapabilityValue> gcd_detail();
#endif

} // namespace ayafileio::discovery
