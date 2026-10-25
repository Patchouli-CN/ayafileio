#include "iocp.hpp"

#include "../../core/config.hpp"
#include "iocp_context.hpp"

namespace ayafileio::backend::windows {

void set_iocp_worker_count(unsigned count) {
    if (count > 128) throw core::IoException(core::IoError::value(
        "worker count must be 0 (auto) or 1-128"));
    auto cfg = core::config().get();
    cfg.io_worker_count = count;
    core::config().update(cfg);
}

void init_iocp() {
    IOCPContext::instance().init();
    static bool registered = false;
    if (!registered) {
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
        registered = true;
    }
}

void shutdown_iocp() {
    IOCPContext::instance().shutdown();
}

void close_all_files() {
    IOCPContext::instance().close_all_sessions();
}

BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        IOCPContext::instance().trigger_ctrlc();
        return FALSE;
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        IOCPContext::instance().trigger_ctrlc();
        IOCPContext::instance().close_all_sessions();
        return TRUE;
    }
    return FALSE;
}

} // namespace ayafileio::backend::windows
