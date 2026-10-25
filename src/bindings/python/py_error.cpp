#include "py_error.hpp"

namespace ayafileio::python {

PyObject* make_exception_object(const core::IoError& error) {
    const char* message = error.message.c_str();
    switch (error.kind) {
        case core::ErrorKind::Native: {
#ifdef _WIN32
            PyObject* cls = map_win_error(error.code);
#else
            PyObject* cls = map_posix_error(static_cast<int>(error.code));
#endif
            return PyObject_CallFunction(cls, "is", static_cast<int>(error.code), message);
        }
        case core::ErrorKind::Value:
            return PyObject_CallFunction(PyExc_ValueError, "s", message);
        case core::ErrorKind::Type:
            return PyObject_CallFunction(PyExc_TypeError, "s", message);
        case core::ErrorKind::Overflow:
            return PyObject_CallFunction(PyExc_OverflowError, "s", message);
        case core::ErrorKind::Unsupported:
            return PyObject_CallFunction(g_UnsupportedOperation, "s", message);
        case core::ErrorKind::Memory:
            return PyObject_CallNoArgs(PyExc_MemoryError);
        case core::ErrorKind::Interrupted:
            return PyObject_CallFunction(g_KeyboardInterrupt, "s", message);
        case core::ErrorKind::Runtime:
            return PyObject_CallFunction(PyExc_RuntimeError, "s", message);
        case core::ErrorKind::None:
            return PyObject_CallFunction(g_OSError, "s", "Unknown native error");
    }
    return PyObject_CallFunction(g_OSError, "s", message);
}

[[noreturn]] void raise_io_error(const core::IoError& error) {
    PyObject* exception = make_exception_object(error);
    if (!exception) throw nb::python_error();
    PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(exception)), exception);
    Py_DECREF(exception);
    throw nb::python_error();
}

[[noreturn]] void raise_current_exception() {
    try {
        throw;
    } catch (const core::IoException& error) {
        raise_io_error(error.error());
    } catch (const nb::python_error&) {
        throw;
    } catch (const nb::builtin_exception& error) {
        PyErr_SetString(g_ValueError, error.what());
        throw nb::python_error();
    } catch (const std::invalid_argument& error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        throw nb::python_error();
    } catch (const std::bad_alloc&) {
        PyErr_SetString(PyExc_MemoryError, "Native allocation failed");
        throw nb::python_error();
    } catch (const std::exception& error) {
        PyErr_SetString(g_OSError, error.what());
        throw nb::python_error();
    }
}

} // namespace ayafileio::python
