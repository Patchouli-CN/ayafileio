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
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>          // for POLLIN
#include "debug_log.hpp"

// ════════════════════════════════════════════════════════════════════════════
// io_uring 实例池 - 复用 io_uring，避免频繁创建/销毁
// ════════════════════════════════════════════════════════════════════════════

// 前置声明
class IOUringBackend;

// 辅助宏：安全打印 std::thread::id（转换为 size_t 哈希值）
#define THREAD_ID_HASH() std::hash<std::thread::id>{}(std::this_thread::get_id())

struct UringInstance {
    struct io_uring ring;
    std::atomic<int> ref_count{0};
    std::atomic<bool> running{true};
    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    PyObject* loop = nullptr;  // 关联的事件循环（强引用）
    int event_fd = -1;         // 用于唤醒 reaper 线程的 eventfd
    
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
        if (event_fd != -1) {
            ::close(event_fd);
            event_fd = -1;
        }
        Py_XDECREF(loop);
    }
    
    void stop_reaper() {
        if (!running.exchange(false)) return;
        UR_LOG("UringInstance::stop_reaper: stopping reaper, inst=%p, thread_hash=0x%zx", (void*)this, THREAD_ID_HASH());
        reaper_stop.store(true, std::memory_order_release);
        
        // 向 eventfd 写入 1 字节，强制唤醒 reaper 线程
        if (event_fd != -1) {
            uint64_t val = 1;
            ssize_t ret = write(event_fd, &val, sizeof(val));
            if (ret != sizeof(val)) {
                UR_LOG("UringInstance::stop_reaper: write to eventfd failed, ret=%zd, errno=%d", ret, errno);
            } else {
                UR_LOG("UringInstance::stop_reaper: wrote to eventfd to wake reaper");
            }
        }
        
        // 等待 reaper 线程结束
        if (reaper_thread.joinable()) {
            reaper_thread.join();
            UR_LOG("UringInstance::stop_reaper: reaper thread joined");
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
        UR_LOG("UringPool::acquire: loop=%p, key=%p, thread_hash=0x%zx", (void*)loop, key, THREAD_ID_HASH());
        
        auto it = m_instances.find(key);
        if (it != m_instances.end()) {
            auto inst = it->second.lock();
            if (inst && inst->running.load(std::memory_order_acquire)) {
                inst->add_ref();
                UR_LOG("UringPool::acquire: found existing instance=%p, ref=%d", (void*)inst.get(), inst->get_ref());
                return inst;
            }
            UR_LOG("UringPool::acquire: existing instance expired, removing");
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
            UR_LOG("UringPool::acquire: setup_instance failed");
            return nullptr;
        }
        
        inst->add_ref();
        m_instances[key] = inst;
        UR_LOG("UringPool::acquire: created new instance=%p, queue_depth=%u", (void*)inst.get(), queue_depth);
        
        return inst;
    }
    
    // 释放引用
    void release(std::shared_ptr<UringInstance>& inst) {
        if (!inst) return;
        
        inst->release();
        int ref = inst->get_ref();
        UR_LOG("UringPool::release: inst=%p, ref=%d", (void*)inst.get(), ref);
        
        // 如果引用计数为 0，延迟清理
        if (ref == 0) {
            schedule_cleanup(inst);
        }
    }
    
    // 强制清理所有实例
    void cleanup() {
        std::lock_guard<std::mutex> lk(m_mutex);
        UR_LOG("UringPool::cleanup: cleaning up all instances");
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
        // 创建 eventfd (非阻塞)
        inst->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (inst->event_fd == -1) {
            UR_LOG("UringPool::setup_instance: eventfd failed, errno=%d", errno);
            return false;
        }
        UR_LOG("UringPool::setup_instance: eventfd created, fd=%d", inst->event_fd);
        
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
            UR_LOG("UringPool::setup_instance: io_uring_queue_init failed, ret=%d, errno=%d", ret, errno);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        UR_LOG("UringPool::setup_instance: success, ring_fd=%d", inst->ring.ring_fd);
        
        // 向 io_uring 提交一个监听 eventfd 可读事件的 poll 请求
        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
        if (!sqe) {
            UR_LOG("UringPool::setup_instance: failed to get sqe for eventfd poll");
            io_uring_queue_exit(&inst->ring);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        io_uring_prep_poll_add(sqe, inst->event_fd, POLLIN);
        io_uring_sqe_set_data(sqe, nullptr);  // 标记为内部事件
        ret = io_uring_submit(&inst->ring);
        if (ret < 0) {
            UR_LOG("UringPool::setup_instance: submit poll failed, ret=%d", ret);
            io_uring_queue_exit(&inst->ring);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        UR_LOG("UringPool::setup_instance: registered eventfd poll, submitted=%d", ret);
        
        return true;
    }
    
    void schedule_cleanup(std::shared_ptr<UringInstance> inst) {
        std::lock_guard<std::mutex> lk(m_cleanup_mutex);
        UR_LOG("UringPool::schedule_cleanup: scheduling cleanup for inst=%p", (void*)inst.get());
        // 延迟 5 秒后清理
        m_pending_cleanup.push_back({
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            inst
        });
        m_cv.notify_one();
    }
    
    void cleanup_loop() {
        UR_LOG("UringPool::cleanup_loop: started");
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
                    UR_LOG("UringPool::cleanup_loop: cleaning up expired instance");
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
        UR_LOG("UringPool::cleanup_loop: exiting");
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