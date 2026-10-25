#pragma once

#include "../core/io_backend.hpp"

#include <cstdint>
#include <string>

namespace ayafileio::backend {

void register_builtin_backends();
void shutdown_builtin_backends();

bool try_native_aopen(
    const std::string& path,
    const std::string& mode,
    core::OperationPtr operation);
uint64_t native_aopen_submission_count();

} // namespace ayafileio::backend
