#pragma once
#include <cstddef>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// Unified configuration struct
// ════════════════════════════════════════════════════════════════════════════

struct Config {
    // ── Cross-platform / general ──────────────────────────────────────────
    unsigned io_worker_count   = 0;            // I/O worker threads, 0=auto (max 128)
    size_t   buffer_pool_max   = 512;          // Max cached buffers
    size_t   buffer_size       = 64 * 1024;    // Buffer size in bytes
    unsigned close_timeout_ms  = 4000;         // Close timeout for pending I/O (ms)

    // ── Windows / IOCP ────────────────────────────────────────────────────
    size_t   handle_pool_max_per_key = 64;     // Max cached handles per file
    size_t   handle_pool_max_total   = 2048;   // Max total cached handles
    unsigned iocp_batch_size         = 64;     // GQCSEx batch size (1-256)

    // ── Linux / io_uring ──────────────────────────────────────────────────
    unsigned io_uring_queue_depth    = 256;    // Submission queue depth (1-4096)
    unsigned io_uring_flags          = 0;      // io_uring_setup flags
    bool     io_uring_sqpoll         = false;  // Enable SQPOLL mode
    unsigned io_uring_sqpoll_idle_ms = 1000;   // SQPOLL idle timeout

    // ── ResultBatcher / adaptive batching (all platforms) ─────────────────
    bool     adaptive_batch             = true;  // Enable adaptive batch sizing
    unsigned adaptive_target_latency_us = 1000;  // Target max extra latency (us), 1-10000

    // ── Validation ────────────────────────────────────────────────────────
    bool validate() const {
        // General
        if (io_worker_count > 128) return false;
        if (buffer_pool_max == 0 || buffer_size == 0) return false;
        if (close_timeout_ms == 0 || close_timeout_ms > 30000) return false;
        // Windows
        if (handle_pool_max_per_key == 0 || handle_pool_max_total == 0) return false;
        if (handle_pool_max_per_key > handle_pool_max_total) return false;
        if (iocp_batch_size < 1 || iocp_batch_size > 256) return false;
        // Linux
        if (io_uring_queue_depth == 0 || io_uring_queue_depth > 4096) return false;
        // Adaptive
        if (adaptive_target_latency_us < 1 || adaptive_target_latency_us > 10000) return false;
        return true;
    }

    static Config defaults() { return Config{}; }
};

// ════════════════════════════════════════════════════════════════════════════
// Global config manager (thread-safe)
// ════════════════════════════════════════════════════════════════════════════

class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager inst;
        return inst;
    }

    Config get() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config;
    }

    void update(const Config& new_config) {
        if (!new_config.validate()) {
            throw std::invalid_argument("Invalid configuration");
        }
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_config = new_config;
    }

    void update_partial(const std::unordered_map<std::string, size_t>& updates) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        for (const auto& [key, value] : updates) {
            if      (key == "io_worker_count")              m_config.io_worker_count = (unsigned)value;
            else if (key == "buffer_pool_max")              m_config.buffer_pool_max = value;
            else if (key == "buffer_size")                  m_config.buffer_size = value;
            else if (key == "close_timeout_ms")             m_config.close_timeout_ms = (unsigned)value;
            else if (key == "handle_pool_max_per_key")      m_config.handle_pool_max_per_key = value;
            else if (key == "handle_pool_max_total")        m_config.handle_pool_max_total = value;
            else if (key == "iocp_batch_size")              m_config.iocp_batch_size = (unsigned)value;
            else if (key == "io_uring_queue_depth")         m_config.io_uring_queue_depth = (unsigned)value;
        }
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid configuration after update");
        }
    }

    // ── Convenience getters (thread-safe) ─────────────────────────────────
    unsigned io_worker_count()    const { std::shared_lock lk(m_mutex); return m_config.io_worker_count; }
    size_t   buffer_pool_max()    const { std::shared_lock lk(m_mutex); return m_config.buffer_pool_max; }
    size_t   buffer_size()        const { std::shared_lock lk(m_mutex); return m_config.buffer_size; }
    unsigned close_timeout_ms()   const { std::shared_lock lk(m_mutex); return m_config.close_timeout_ms; }
    size_t   handle_pool_max_per_key() const { std::shared_lock lk(m_mutex); return m_config.handle_pool_max_per_key; }
    size_t   handle_pool_max_total()   const { std::shared_lock lk(m_mutex); return m_config.handle_pool_max_total; }
    unsigned iocp_batch_size()         const { std::shared_lock lk(m_mutex); return m_config.iocp_batch_size; }
    unsigned io_uring_queue_depth()    const { std::shared_lock lk(m_mutex); return m_config.io_uring_queue_depth; }
    unsigned io_uring_flags()          const { std::shared_lock lk(m_mutex); return m_config.io_uring_flags; }
    bool     io_uring_sqpoll()         const { std::shared_lock lk(m_mutex); return m_config.io_uring_sqpoll; }
    unsigned io_uring_sqpoll_idle_ms() const { std::shared_lock lk(m_mutex); return m_config.io_uring_sqpoll_idle_ms; }
    bool     adaptive_batch()          const { std::shared_lock lk(m_mutex); return m_config.adaptive_batch; }
    unsigned adaptive_target_latency_us() const { std::shared_lock lk(m_mutex); return m_config.adaptive_target_latency_us; }

private:
    ConfigManager() : m_config(Config::defaults()) {}

    Config m_config;
    mutable std::shared_mutex m_mutex;
};

inline ConfigManager& config() {
    return ConfigManager::instance();
}

} // namespace ayafileio
