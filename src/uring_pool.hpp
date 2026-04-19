#pragma once
#ifdef HAVE_IO_URING

#include <liburing.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <Python.h>

// ════════════════════════════════════════════════════════════════════════════
// io_uring 实例池 - 复用 io_uring，避免频繁创建/销毁
// ════════════════════════════════════════════════════════════════════════════

// 前置声明
class IOUringBackend;

struct UringInstance {
    struct io_uring ring;
    std::atomic<int> ref_count{0};
    std::atomic<bool> running{true};
    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    PyObject* loop = nullptr;  // 关联的事件循环（强引用）
    
    // 配置参数
    unsigned queue_depth = 256;
    unsigned flags = 0;
    bool sqpoll = false;
    
    // reaper 循环函数指针（由 IOUringBackend 设置）
    using ReaperFunc = void (*)(UringInstance*);
    ReaperFunc reaper_func = nullptr;
    
    ~UringInstance() {
        stop_reaper();
        io_uring_queue_exit(&ring);
        Py_XDECREF(loop);
    }
    
    void stop_reaper() {
        if (!running.exchange(false)) return;
        reaper_stop.store(true, std::memory_order_release);
        
        // 等待 reaper 线程结束
        if (reaper_thread.joinable()) {
            reaper_thread.join();
        }
    }
    
    void add_ref() { ref_count.fetch_add(1, std::memory_order_relaxed); }
    void release() { ref_count.fetch_sub(1, std::memory_order_relaxed); }
    int get_ref() const { return ref_count.load(std::memory_order_relaxed); }
};

class UringPool {
public:
    static UringPool& instance() {
        static UringPool pool;
        return pool;
    }
    
    // 获取或创建与当前事件循环关联的 io_uring 实例
    std::shared_ptr<UringInstance> acquire(PyObject* loop, 
                                            UringInstance::ReaperFunc reaper_func,
                                            unsigned queue_depth = 256,
                                            unsigned flags = 0,
                                            bool sqpoll = false) {
        std::lock_guard<std::mutex> lk(m_mutex);
        
        // 使用 loop 指针作为 key
        void* key = loop;
        
        auto it = m_instances.find(key);
        if (it != m_instances.end()) {
            auto inst = it->second.lock();
            if (inst && inst->running.load(std::memory_order_acquire)) {
                inst->add_ref();
                return inst;
            }
            // 实例已失效，移除
            m_instances.erase(it);
        }
        
        // 创建新实例
        auto inst = std::make_shared<UringInstance>();
        inst->queue_depth = queue_depth;
        inst->flags = flags;
        inst->sqpoll = sqpoll;
        inst->loop = loop;
        inst->reaper_func = reaper_func;
        Py_INCREF(loop);
        
        if (!setup_instance(inst.get())) {
            Py_DECREF(loop);
            return nullptr;
        }
        
        inst->add_ref();
        m_instances[key] = inst;
        
        return inst;
    }
    
    // 释放引用
    void release(std::shared_ptr<UringInstance>& inst) {
        if (!inst) return;
        
        inst->release();
        
        // 如果引用计数为 0，延迟清理
        if (inst->get_ref() == 0) {
            schedule_cleanup(inst);
        }
    }
    
    // 强制清理所有实例
    void cleanup() {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& pair : m_instances) {
            if (auto inst = pair.second.lock()) {
                inst->stop_reaper();
            }
        }
        m_instances.clear();
        
        // 清理待销毁队列
        std::lock_guard<std::mutex> lk2(m_cleanup_mutex);
        m_pending_cleanup.clear();
    }
    
private:
    UringPool() {
        // 启动清理线程
        m_cleanup_thread = std::thread(&UringPool::cleanup_loop, this);
    }
    
    ~UringPool() {
        m_stop_cleanup = true;
        m_cv.notify_all();
        if (m_cleanup_thread.joinable()) {
            m_cleanup_thread.join();
        }
        cleanup();
    }
    
    bool setup_instance(UringInstance* inst) {
        unsigned actual_flags = inst->flags;
        if (inst->sqpoll) {
            actual_flags |= IORING_SETUP_SQPOLL;
        }
#ifdef IORING_SETUP_SINGLE_ISSUER
        actual_flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
        actual_flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
        
        int ret = io_uring_queue_init(inst->queue_depth, &inst->ring, actual_flags);
        if (ret < 0) {
            return false;
        }
        
        return true;
    }
    
    void schedule_cleanup(std::shared_ptr<UringInstance> inst) {
        std::lock_guard<std::mutex> lk(m_cleanup_mutex);
        // 延迟 5 秒后清理
        m_pending_cleanup.push_back({
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            inst
        });
        m_cv.notify_one();
    }
    
    void cleanup_loop() {
        while (!m_stop_cleanup) {
            std::unique_lock<std::mutex> lk(m_cleanup_mutex);
            m_cv.wait_for(lk, std::chrono::seconds(1), [this] {
                return m_stop_cleanup || !m_pending_cleanup.empty();
            });
            
            if (m_stop_cleanup) break;
            
            auto now = std::chrono::steady_clock::now();
            auto it = m_pending_cleanup.begin();
            while (it != m_pending_cleanup.end()) {
                if (it->expiry <= now) {
                    // 从主 map 中移除
                    if (auto inst = it->instance.lock()) {
                        inst->stop_reaper();
                        std::lock_guard<std::mutex> lk2(m_mutex);
                        void* key = inst->loop;
                        auto map_it = m_instances.find(key);
                        if (map_it != m_instances.end()) {
                            auto existing = map_it->second.lock();
                            if (!existing || existing.get() == inst.get()) {
                                m_instances.erase(map_it);
                            }
                        }
                    }
                    it = m_pending_cleanup.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    
    struct CleanupEntry {
        std::chrono::steady_clock::time_point expiry;
        std::weak_ptr<UringInstance> instance;
    };
    
    std::mutex m_mutex;
    std::unordered_map<void*, std::weak_ptr<UringInstance>> m_instances;
    
    std::mutex m_cleanup_mutex;
    std::vector<CleanupEntry> m_pending_cleanup;
    std::condition_variable m_cv;
    std::thread m_cleanup_thread;
    bool m_stop_cleanup = false;
};

#endif // HAVE_IO_URING