#include "io_backend.hpp"
#include "globals.hpp"

void IOBackendBase::resolve_ok(PyObject *future, PyObject *val) {
    PyObject *fn = PyObject_GetAttr(future, g_str_set_result);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, val, nullptr);
    Py_XDECREF(r); Py_DECREF(fn);
}

void IOBackendBase::resolve_bytes(PyObject *future, const char *buf, Py_ssize_t n) {
    PyObject *b = PyBytes_FromStringAndSize(buf, n);
    resolve_ok(future, b); Py_DECREF(b);
}

void IOBackendBase::resolve_exc(PyObject *future, PyObject *cls, DWORD err, const char *msg) {
    PyObject *exc = err
        ? PyObject_CallFunction(cls, "is", (int)err, msg)
        : PyObject_CallFunction(cls, "s",  msg);
    PyObject *fn = PyObject_GetAttr(future, g_str_set_exception);
    PyObject *r  = PyObject_CallFunctionObjArgs(fn, exc, nullptr);
    Py_XDECREF(r); Py_DECREF(fn); Py_DECREF(exc);
}
