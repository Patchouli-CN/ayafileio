#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace ayafileio::core {

enum class ReqType : uint8_t { Read, Write, Other, Open };

enum class ErrorKind : uint8_t {
    None = 0,
    Native,
    Value,
    Type,
    Overflow,
    Unsupported,
    Memory,
    Interrupted,
    Runtime,
};

struct IoError {
    ErrorKind kind{ErrorKind::None};
    uint32_t code{0};
    std::string message;

    static IoError native(uint32_t value, std::string text) {
        return {ErrorKind::Native, value, std::move(text)};
    }
    static IoError value(std::string text) {
        return {ErrorKind::Value, 0, std::move(text)};
    }
    static IoError overflow(std::string text) {
        return {ErrorKind::Overflow, 0, std::move(text)};
    }
    static IoError unsupported(std::string text) {
        return {ErrorKind::Unsupported, 0, std::move(text)};
    }
    static IoError memory(std::string text) {
        return {ErrorKind::Memory, 0, std::move(text)};
    }
    static IoError interrupted(std::string text) {
        return {ErrorKind::Interrupted, 0, std::move(text)};
    }
    static IoError runtime(std::string text) {
        return {ErrorKind::Runtime, 0, std::move(text)};
    }
};

class IoException : public std::exception {
public:
    explicit IoException(IoError error) : error_(std::move(error)) {}

    const char* what() const noexcept override { return error_.message.c_str(); }
    const IoError& error() const noexcept { return error_; }

private:
    IoError error_;
};

} // namespace ayafileio::core
