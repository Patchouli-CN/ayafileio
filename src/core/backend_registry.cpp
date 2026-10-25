#include "backend_registry.hpp"

#include <utility>

namespace ayafileio::core {

BackendRegistry& BackendRegistry::instance() {
    static BackendRegistry registry;
    return registry;
}

void BackendRegistry::register_backend(BackendKind kind, BackendEntry entry) {
    std::unique_lock lock(mutex_);
    entries_[kind] = std::move(entry);
}

void BackendRegistry::set_auto_order(std::vector<BackendKind> order) {
    std::unique_lock lock(mutex_);
    auto_order_ = std::move(order);
}

void BackendRegistry::clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
    auto_order_ = {BackendKind::ThreadPool};
}

bool BackendRegistry::available(BackendKind kind) const {
    if (kind == BackendKind::Auto) return true;
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(kind);
    return it != entries_.end() && it->second.available();
}

BackendKind BackendRegistry::effective_backend() const {
    const BackendKind configured = config().backend();
    if (configured != BackendKind::Auto && available(configured)) return configured;
    std::shared_lock lock(mutex_);
    for (const auto kind : auto_order_) {
        const auto it = entries_.find(kind);
        if (it != entries_.end() && it->second.available()) return kind;
    }
    return BackendKind::ThreadPool;
}

BackendEntry BackendRegistry::entry(BackendKind kind) const {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(kind);
    if (it == entries_.end()) {
        throw IoException(IoError::runtime("Backend is not registered"));
    }
    return it->second;
}

std::unique_ptr<IOBackendBase> BackendRegistry::create(
    BackendKind kind,
    const std::string& path,
    const std::string& mode) {
    auto selected = entry(kind);
    return selected.open(path, mode);
}

std::unique_ptr<IOBackendBase> BackendRegistry::create_wrapped(
    BackendKind kind,
    int fd,
    const std::string& mode,
    bool owns_fd) {
    auto selected = entry(kind);
    return selected.wrap(fd, mode, owns_fd);
}

std::unique_ptr<IOBackendBase> BackendRegistry::open(
    const std::string& path,
    const std::string& mode) {
    const auto kind = effective_backend();
    try {
        return create(kind, path, mode);
    } catch (const std::exception&) {
        if (kind == BackendKind::ThreadPool) throw;
        return create(BackendKind::ThreadPool, path, mode);
    }
}

std::unique_ptr<IOBackendBase> BackendRegistry::wrap(
    int fd,
    const std::string& mode,
    bool owns_fd) {
#ifdef _WIN32
    const auto kind = BackendKind::Iocp;
#else
    const auto kind = effective_backend();
#endif
    try {
        return create_wrapped(kind, fd, mode, owns_fd);
    } catch (const std::exception&) {
        if (kind == BackendKind::ThreadPool) throw;
        return create_wrapped(BackendKind::ThreadPool, fd, mode, owns_fd);
    }
}

const char* backend_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::Iocp: return "iocp";
        case BackendKind::IoUring: return "io_uring";
        case BackendKind::DispatchIo: return "dispatch_io";
        case BackendKind::ThreadPool: return "thread_pool";
        case BackendKind::Auto: return "auto";
    }
    return "auto";
}

bool parse_backend_name(const std::string& name, BackendKind& out) {
    if (name == "auto") out = BackendKind::Auto;
    else if (name == "iocp") out = BackendKind::Iocp;
    else if (name == "io_uring") out = BackendKind::IoUring;
    else if (name == "dispatch_io") out = BackendKind::DispatchIo;
    else if (name == "thread_pool") out = BackendKind::ThreadPool;
    else return false;
    return true;
}

} // namespace ayafileio::core
