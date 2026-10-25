#pragma once

#include "../../core/completion.hpp"
#include "batcher_registry.hpp"
#include "py_globals.hpp"

#include <functional>
#include <memory>
#include <string>

namespace ayafileio::python {

class LoopContext {
public:
    LoopContext(PyObject* loop, PyObject* create_future, ResultBatcher* batcher)
        : loop_(loop), create_future_(create_future), batcher_(batcher) {}
    ~LoopContext();

    static std::shared_ptr<LoopContext> current();

    PyObject* create_future() const { return create_future_; }
    PyObject* identity() const { return loop_; }
    ResultBatcher* batcher() const { return batcher_; }

private:
    PyObject* loop_;
    PyObject* create_future_;
    ResultBatcher* batcher_;
};

class PyOperation final : public core::Operation {
public:
    using OpenFactory = std::function<PyObject*(std::intptr_t)>;

    PyOperation(std::shared_ptr<LoopContext> context, core::ReqType type,
                OpenFactory open_factory = {});
    ~PyOperation() override;

    core::Buffer allocate_read_buffer(std::size_t size) override;
    core::SchedulerContext scheduler_context() override;
    void submitted() override;
    void complete(core::Completion result) override;
    void complete_inline(core::Completion result) override;
    void finished() override;

    nb::object future_object();

private:
    void complete_locked(core::Completion result, bool inline_completion);
    PyObject* convert(core::Completion& result, bool& failed);

    std::shared_ptr<LoopContext> context_;
    ResultBatcher* batcher_;
    core::ReqType type_;
    OpenFactory open_factory_;
    PyObject* future_{nullptr};
    PyObject* set_result_{nullptr};
    PyObject* set_exception_{nullptr};
    std::atomic<bool> submitted_{false};
};

} // namespace ayafileio::python
