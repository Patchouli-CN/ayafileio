#ifdef _WIN32
#pragma once
#include <windows.h>
#include <atomic>

// ════════════════════════════════════════════════════════════════════════════
// §4  IOCP entry points — thin wrappers around IOCPContext singleton
// ════════════════════════════════════════════════════════════════════════════

void init_iocp();
void shutdown_iocp();
void close_all_files();
void set_iocp_worker_count(unsigned count);
BOOL WINAPI ctrl_handler(DWORD t);

#endif
