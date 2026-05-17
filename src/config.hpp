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
// 统一配置结构
// ════════════════════════════════════════════════════════════════════════════

struct Config {
    // === 句柄池配置 (Windows) ===
    size_t handle_pool_max_per_key = 64;
    size_t handle_pool_max_total   = 2048;
    
    // === I/O 工作线程配置 (ThreadIOBackend) ===
    unsigned io_worker_count = 0;  // 0 = 自动
    
    // === 缓冲区池配置 ===
    size_t buffer_pool_max = 512;
    size_t buffer_size     = 64 * 1024;
    
    // === 超时配置 ===
    unsigned close_timeout_ms = 4000;
    
    // === IOCP 配置 (Windows) ===
    unsigned iocp_batch_size = 64;

    // === io_uring 配置 (Linux) ===
    unsigned io_uring_queue_depth = 256;
    unsigned io_uring_flags = 0;
    bool io_uring_sqpoll = false;
    unsigned io_uring_sqpoll_idle_ms = 1000;

    // 验证配置有效性
    bool validate() const {
        if (handle_pool_max_per_key == 0 || handle_pool_max_total == 0) return false;
        if (handle_pool_max_per_key > handle_pool_max_total) return false;
        if (io_worker_count > 128) return false;
        if (buffer_pool_max == 0 || buffer_size == 0) return false;
        if (close_timeout_ms == 0 || close_timeout_ms > 30000) return false;
        if (iocp_batch_size < 1 || iocp_batch_size > 256) return false;
        if (io_uring_queue_depth == 0 || io_uring_queue_depth > 4096) return false;
        return true;
    }
    
    static Config defaults() { return Config{}; }
};

// ════════════════════════════════════════════════════════════════════════════
// 全局配置管理器 (线程安全)
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
            if (key == "handle_pool_max_per_key") m_config.handle_pool_max_per_key = value;
            else if (key == "handle_pool_max_total") m_config.handle_pool_max_total = value;
            else if (key == "io_worker_count") m_config.io_worker_count = (unsigned)value;
            else if (key == "buffer_pool_max") m_config.buffer_pool_max = value;
            else if (key == "buffer_size") m_config.buffer_size = value;
            else if (key == "close_timeout_ms") m_config.close_timeout_ms = (unsigned)value;
            else if (key == "iocp_batch_size") m_config.iocp_batch_size = (unsigned)value;
            else if (key == "io_uring_queue_depth") m_config.io_uring_queue_depth = (unsigned)value;
        }
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid configuration after update");
        }
    }
    
    // 便捷 getter（线程安全，返回副本）
    size_t handle_pool_max_per_key() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.handle_pool_max_per_key; 
    }
    size_t handle_pool_max_total() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.handle_pool_max_total; 
    }
    unsigned io_worker_count() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.io_worker_count; 
    }
    size_t buffer_pool_max() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.buffer_pool_max; 
    }
    size_t buffer_size() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.buffer_size; 
    }
    unsigned close_timeout_ms() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.close_timeout_ms;
    }
    unsigned iocp_batch_size() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.iocp_batch_size;
    }
    unsigned io_uring_queue_depth() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.io_uring_queue_depth; 
    }
    unsigned io_uring_flags() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.io_uring_flags; 
    }
    bool io_uring_sqpoll() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.io_uring_sqpoll; 
    }
    unsigned io_uring_sqpoll_idle_ms() const { 
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config.io_uring_sqpoll_idle_ms; 
    }
    
private:
    ConfigManager() : m_config(Config::defaults()) {}
    
    Config m_config;
    mutable std::shared_mutex m_mutex;
};

// 便捷访问函数
inline ConfigManager& config() {
    return ConfigManager::instance();
}

} // namespace ayafileio