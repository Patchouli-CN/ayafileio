#include "file_handle.hpp"

#include "backend_registry.hpp"

namespace ayafileio::core {

FileHandle::FileHandle(const std::string& path, const std::string& mode)
    : backend_(BackendRegistry::instance().open(path, mode)) {}

FileHandle::FileHandle(int fd, const std::string& mode, bool owns_fd)
    : backend_(BackendRegistry::instance().wrap(fd, mode, owns_fd)) {}

} // namespace ayafileio::core
