#pragma once

#include "../../core/result.hpp"
#include "py_globals.hpp"

namespace ayafileio::python {

PyObject* make_exception_object(const core::IoError& error);
[[noreturn]] void raise_io_error(const core::IoError& error);
[[noreturn]] void raise_current_exception();

} // namespace ayafileio::python
