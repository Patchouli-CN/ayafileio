#include "py_api.hpp"

#include "../../discover/discover.hpp"
#include <nanobind/stl/string.h>

namespace ayafileio::python {
namespace {

nb::object capability_value(const core::CapabilityValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) return nb::cast(*boolean);
    if (const auto* text = std::get_if<std::string>(&value)) return nb::cast(*text);
    if (const auto* map = std::get_if<std::unordered_map<std::string, bool>>(&value)) {
        nb::dict result;
        for (const auto& [key, item] : *map) result[key.c_str()] = nb::cast(item);
        return result;
    }
    return nb::none();
}

} // namespace

nb::dict py_get_backend_info() {
    const auto info = discovery::backend_info();
    nb::dict result;
    result["platform"] = info.platform.c_str();
    result["backend"] = info.backend.c_str();
    result["is_truly_async"] = info.is_truly_async;
    result["os_version"] = info.os_version.c_str();
    result["description"] = info.description.c_str();
    return result;
}

nb::dict py_get_capabilities() {
    const auto caps = discovery::capabilities();
    nb::dict result = py_get_backend_info();
    nb::dict detail;
    for (const auto& [key, value] : caps.backend_detail) {
        detail[key.c_str()] = capability_value(value);
    }
    result["backend_detail"] = detail;
    return result;
}

} // namespace ayafileio::python
