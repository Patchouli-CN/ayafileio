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
    size_t handle_pool_max_per_key = 64;      // 每个文件最大缓存句柄数
    size_t handle_pool_max_total   = 2048;    // 全局最大缓存句柄数
    
    // === I/O 工作线程配置 (ThreadIOBackend) ===
    // 0 = 自动 (CPU核心数 * 2, 上限 16)
    // 1-128 = 固定数量
    unsigned io_worker_count = 0;
    
    // === 缓冲区池配置 ===
    size_t buffer_pool_max = 512;             // 最大缓存缓冲区数
    size_t buffer_size     = 64 * 1024;       // 单个缓冲区大小 (64KB)
    
    // === 超时配置 ===
    unsigned close_timeout_ms = 4000;         // 关闭时等待 pending I/O 的最大时间 (ms)
    
    // === io_uring 配置 (Linux) ===
    unsigned io_uring_queue_depth = 256;      // io_uring 队列深度
    unsigned io_uring_flags = 0;              // io_uring 初始化标志 (IORING_SETUP_*)
    bool io_uring_sqpoll = false;             // 是否启用 SQPOLL 模式
    unsigned io_uring_sqpoll_idle_ms = 1000;  // SQPOLL 空闲超时 (ms)
    
    // === 调试/日志配置 ===
    bool enable_debug_log = false;             // 是否启用调试日志
    bool enable_perf_stats = false;            // 是否启用性能统计
    
    // 验证配置有效性
    bool validate() const {
        if (handle_pool_max_per_key == 0 || handle_pool_max_total == 0) return false;
        if (handle_pool_max_per_key > handle_pool_max_total) return false;
        if (io_worker_count > 128) return false;
        if (buffer_pool_max == 0 || buffer_size == 0) return false;
        if (close_timeout_ms == 0 || close_timeout_ms > 30000) return false;
        if (io_uring_queue_depth == 0 || io_uring_queue_depth > 4096) return false;
        return true;
    }
    
    // 获取默认配置
    static Config defaults() {
        return Config();
    }
    
    // 从环境变量加载
    static Config from_env() {
        Config cfg;
        // 这里可以从环境变量读取
        // 例如: AYAFILEIO_BUFFER_SIZE=131072
        return cfg;
    }
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
    
    // 获取当前配置（返回副本）
    Config get() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_config;
    }
    
    // 更新配置（部分或全部）
    void update(const Config& new_config) {
        if (!new_config.validate()) {
            throw std::invalid_argument("Invalid configuration");
        }
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_config = new_config;
        // 通知配置已变更（可以触发回调）
        on_config_changed();
    }
    
    // 部分更新（只更新指定的字段）
    void update_partial(const std::unordered_map<std::string, size_t>& updates) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        for (const auto& [key, value] : updates) {
            if (key == "handle_pool_max_per_key") m_config.handle_pool_max_per_key = value;
            else if (key == "handle_pool_max_total") m_config.handle_pool_max_total = value;
            else if (key == "io_worker_count") m_config.io_worker_count = (unsigned)value;
            else if (key == "buffer_pool_max") m_config.buffer_pool_max = value;
            else if (key == "buffer_size") m_config.buffer_size = value;
            else if (key == "close_timeout_ms") m_config.close_timeout_ms = (unsigned)value;
            else if (key == "io_uring_queue_depth") m_config.io_uring_queue_depth = (unsigned)value;
        }
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid configuration after update");
        }
        on_config_changed();
    }
    
    // 便捷 getter（无需加锁的快速读取，适用于单次读取）
    size_t handle_pool_max_per_key() const { return m_config.handle_pool_max_per_key; }
    size_t handle_pool_max_total() const { return m_config.handle_pool_max_total; }
    unsigned io_worker_count() const { return m_config.io_worker_count; }
    size_t buffer_pool_max() const { return m_config.buffer_pool_max; }
    size_t buffer_size() const { return m_config.buffer_size; }
    unsigned close_timeout_ms() const { return m_config.close_timeout_ms; }
    unsigned io_uring_queue_depth() const { return m_config.io_uring_queue_depth; }
    unsigned io_uring_flags() const { return m_config.io_uring_flags; }
    bool io_uring_sqpoll() const { return m_config.io_uring_sqpoll; }
    unsigned io_uring_sqpoll_idle_ms() const { return m_config.io_uring_sqpoll_idle_ms; }
    
    // 注册配置变更回调
    using ChangeCallback = void(*)();
    void register_callback(ChangeCallback cb) {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        m_callbacks.push_back(cb);
    }
    
private:
    ConfigManager() : m_config(Config::defaults()) {}
    
    void on_config_changed() {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        for (auto cb : m_callbacks) {
            cb();
        }
    }
    
    Config m_config;
    mutable std::shared_mutex m_mutex;
    std::mutex m_cb_mutex;
    std::vector<ChangeCallback> m_callbacks;
};

// 便捷访问宏
inline ConfigManager& config() {
    return ConfigManager::instance();
}

} // namespace ayafileio