#pragma once
#include <string>
#include <stdexcept>

namespace ayafileio {

struct ModeInfo {
    bool canRead = false;
    bool canWrite = false;
    bool appendMode = false;
    bool plus = false;
    bool hasR = false;
    bool hasW = false;
    bool hasA = false;
    bool hasX = false;
};

inline ModeInfo parse_mode(const std::string &mode) {
    ModeInfo mi;
    mi.plus = mode.find('+') != std::string::npos;
    mi.hasR = mode.find('r') != std::string::npos;
    mi.hasW = mode.find('w') != std::string::npos;
    mi.hasA = mode.find('a') != std::string::npos;
    mi.hasX = mode.find('x') != std::string::npos;

    if (mi.hasX && mi.hasW) throw std::invalid_argument("Invalid mode: cannot combine x and w");
    if (!mi.hasR && !mi.hasW && !mi.hasA && !mi.hasX) throw std::invalid_argument("Invalid mode: missing mode character");

    if (mi.hasR) mi.canRead = true;
    if (mi.hasW) mi.canWrite = true;
    if (mi.hasA) { mi.canWrite = true; mi.appendMode = true; }
    if (mi.plus) { mi.canRead = true; mi.canWrite = true; }

    return mi;
}

} // namespace ayafileio
