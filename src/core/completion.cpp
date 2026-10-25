#include "completion.hpp"

#include <atomic>

namespace ayafileio::core {
namespace {
std::atomic<CompletionScheduler*> g_scheduler{nullptr};

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

} // namespace ayafileio::core
