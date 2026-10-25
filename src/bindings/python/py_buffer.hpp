#pragma once

#include "../../core/buffer.hpp"
#include "py_globals.hpp"

namespace ayafileio::python {

core::Buffer make_py_buffer(PyObject* object, int flags);

} // namespace ayafileio::python
