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

#include <string>
#include <cstdint>
#include <string_view>

namespace ayafileio::python {
namespace {

// configure() 接受的 key 白名单。
//
// 没有这道检查时，拼错一个字母会变成"设置成功"的假象：返回 None、不抛异常、配置
// 原封不动，而唯一的反馈是用户自己去比对 get_config()。对一个把 configure() 当作
// 唯一配置入口的库来说，这个入口的输入校验强度决定了整条链路的上限。
//
// 列表必须与下面 `options.contains(...)` 的分支严格一致；多一个或少一个都会让
// 文档与行为错位。
constexpr std::string_view kKnownConfigKeys[] = {
    "backend",
    "io_worker_count",
    "buffer_pool_max",
    "buffer_size",
    "close_timeout_ms",
    "io_uring_queue_depth",
    "iocp_batch_size",
    "io_uring_sqpoll",
    "adaptive_batch",
    "adaptive_target_latency_us",
};

// nanobind 把类型不匹配的 cast 失败报成 std::runtime_error，于是 Python 侧看到的是
// RuntimeError("bad cast")。而"参数类型不对"在 Python 里应当永远是 TypeError ——
// 用户写 except TypeError 兜不住 RuntimeError，这是真实的可用性缺陷。
// 这里统一翻译，并带上出问题的 key，否则只能告诉用户"某个值类型错了"。
template <typename T>
T cast_option(nb::dict options, const char* key) {
    try {
        return nb::cast<T>(options[key]);
    } catch (const nb::cast_error&) {
        throw nb::type_error(
            ("configure(): value for '" + std::string(key) + "' has the wrong type").c_str());
    }
}

// 尺寸类字段：先收成 int64 再验正负。这样 -1 报的是 ValueError（值非法），
// 而不是 TypeError（类型不对）—— 两者对调用方的意义完全不同。
std::size_t cast_size_option(nb::dict options, const char* key) {
    const auto value = cast_option<std::int64_t>(options, key);
    if (value <= 0) {
        throw nb::value_error((std::string(key) + " must be positive").c_str());
    }
    return static_cast<std::size_t>(value);
}

std::string unknown_key_message(const std::string& key) {
    std::string message = "unknown config key: '" + key + "'. Valid keys: ";
    bool first = true;
    for (const auto candidate : kKnownConfigKeys) {
        if (!first) message += ", ";
        message += candidate;
        first = false;
    }
    return message;
}

} // namespace

void py_configure(nb::dict options) {
    // 先验 key，再改任何状态：否则一批里有一半生效、一半被静默忽略。
    for (const auto& item : options) {
        const auto key = nb::cast<std::string>(item.first);
        bool known = false;
        for (const auto candidate : kKnownConfigKeys) {
            if (candidate == key) {
                known = true;
                break;
            }
        }
        if (!known) throw nb::value_error(unknown_key_message(key).c_str());
    }

    auto cfg = core::config().get();
    if (options.contains("backend")) {
        const auto name = cast_option<std::string>(options, "backend");
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
        const auto value = cast_option<unsigned>(options, "io_worker_count");
        if (value > 128) throw nb::value_error("io_worker_count must be 0-128");
        cfg.io_worker_count = value;
    }
    if (options.contains("buffer_pool_max")) cfg.buffer_pool_max = cast_size_option(options, "buffer_pool_max");
    if (options.contains("buffer_size")) cfg.buffer_size = cast_size_option(options, "buffer_size");
    if (options.contains("close_timeout_ms")) cfg.close_timeout_ms = cast_option<unsigned>(options, "close_timeout_ms");
    if (options.contains("io_uring_queue_depth")) cfg.io_uring_queue_depth = cast_option<unsigned>(options, "io_uring_queue_depth");
    if (options.contains("iocp_batch_size")) {
        const auto value = cast_option<unsigned>(options, "iocp_batch_size");
        if (value < 1 || value > 256) throw nb::value_error("iocp_batch_size must be 1-256");
        cfg.iocp_batch_size = value;
    }
    if (options.contains("io_uring_sqpoll")) {
        const bool value = cast_option<bool>(options, "io_uring_sqpoll");
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
    if (options.contains("adaptive_batch")) cfg.adaptive_batch = cast_option<bool>(options, "adaptive_batch");
    if (options.contains("adaptive_target_latency_us")) {
        const auto value = cast_option<unsigned>(options, "adaptive_target_latency_us");
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
