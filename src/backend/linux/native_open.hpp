#pragma once
#ifdef HAVE_IO_URING

#include "../../core/io_backend.hpp"

#include <cstdint>
#include <string>

namespace ayafileio::backend::linux {

bool submit_native_aopen(
    const std::string& path,
    const std::string& mode,
    core::OperationPtr operation);
void complete_native_aopen(core::IORequest* request, int result, bool inline_completion);
uint64_t native_aopen_submission_count();

} // namespace ayafileio::backend::linux

#endif
