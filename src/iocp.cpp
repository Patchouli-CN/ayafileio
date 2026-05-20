#include "iocp.hpp"
#include "iocp_context.hpp"
#include "config.hpp"

// ════════════════════════════════════════════════════════════════════════════
// §4  IOCP entry points — delegate to IOCPContext singleton
// ════════════════════════════════════════════════════════════════════════════

void set_iocp_worker_count(unsigned count) {
    if (count > 128) {
        throw py::value_error("worker count must be 0 (auto) or 1-128");
    }
    auto cfg = ayafileio::config().get();
    cfg.io_worker_count = count;
    ayafileio::config().update(cfg);
}

void init_iocp() {
    IOCPContext::instance().init();
    static bool ctrl_reg = false;
    if (!ctrl_reg) {
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
        ctrl_reg = true;
    }
}

void shutdown_iocp() {
    IOCPContext::instance().shutdown();
}

void close_all_files() {
    IOCPContext::instance().close_all_sessions();
}

BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT ||
        t == CTRL_CLOSE_EVENT || t == CTRL_LOGOFF_EVENT ||
        t == CTRL_SHUTDOWN_EVENT) {
        IOCPContext::instance().trigger_ctrlc();
        IOCPContext::instance().close_all_sessions();
        return TRUE;
    }
    return FALSE;
}
