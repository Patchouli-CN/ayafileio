#pragma once

#include "py_globals.hpp"

namespace ayafileio::python {

nb::object py_open_async(const char* path, const char* mode);
uint64_t native_aopen_submission_count();

void py_configure(nb::dict options);
nb::dict py_get_config();
void py_reset_config();
void py_set_worker_count(int count);
#ifdef _WIN32
void py_set_iocp_worker_count(int count);
nb::dict py_debug_quarantine_stats();
#endif

nb::dict py_get_backend_info();
nb::dict py_get_capabilities();

long py_debug_batcher_outstanding();
nb::dict py_debug_batcher_state();
unsigned py_debug_pool_workers();

} // namespace ayafileio::python
