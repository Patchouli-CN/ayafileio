#pragma once

#include "config.hpp"
#include "io_backend.hpp"

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayafileio::core {

struct BackendEntry {
    using OpenFactory = std::function<std::unique_ptr<IOBackendBase>(
        const std::string&, const std::string&)>;
    using WrapFactory = std::function<std::unique_ptr<IOBackendBase>(
        int, const std::string&, bool)>;
    using Availability = std::function<bool()>;

    OpenFactory open;
    WrapFactory wrap;
    Availability available{[] { return true; }};
};

class BackendRegistry {
public:
    static BackendRegistry& instance();

    void register_backend(BackendKind kind, BackendEntry entry);
    void set_auto_order(std::vector<BackendKind> order);
    void clear();

    bool available(BackendKind kind) const;
    BackendKind effective_backend() const;
    std::unique_ptr<IOBackendBase> open(const std::string& path, const std::string& mode);
    std::unique_ptr<IOBackendBase> wrap(int fd, const std::string& mode, bool owns_fd);

private:
    BackendEntry entry(BackendKind kind) const;
    std::unique_ptr<IOBackendBase> create(
        BackendKind kind,
        const std::string& path,
        const std::string& mode);
    std::unique_ptr<IOBackendBase> create_wrapped(
        BackendKind kind,
        int fd,
        const std::string& mode,
        bool owns_fd);

    mutable std::shared_mutex mutex_;
    std::unordered_map<BackendKind, BackendEntry> entries_;
    std::vector<BackendKind> auto_order_{BackendKind::ThreadPool};
};

const char* backend_name(BackendKind kind);
bool parse_backend_name(const std::string& name, BackendKind& out);

} // namespace ayafileio::core
