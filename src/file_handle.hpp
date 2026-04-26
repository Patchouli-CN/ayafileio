#pragma once
#include "globals.hpp"
#include "io_backend.hpp"
#include <string>

// ════════════════════════════════════════════════════════════════════════════
// §6  FileHandle
// ════════════════════════════════════════════════════════════════════════════

class FileHandle {
public:
    FileHandle(const std::string& path, const std::string& mode);
    FileHandle(int fd, const std::string& mode, bool owns_fd = false);  // ← 新增
    ~FileHandle();

    PyObject* read(int64_t size = -1) { return m_backend->read(size); }
    PyObject* write(Py_buffer* view) { return m_backend->write(view); }
    PyObject* seek(int64_t offset, int whence = 0) { return m_backend->seek(offset, whence); }
    PyObject* flush() { return m_backend->flush(); }
    PyObject* close() { return m_backend->close(); }
    void      close_impl() { m_backend->close_impl(); }

    void complete_ok(IORequest* req, DWORD bytes) { m_backend->complete_ok(req, bytes); }
    void complete_error(IORequest* req, DWORD err) { m_backend->complete_error(req, err); }

private:
    IOBackendBase* m_backend = nullptr;
};
