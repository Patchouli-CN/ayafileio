
#include "iocp.hpp"
#include "io_backend.hpp"
#include "io_request.hpp"
#include "config.hpp"

HANDLE                           g_iocp           = NULL;
std::atomic<bool>                g_iocpRunning{false};
std::atomic<bool>                g_ctrlcTriggered{false};
std::vector<std::thread>         g_iocpWorkers;
std::mutex                       g_openFilesMtx;
std::unordered_set<IOBackendBase*>  g_openFiles;
std::atomic<unsigned>            g_iocp_worker_count{0};

// ════════════════════════════════════════════════════════════════════════════
// §4  IOCP init / shutdown
// ════════════════════════════════════════════════════════════════════════════

void set_iocp_worker_count(unsigned count) {
if (count == 0 || (count >= 1 && count <= 128)) {
        g_iocp_worker_count.store(count);
    } else {
        throw py::value_error("worker count must be 0 (auto) or 1-128");
    }
}

void init_iocp() {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    // 优化：允许更多工作线程以处理高并发
    // 原来：min(CPU核心数, 4)
    // 现在：min(CPU核心数 * 2, 16)，但至少1个
    unsigned n = g_iocp_worker_count.load();
    if (n == 0) {
        // 自动模式：CPU*2，上限 16
        n = std::max(1u, std::min((unsigned)si.dwNumberOfProcessors * 2, 16u));
    }
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!g_iocp) throw std::runtime_error("Failed to create IOCP");
    g_iocpRunning.store(true, std::memory_order_release);
    g_iocpWorkers.reserve(n);
    for (unsigned i = 0; i < n; ++i) g_iocpWorkers.emplace_back(iocp_thread_proc);
}

void shutdown_iocp() {
    if (!g_iocp) return;
    g_iocpRunning.store(false, std::memory_order_release);
    for (size_t i = 0; i < g_iocpWorkers.size(); ++i)
        PostQueuedCompletionStatus(g_iocp, 0, 0, NULL);
    for (auto &t : g_iocpWorkers) if (t.joinable()) t.join();
    g_iocpWorkers.clear();
    CloseHandle(g_iocp); g_iocp = NULL;
}

BOOL WINAPI ctrl_handler(DWORD t) {
    if (t==CTRL_C_EVENT||t==CTRL_BREAK_EVENT||t==CTRL_CLOSE_EVENT||
        t==CTRL_LOGOFF_EVENT||t==CTRL_SHUTDOWN_EVENT) {
        g_ctrlcTriggered.store(true, std::memory_order_relaxed);
        close_all_files(); return TRUE;
    }
    return FALSE;
}

// ════════════════════════════════════════════════════════════════════════════
// §7  IOCP worker + close_all
// ════════════════════════════════════════════════════════════════════════════

void iocp_thread_proc() {
    OVERLAPPED_ENTRY entry{};
    OVERLAPPED_ENTRY batch[255]{};
    unsigned batch_count = ayafileio::config().iocp_batch_size() - 1;
    if (batch_count > 255) batch_count = 255;

    auto process = [](OVERLAPPED_ENTRY& e) {
        if (!e.lpOverlapped) return;
        auto* req = reinterpret_cast<IORequest*>(e.lpOverlapped);
        auto* file = req->file;
        if (!file) { delete req; return; }
        if (e.Internal == 0) {
            file->complete_ok(req, e.dwNumberOfBytesTransferred);
        } else {
            file->complete_error(req, GetLastError());
        }
    };

    while (true) {
        ULONG count = 0;
        GetQueuedCompletionStatusEx(
            g_iocp, &entry, 1, &count, INFINITE, FALSE);
        if (count == 0 || !entry.lpOverlapped) {
            if (!g_iocpRunning.load(std::memory_order_acquire)) break;
            continue;
        }
        process(entry);

        ULONG more = 0;
        GetQueuedCompletionStatusEx(
            g_iocp, batch, batch_count, &more, 0, FALSE);
        for (ULONG i = 0; i < more; ++i) process(batch[i]);
    }
}

void close_all_files() {
    std::vector<IOBackendBase*> snap;
    { std::lock_guard<std::mutex> lk(g_openFilesMtx); snap.assign(g_openFiles.begin(), g_openFiles.end()); }
    for (auto *f : snap) try { f->close_impl(); } catch (...) {}
}
