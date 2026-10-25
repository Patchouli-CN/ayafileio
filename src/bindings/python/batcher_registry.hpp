#pragma once

#include "result_batcher.hpp"
#include "../../core/completion.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace ayafileio::python {

class BatcherRegistry final : public core::CompletionScheduler {
public:
    static BatcherRegistry& instance();

    ResultBatcher* get_or_create(PyObject* loop);
    std::vector<ResultBatcher*> snapshot();
    void clear();
    void refresh_config();

    uint32_t next_timeout_ms() override;
    void flush_idle() override;

private:
    ResultBatcher* find_live_locked(PyObject* loop);

    std::mutex mutex_;
    std::vector<std::pair<PyObject*, ResultBatcher*>> active_;
    std::vector<ResultBatcher*> retired_;
};

ResultBatcher* get_or_create_batcher(PyObject* loop);
void clear_batchers();
void refresh_batchers_config();

} // namespace ayafileio::python
