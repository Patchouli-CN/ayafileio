#include "backend_registry.hpp"
#include "buffer_pool.hpp"
#include "file_state.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <optional>

int main() {
    using namespace ayafileio::core;

    FileState state;
    state.set_cached_size(100);
    const auto first = state.plan_read(20, [] { return std::optional<uint64_t>(100); });
    assert(!first.eof && first.offset == 0 && first.size == 20);
    const auto second = state.plan_read(-1, [] { return std::optional<uint64_t>(100); });
    assert(!second.eof && second.offset == 20 && second.size == 80);
    const auto eof = state.plan_read(0, [] { return std::optional<uint64_t>(100); });
    assert(eof.eof && eof.size == 0);
    const auto end = state.seek(0, 2, [] { return std::optional<uint64_t>(100); });
    assert(end && *end == 100);
    const auto negative = state.seek(-1, 0, [] { return std::optional<uint64_t>(100); });
    assert(!negative);

    auto& registry = BackendRegistry::instance();
    registry.clear();
    registry.register_backend(
        BackendKind::ThreadPool,
        BackendEntry{
            [](const std::string&, const std::string&) -> std::unique_ptr<IOBackendBase> {
                return nullptr;
            },
            [](int, const std::string&, bool) -> std::unique_ptr<IOBackendBase> {
                return nullptr;
            },
        });
    registry.set_auto_order({BackendKind::ThreadPool});
    assert(registry.available(BackendKind::ThreadPool));
    assert(registry.effective_backend() == BackendKind::ThreadPool);
    assert(registry.open("dummy", "rb") == nullptr);
    registry.clear();

    BackendKind parsed = BackendKind::Auto;
    assert(parse_backend_name("thread_pool", parsed));
    assert(parsed == BackendKind::ThreadPool);

    auto* buffer = pool_acquire_with_size(5000);
    assert(buffer != nullptr && buffer->size >= 5000);
    pool_release(buffer);
    assert(BufferPool::instance().total_buffers() > 0);
    pool_clear();
    assert(BufferPool::instance().total_buffers() == 0);

    std::cout << "core tests passed\n";
    return 0;
}
