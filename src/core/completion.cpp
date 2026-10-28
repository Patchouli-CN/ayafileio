#include "completion.hpp"

#include <atomic>

namespace ayafileio::core {
namespace {
std::atomic<CompletionScheduler*> g_scheduler{nullptr};
std::atomic<WorkerThreadHook> g_thread_attach{nullptr};
std::atomic<WorkerThreadHook> g_thread_detach{nullptr};

class NoopOperation final : public Operation {
public:
    void submitted() override {}
    void complete(Completion) override {}
    void complete_inline(Completion) override {}
    void finished() override {}
};

}

void set_completion_scheduler(CompletionScheduler* scheduler) {
    g_scheduler.store(scheduler, std::memory_order_release);
}

CompletionScheduler* completion_scheduler() {
    return g_scheduler.load(std::memory_order_acquire);
}

std::shared_ptr<Operation> make_noop_operation() {
    static std::shared_ptr<Operation> operation = std::make_shared<NoopOperation>();
    return operation;
}

void set_worker_thread_hooks(WorkerThreadHook attach, WorkerThreadHook detach) {
    g_thread_attach.store(attach, std::memory_order_release);
    g_thread_detach.store(detach, std::memory_order_release);
}

void worker_thread_attach() {
    if (auto hook = g_thread_attach.load(std::memory_order_acquire)) hook();
}

void worker_thread_detach() {
    if (auto hook = g_thread_detach.load(std::memory_order_acquire)) hook();
}

} // namespace ayafileio::core
