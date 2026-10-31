#include "py_future.hpp"

#include "py_error.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ayafileio::python {
namespace {

core::Buffer allocate_python_read_buffer(std::size_t size);

} // namespace

LoopContext::~LoopContext() {
    PyGILState_STATE state = PyGILState_Ensure();
    Py_XDECREF(create_future_);
    Py_XDECREF(loop_);
    PyGILState_Release(state);
}

std::shared_ptr<LoopContext> LoopContext::current() {
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw nb::python_error();
    PyObject* create_future = PyObject_GetAttr(loop, g_str_create_future);
    if (!create_future) {
        Py_DECREF(loop);
        throw nb::python_error();
    }
    try {
        return std::make_shared<LoopContext>(loop, create_future, get_or_create_batcher(loop));
    } catch (...) {
        Py_DECREF(create_future);
        Py_DECREF(loop);
        throw;
    }
}

PyOperation::PyOperation(
    std::shared_ptr<LoopContext> context,
    core::ReqType type,
    OpenFactory open_factory)
    : context_(std::move(context)),
      batcher_(context_->batcher()),
      type_(type),
      open_factory_(std::move(open_factory)) {
    future_ = PyObject_CallNoArgs(context_->create_future());
    if (!future_) throw nb::python_error();
    set_result_ = PyObject_GetAttr(future_, g_str_set_result);
    set_exception_ = PyObject_GetAttr(future_, g_str_set_exception);
    if (!set_result_ || !set_exception_) {
        Py_DECREF(future_);
        future_ = nullptr;
        throw nb::python_error();
    }
}

PyOperation::~PyOperation() {
    PyGILState_STATE state = PyGILState_Ensure();
    Py_XDECREF(set_result_);
    Py_XDECREF(set_exception_);
    Py_XDECREF(future_);
    PyGILState_Release(state);
}

nb::object PyOperation::future_object() {
    Py_INCREF(future_);
    return nb::steal<nb::object>(nb::handle(future_));
}

core::SchedulerContext PyOperation::scheduler_context() {
    return {batcher_, batcher_->scheduler_owner()};
}

core::Buffer PyOperation::allocate_read_buffer(std::size_t size) {
    return allocate_python_read_buffer(size);
}

void PyOperation::submitted() {
    if (!submitted_.exchange(true)) batcher_->op_submitted();
}

void PyOperation::complete(core::Completion result) {
    PyGILState_STATE state = PyGILState_Ensure();
    complete_locked(std::move(result), false);
    PyGILState_Release(state);
}

void PyOperation::complete_inline(core::Completion result) {
    PyGILState_STATE state = PyGILState_Ensure();
    complete_locked(std::move(result), true);
    PyGILState_Release(state);
}

void PyOperation::finished() {
    PyGILState_STATE state = PyGILState_Ensure();
    if (submitted_.exchange(false) && batcher_->op_completed() && batcher_->has_pending()) {
        batcher_->flush();
    }
    PyGILState_Release(state);
}

PyObject* PyOperation::convert(core::Completion& result, bool& failed) {
    try {
        if (result.failed()) {
            failed = true;
            if (result.error_object) {
                PyObject* value = static_cast<PyObject*>(result.error_object.get());
                Py_INCREF(value);
                return value;
            }
            return make_exception_object(result.error);
        }
        if (type_ == core::ReqType::Open) {
            if (open_factory_) return open_factory_(result.handle);
            return PyLong_FromSsize_t(static_cast<Py_ssize_t>(result.handle));
        }
        if (result.type == core::ReqType::Read && !result.return_count) {
            if (result.buffer.has_result() && result.value == result.buffer.size()) {
                PyObject* value = static_cast<PyObject*>(result.buffer.result_handle());
                Py_INCREF(value);
                return value;
            }
            return PyBytes_FromStringAndSize(
                result.buffer.data(), static_cast<Py_ssize_t>(result.value));
        }
        if (result.type == core::ReqType::Write || result.return_count) {
            return PyLong_FromSize_t(result.value);
        }
        Py_RETURN_NONE;
    } catch (nb::python_error& error) {
        failed = true;
        error.restore();
        return fetch_current_exc();
    } catch (const core::IoException& error) {
        failed = true;
        return make_exception_object(error.error());
    } catch (const std::exception& error) {
        failed = true;
        return make_exception_object(core::IoError::runtime(error.what()));
    }
}

void PyOperation::complete_locked(core::Completion result, bool inline_completion) {
    bool last = false;
    if (submitted_.exchange(false)) last = batcher_->op_completed();

    bool failed = result.failed();
    PyObject* value = convert(result, failed);
    PyObject* setter = failed ? set_exception_ : set_result_;
    if (std::getenv("AYAFILEIO_DEBUG_WHOLE")) {
        std::fprintf(stderr,
                     "[DEBUG-whole] convert this=%p value=%p setter=%p last=%d inline=%d\n",
                     static_cast<void*>(this), static_cast<void*>(value),
                     static_cast<void*>(setter), static_cast<int>(last),
                     static_cast<int>(inline_completion));
    }

    if (inline_completion) {
        if (setter && value) {
            PyObject* call_result = PyObject_CallFunctionObjArgs(setter, value, nullptr);
            if (!call_result) {
                if (g_InvalidStateError && PyErr_ExceptionMatches(g_InvalidStateError)) {
                    PyErr_Clear();
                } else {
                    PyErr_Print();
                }
            } else {
                Py_DECREF(call_result);
            }
        }
        Py_XDECREF(value);
        if (last && batcher_->has_pending()) batcher_->flush();
        return;
    }

    if (setter && value) {
        Py_INCREF(setter);
        const bool threshold = batcher_->push(setter, value);
        if (threshold || last) batcher_->flush();
    } else {
        Py_XDECREF(value);
    }
}

namespace {

core::Buffer allocate_python_read_buffer(std::size_t size) {
    PyObject* bytes = PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size));
    if (!bytes) return {};
    std::shared_ptr<void> owner(bytes, [](void* value) {
        PyGILState_STATE state = PyGILState_Ensure();
        Py_DECREF(value);
        PyGILState_Release(state);
    });
    return core::Buffer::external(
        PyBytes_AS_STRING(bytes), size, std::move(owner), bytes);
}

PyObject* convert_batch_read(core::Completion& result, bool& failed) {
    try {
        if (result.failed()) {
            failed = true;
            if (result.error_object) {
                PyObject* value = static_cast<PyObject*>(result.error_object.get());
                Py_INCREF(value);
                return value;
            }
            return make_exception_object(result.error);
        }
        if (result.buffer.has_result() && result.value == result.buffer.size()) {
            PyObject* value = static_cast<PyObject*>(result.buffer.result_handle());
            Py_INCREF(value);
            return value;
        }
        return PyBytes_FromStringAndSize(
            result.buffer.data(), static_cast<Py_ssize_t>(result.value));
    } catch (nb::python_error& error) {
        failed = true;
        error.restore();
        return fetch_current_exc();
    } catch (const std::exception& error) {
        failed = true;
        return make_exception_object(core::IoError::runtime(error.what()));
    }
}

} // namespace

PyBatchState::PyBatchState(std::shared_ptr<LoopContext> context, std::size_t count)
    : context_(std::move(context)),
      batcher_(context_->batcher()),
      slots_(count),
      remaining_(count) {
    future_ = PyObject_CallNoArgs(context_->create_future());
    if (!future_) throw nb::python_error();
    set_result_ = PyObject_GetAttr(future_, g_str_set_result);
    set_exception_ = PyObject_GetAttr(future_, g_str_set_exception);
    if (!set_result_ || !set_exception_) {
        Py_DECREF(future_);
        future_ = nullptr;
        throw nb::python_error();
    }
    if (remaining_ == 0) finish(true);
}

PyBatchState::~PyBatchState() {
    PyGILState_STATE state = PyGILState_Ensure();
    Py_XDECREF(set_result_);
    Py_XDECREF(set_exception_);
    Py_XDECREF(future_);
    for (auto& slot : slots_) Py_XDECREF(slot.value);
    PyGILState_Release(state);
}

nb::object PyBatchState::future_object() {
    Py_INCREF(future_);
    return nb::steal<nb::object>(nb::handle(future_));
}

core::SchedulerContext PyBatchState::scheduler_context() {
    return {batcher_, batcher_->scheduler_owner()};
}

core::Buffer PyBatchState::allocate_read_buffer(std::size_t size) {
    return allocate_python_read_buffer(size);
}

void PyBatchState::complete(
    std::size_t index, core::Completion result, bool inline_completion) {
    bool failed = result.failed();
    PyObject* value = convert_batch_read(result, failed);
    if (!value) {
        failed = true;
        value = fetch_current_exc();
    }
    bool finished = false;
    {
        std::lock_guard lock(mutex_);
        Py_XDECREF(slots_[index].value);
        slots_[index].value = value;
        slots_[index].failed = failed;
        finished = --remaining_ == 0;
    }
    if (finished) finish(inline_completion);
}

void PyBatchState::fail_from(std::size_t index, PyObject* exception) {
    bool finished = false;
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = index; i < slots_.size(); ++i) {
            Py_INCREF(exception);
            Py_XDECREF(slots_[i].value);
            slots_[i].value = exception;
            slots_[i].failed = true;
            --remaining_;
        }
        finished = remaining_ == 0;
    }
    if (finished) finish(true);
}

void PyBatchState::finish(bool inline_completion) {
    Slot* error = nullptr;
    for (auto& slot : slots_) {
        if (slot.failed) {
            error = &slot;
            break;
        }
    }

    PyObject* value = nullptr;
    PyObject* setter = set_result_;
    if (error) {
        value = error->value;
        error->value = nullptr;
        setter = set_exception_;
        for (auto& slot : slots_) {
            Py_XDECREF(slot.value);
            slot.value = nullptr;
        }
    } else {
        value = PyList_New(static_cast<Py_ssize_t>(slots_.size()));
        if (!value) {
            setter = set_exception_;
            value = fetch_current_exc();
        } else {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                PyList_SetItem(value, static_cast<Py_ssize_t>(i), slots_[i].value);
                slots_[i].value = nullptr;
            }
        }
    }

    if (!setter || !value) {
        Py_XDECREF(value);
        return;
    }
    if (inline_completion) {
        PyObject* call_result = PyObject_CallFunctionObjArgs(setter, value, nullptr);
        if (!call_result) {
            if (g_InvalidStateError && PyErr_ExceptionMatches(g_InvalidStateError)) {
                PyErr_Clear();
            } else {
                PyErr_Print();
            }
        } else {
            Py_DECREF(call_result);
        }
        Py_DECREF(value);
        return;
    }
    Py_INCREF(setter);
    batcher_->push(setter, value);
    batcher_->flush();
}

core::Buffer PyBatchOperation::allocate_read_buffer(std::size_t size) {
    return state_->allocate_read_buffer(size);
}

core::SchedulerContext PyBatchOperation::scheduler_context() {
    return state_->scheduler_context();
}

void PyBatchOperation::submitted() {
    if (!submitted_.exchange(true)) state_->batcher()->op_submitted();
}

void PyBatchOperation::complete(core::Completion result) {
    PyGILState_STATE state = PyGILState_Ensure();
    if (submitted_.exchange(false)) state_->batcher()->op_completed();
    state_->complete(index_, std::move(result), false);
    PyGILState_Release(state);
}

void PyBatchOperation::complete_inline(core::Completion result) {
    PyGILState_STATE state = PyGILState_Ensure();
    if (submitted_.exchange(false)) state_->batcher()->op_completed();
    state_->complete(index_, std::move(result), true);
    PyGILState_Release(state);
}

void PyBatchOperation::finished() {
    PyGILState_STATE state = PyGILState_Ensure();
    if (submitted_.exchange(false) && state_->batcher()->op_completed() &&
        state_->batcher()->has_pending()) {
        state_->batcher()->flush();
    }
    PyGILState_Release(state);
}

} // namespace ayafileio::python
