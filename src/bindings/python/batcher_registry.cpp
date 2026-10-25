#include "batcher_registry.hpp"

#include "../../core/config.hpp"

#include <algorithm>
#include <limits>
#include <memory>

namespace ayafileio::python {

BatcherRegistry& BatcherRegistry::instance() {
    static BatcherRegistry registry;
    return registry;
}

ResultBatcher* BatcherRegistry::find_live_locked(PyObject* loop) {
    for (auto& entry : active_) {
        if (entry.first != loop || !entry.second) continue;
        if (entry.second->loop_is(loop)) return entry.second;
        retired_.push_back(entry.second);
        entry.first = nullptr;
        entry.second = nullptr;
    }
    return nullptr;
}

ResultBatcher* BatcherRegistry::get_or_create(PyObject* loop) {
    {
        std::lock_guard lock(mutex_);
        if (auto* hit = find_live_locked(loop)) return hit;
    }

    auto& cfg = core::config();
    std::unique_ptr<ResultBatcher> candidate(
        new ResultBatcher(loop, cfg.iocp_batch_size(), 5));
    candidate->set_adaptive(cfg.adaptive_batch());
    candidate->set_target_latency_us(cfg.adaptive_target_latency_us());

    ResultBatcher* result = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (auto* hit = find_live_locked(loop)) {
            result = hit;
        } else {
            active_.emplace_back(loop, candidate.get());
            result = candidate.release();
        }
        active_.erase(
            std::remove_if(active_.begin(), active_.end(),
                           [](const auto& entry) { return entry.second == nullptr; }),
            active_.end());
    }
    return result;
}

std::vector<ResultBatcher*> BatcherRegistry::snapshot() {
    std::lock_guard lock(mutex_);
    std::vector<ResultBatcher*> result;
    result.reserve(active_.size());
    for (const auto& entry : active_) result.push_back(entry.second);
    return result;
}

void BatcherRegistry::clear() {
    std::vector<ResultBatcher*> victims;
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : active_) {
            if (entry.second) victims.push_back(entry.second);
        }
        victims.insert(victims.end(), retired_.begin(), retired_.end());
        active_.clear();
        retired_.clear();
    }
    for (auto* batcher : victims) delete batcher;
}

void BatcherRegistry::refresh_config() {
    auto& cfg = core::config();
    for (auto* batcher : snapshot()) {
        batcher->set_threshold_max(cfg.iocp_batch_size());
        batcher->set_adaptive(cfg.adaptive_batch());
        batcher->set_target_latency_us(cfg.adaptive_target_latency_us());
    }
}

uint32_t BatcherRegistry::next_timeout_ms() {
    uint32_t result = std::numeric_limits<uint32_t>::max();
    for (auto* batcher : snapshot()) {
        const auto timeout = batcher->get_timeout_ms();
        if (timeout < result) result = timeout;
    }
    return result;
}

void BatcherRegistry::flush_idle() {
    PyGILState_STATE state = PyGILState_Ensure();
    for (auto* batcher : snapshot()) batcher->flush_if_idle();
    PyGILState_Release(state);
}

ResultBatcher* get_or_create_batcher(PyObject* loop) {
    return BatcherRegistry::instance().get_or_create(loop);
}

void clear_batchers() {
    BatcherRegistry::instance().clear();
}

void refresh_batchers_config() {
    BatcherRegistry::instance().refresh_config();
}

} // namespace ayafileio::python
