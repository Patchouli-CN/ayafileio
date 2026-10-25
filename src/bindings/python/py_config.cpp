#include "py_api.hpp"

#include "batcher_registry.hpp"
#include "../../backend/builtin.hpp"
#include "../../core/backend_registry.hpp"
#include "../../core/buffer_pool.hpp"
#include "../../core/global_thread_pool.hpp"
#include "../../discover/discover.hpp"
#ifdef _WIN32
#include "../../backend/windows/iocp.hpp"
#endif
#include <nanobind/stl/string.h>

namespace ayafileio::python {

void py_configure(nb::dict options) {
    auto cfg = core::config().get();
    if (options.contains("backend")) {
        const auto name = nb::cast<std::string>(options["backend"]);
        core::BackendKind kind = core::BackendKind::Auto;
        if (!core::parse_backend_name(name, kind)) {
            throw nb::value_error(
                "backend must be one of: 'auto', 'iocp', 'io_uring', 'dispatch_io', 'thread_pool'");
        }
        if (kind != core::BackendKind::Auto && !core::BackendRegistry::instance().available(kind)) {
            throw nb::value_error(("backend '" + name + "' is not available on this platform").c_str());
        }
        cfg.backend = kind;
    }
    if (options.contains("io_worker_count")) {
        const auto value = nb::cast<unsigned>(options["io_worker_count"]);
        if (value > 128) throw nb::value_error("io_worker_count must be 0-128");
        cfg.io_worker_count = value;
    }
    if (options.contains("buffer_pool_max")) cfg.buffer_pool_max = nb::cast<std::size_t>(options["buffer_pool_max"]);
    if (options.contains("buffer_size")) cfg.buffer_size = nb::cast<std::size_t>(options["buffer_size"]);
    if (options.contains("close_timeout_ms")) cfg.close_timeout_ms = nb::cast<unsigned>(options["close_timeout_ms"]);
    if (options.contains("io_uring_queue_depth")) cfg.io_uring_queue_depth = nb::cast<unsigned>(options["io_uring_queue_depth"]);
    if (options.contains("iocp_batch_size")) {
        const auto value = nb::cast<unsigned>(options["iocp_batch_size"]);
        if (value < 1 || value > 256) throw nb::value_error("iocp_batch_size must be 1-256");
        cfg.iocp_batch_size = value;
    }
    if (options.contains("io_uring_sqpoll")) {
        const bool value = nb::cast<bool>(options["io_uring_sqpoll"]);
        if (value) {
            const auto want = cfg.backend == core::BackendKind::Auto
                ? core::BackendRegistry::instance().effective_backend()
                : cfg.backend;
            if (want != core::BackendKind::IoUring) {
                throw nb::value_error("io_uring_sqpoll=True requires the io_uring backend");
            }
#ifdef HAVE_IO_URING
            if (!discovery::io_uring_available() || !discovery::io_uring_sqpoll_supported()) {
                throw nb::value_error("io_uring_sqpoll=True is unavailable on this system");
            }
#else
            throw nb::value_error("io_uring_sqpoll is only meaningful on the Linux io_uring backend");
#endif
        }
        cfg.io_uring_sqpoll = value;
    }
    if (options.contains("adaptive_batch")) cfg.adaptive_batch = nb::cast<bool>(options["adaptive_batch"]);
    if (options.contains("adaptive_target_latency_us")) {
        const auto value = nb::cast<unsigned>(options["adaptive_target_latency_us"]);
        if (value < 1 || value > 10000) {
            throw nb::value_error("adaptive_target_latency_us must be 1-10000");
        }
        cfg.adaptive_target_latency_us = value;
    }

    core::config().update(cfg);
    core::BufferPool::instance().refresh_config();
    refresh_batchers_config();
    core::GlobalThreadPool::instance().target_workers(core::effective_io_worker_count());
    g_worker_count.store(cfg.io_worker_count);
}

nb::dict py_get_config() {
    const auto cfg = core::config().get();
    nb::dict result;
    result["backend"] = core::backend_name(cfg.backend);
    result["io_worker_count"] = cfg.io_worker_count;
    result["buffer_pool_max"] = cfg.buffer_pool_max;
    result["buffer_size"] = cfg.buffer_size;
    result["close_timeout_ms"] = cfg.close_timeout_ms;
    result["iocp_batch_size"] = cfg.iocp_batch_size;
    result["io_uring_queue_depth"] = cfg.io_uring_queue_depth;
    result["io_uring_sqpoll"] = cfg.io_uring_sqpoll;
    result["adaptive_batch"] = cfg.adaptive_batch;
    result["adaptive_target_latency_us"] = cfg.adaptive_target_latency_us;
    return result;
}

void py_reset_config() {
    core::config().update(core::Config::defaults());
    core::BufferPool::instance().refresh_config();
    refresh_batchers_config();
    core::GlobalThreadPool::instance().target_workers(core::effective_io_worker_count());
    g_worker_count.store(0);
}

void py_set_worker_count(int count) {
    set_worker_count(static_cast<unsigned>(count));
}

#ifdef _WIN32
void py_set_iocp_worker_count(int count) {
    backend::windows::set_iocp_worker_count(static_cast<unsigned>(count));
}
#endif

} // namespace ayafileio::python
