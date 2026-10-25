#pragma once

#include <windows.h>

namespace ayafileio::backend::windows {

void init_iocp();
void shutdown_iocp();
void close_all_files();
void set_iocp_worker_count(unsigned count);
BOOL WINAPI ctrl_handler(DWORD type);

} // namespace ayafileio::backend::windows
