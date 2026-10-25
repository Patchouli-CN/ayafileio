#include "py_async_file.hpp"
#include "py_error.hpp"
#include "../../backend/builtin.hpp"
#include "../../core/config.hpp"
#include "../../core/global_thread_pool.hpp"

namespace ayafileio::python {
namespace {

PyObject* cast_file(PyAsyncFile* file) {
    auto object = nb::cast(file, nb::rv_policy::take_ownership);
    PyObject* result = object.ptr();
    Py_INCREF(result);
    return result;
}

} // namespace

nb::object py_open_async(const char* path, const char* mode) {
    auto context = LoopContext::current();
    const std::string path_copy(path);
    const std::string mode_copy(mode);

    auto native_operation = std::make_shared<PyOperation>(
        context,
        core::ReqType::Open,
        [mode_copy](std::intptr_t fd) -> PyObject* {
            return cast_file(new PyAsyncFile(
                static_cast<int>(fd), mode_copy.c_str(), true));
        });
    native_operation->submitted();
    if (backend::try_native_aopen(path_copy, mode_copy, native_operation)) {
        return native_operation->future_object();
    }
    native_operation->finished();

    auto operation = std::make_shared<PyOperation>(
        context,
        core::ReqType::Open,
        [](std::intptr_t object) -> PyObject* {
            return reinterpret_cast<PyObject*>(object);
        });
    operation->submitted();
    auto& pool = core::GlobalThreadPool::instance();
    pool.ensure_started(core::effective_io_worker_count());
    pool.enqueue([operation, path_copy, mode_copy]() {
        PyGILState_STATE state = PyGILState_Ensure();
        try {
            auto handle = [&] {
                GilRelease release;
                return std::make_unique<core::FileHandle>(path_copy, mode_copy);
            }();
            PyObject* object = cast_file(new PyAsyncFile(std::move(handle)));
            operation->complete(core::Completion::success(
                core::ReqType::Open, 0, reinterpret_cast<std::intptr_t>(object)));
        } catch (const core::IoException& error) {
            operation->complete(core::Completion::failure(
                core::ReqType::Open, error.error()));
        } catch (nb::python_error& error) {
            const std::string message = error.what();
            error.restore();
            PyObject* exception = fetch_current_exc();
            std::shared_ptr<void> owner(exception, [](void* value) {
                PyGILState_STATE state = PyGILState_Ensure();
                Py_XDECREF(value);
                PyGILState_Release(state);
            });
            operation->complete(core::Completion::failure(
                core::ReqType::Open, core::IoError::runtime(message), std::move(owner)));
        } catch (const std::exception& error) {
            operation->complete(core::Completion::failure(
                core::ReqType::Open, core::IoError::runtime(error.what())));
        }
        PyGILState_Release(state);
    });
    return operation->future_object();
}

uint64_t native_aopen_submission_count() {
    return backend::native_aopen_submission_count();
}

} // namespace ayafileio::python
