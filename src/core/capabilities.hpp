#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace ayafileio::core {

struct BackendInfo {
    std::string platform;
    std::string backend;
    std::string os_version;
    std::string description;
};

using CapabilityValue = std::variant<
    bool,
    std::string,
    std::unordered_map<std::string, bool>,
    std::nullptr_t>;

struct Capabilities : BackendInfo {
    std::unordered_map<std::string, bool> features;
    std::unordered_map<std::string, CapabilityValue> backend_detail;
};

} // namespace ayafileio::core
