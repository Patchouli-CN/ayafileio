#include "py_buffer.hpp"

#include <memory>

namespace ayafileio::python {
namespace {

struct PyBufferOwner {
    PyObject* object;
    Py_buffer view;

    PyBufferOwner(PyObject* object, Py_buffer view) : object(object), view(view) {
        Py_INCREF(object);
    }

    ~PyBufferOwner() {
        PyGILState_STATE state = PyGILState_Ensure();
        if (view.buf) PyBuffer_Release(&view);
        Py_DECREF(object);
        PyGILState_Release(state);
    }
};

} // namespace

core::Buffer make_py_buffer(PyObject* object, int flags) {
    Py_buffer view;
    if (PyObject_GetBuffer(object, &view, flags) < 0) throw nb::python_error();
    auto owner = std::make_shared<PyBufferOwner>(object, view);
    return core::Buffer::external(
        static_cast<char*>(view.buf), static_cast<std::size_t>(view.len), std::move(owner));
}

} // namespace ayafileio::python
