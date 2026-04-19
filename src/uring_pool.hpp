#pragma once
#ifdef HAVE_IO_URING

#include <liburing.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <vector>
#include <Python.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include "debug_log.hpp"

class IOUringBackend;

#define THREAD_ID_HASH() std::hash<std::thread::id>{}(std::this_thread::get_id())

extern std::atomic<bool> g_uring_running;
extern std::mutex g_uring_instances_mtx;
extern std::unordered_map<void*, std::shared_ptr<struct UringInstance>> g_uring_instances;

struct UringInstance {
    struct io_uring ring;
    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    std::atomic<bool> reaper_started{false};
    std::atomic<bool> running{true};
    std::mutex start_mutex;
    PyObject* loop = nullptr;
    int event_fd = -1;
    
    unsigned queue_depth = 256;
    unsigned flags = 0;
    bool sqpoll = false;
    
    using ReaperFunc = void (*)(UringInstance*);
    ReaperFunc reaper_func = nullptr;
    
    UringInstance() = default;
    UringInstance(const UringInstance&) = delete;
    UringInstance& operator=(const UringInstance&) = delete;
    
    ~UringInstance() {
        UR_LOG("UringInstance destructor: this=%p", (void*)this);
        
        running.store(false, std::memory_order_release);
        reaper_stop.store(true, std::memory_order_release);
        
        if (event_fd != -1) {
            uint64_t val = 1;
            write(event_fd, &val, sizeof(val));
        }
        
        if (reaper_thread.joinable()) {
            reaper_thread.join();
        }
        
        io_uring_queue_exit(&ring);
        
        if (event_fd != -1) {
            ::close(event_fd);
        }
        
        UR_LOG("UringInstance destructor: done");
    }
    
    void stop_reaper() {
        running.store(false, std::memory_order_release);
        reaper_stop.store(true, std::memory_order_release);
        if (event_fd != -1) {
            uint64_t val = 1;
            write(event_fd, &val, sizeof(val));
        }
        if (reaper_thread.joinable()) {
            reaper_thread.join();
        }
    }
};

class UringManager {
public:
    static UringManager& instance() {
        static UringManager mgr;
        return mgr;
    }
    
    std::shared_ptr<UringInstance> acquire(PyObject* loop,
                                            UringInstance::ReaperFunc reaper_func,
                                            unsigned queue_depth = 256,
                                            unsigned flags = 0,
                                            bool sqpoll = false) {
        std::lock_guard<std::mutex> lk(m_mutex);
        
        void* key = loop;
        UR_LOG("UringManager::acquire: loop=%p", (void*)loop);
        
        auto it = m_instances.find(key);
        if (it != m_instances.end()) {
            auto inst = it->second;
            if (inst && inst->running.load(std::memory_order_acquire)) {
                UR_LOG("UringManager::acquire: found existing instance=%p", (void*)inst.get());
                return inst;
            }
            UR_LOG("UringManager::acquire: existing instance expired, removing");
            m_instances.erase(it);
        }
        
        auto inst = std::make_shared<UringInstance>();
        inst->queue_depth = queue_depth;
        inst->flags = flags;
        inst->sqpoll = sqpoll;
        inst->loop = loop;
        Py_INCREF(loop);  // 增加引用计数
        inst->reaper_func = reaper_func;
        inst->running.store(true, std::memory_order_release);
        
        if (!setup_instance(inst.get())) {
            Py_DECREF(loop);
            UR_LOG("UringManager::acquire: setup_instance failed");
            return nullptr;
        }
        
        m_instances[key] = inst;
        
        {
            std::lock_guard<std::mutex> lk2(g_uring_instances_mtx);
            g_uring_instances[key] = inst;
        }
        
        UR_LOG("UringManager::acquire: created new instance=%p", (void*)inst.get());
        return inst;
    }
    
    void start_reaper(std::shared_ptr<UringInstance> inst) {
        std::lock_guard<std::mutex> lk(inst->start_mutex);
        if (inst->reaper_started.exchange(true)) {
            UR_LOG("UringManager::start_reaper: reaper already started for inst=%p", (void*)inst.get());
            return;
        }
        inst->reaper_thread = std::thread(inst->reaper_func, inst.get());
        UR_LOG("UringManager::start_reaper: started reaper for inst=%p", (void*)inst.get());
    }
    
    void cleanup_all() {
        std::lock_guard<std::mutex> lk(m_mutex);
        UR_LOG("UringManager::cleanup_all: cleaning %zu instances", m_instances.size());
        
        for (auto& pair : m_instances) {
            auto inst = pair.second;
            if (inst) {
                UR_LOG("UringManager::cleanup_all: stopping instance=%p", (void*)inst.get());
                inst->stop_reaper();
                if (inst->loop) {
                    PyGILState_STATE gstate = PyGILState_Ensure();
                    Py_DECREF(inst->loop);
                    inst->loop = nullptr;
                    PyGILState_Release(gstate);
                }
            }
        }
        m_instances.clear();
        
        {
            std::lock_guard<std::mutex> lk2(g_uring_instances_mtx);
            g_uring_instances.clear();
        }
        
        UR_LOG("UringManager::cleanup_all: done");
    }
    
    // 移除不再使用的实例（当 loop 被销毁时调用）
    void remove(PyObject* loop) {
        std::lock_guard<std::mutex> lk(m_mutex);
        void* key = loop;
        auto it = m_instances.find(key);
        if (it != m_instances.end()) {
            UR_LOG("UringManager::remove: removing instance for loop=%p", (void*)loop);
            it->second->stop_reaper();
            m_instances.erase(it);
        }
    }
    
    size_t instance_count() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_instances.size();
    }
    
private:
    UringManager() {
        g_uring_running.store(true, std::memory_order_release);
    }
    
    ~UringManager() {
        cleanup_all();
    }
    
    bool setup_instance(UringInstance* inst) {
        inst->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (inst->event_fd == -1) {
            UR_LOG("UringManager::setup_instance: eventfd failed, errno=%d", errno);
            return false;
        }
        
        unsigned actual_flags = inst->flags;
        if (inst->sqpoll) actual_flags |= IORING_SETUP_SQPOLL;
#ifdef IORING_SETUP_SINGLE_ISSUER
        actual_flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
        actual_flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
        
        int ret = io_uring_queue_init(inst->queue_depth, &inst->ring, actual_flags);
        if (ret < 0) {
            UR_LOG("UringManager::setup_instance: io_uring_queue_init failed, ret=%d", ret);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        
        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, inst->event_fd, POLLIN);
            io_uring_sqe_set_data(sqe, nullptr);
            io_uring_submit(&inst->ring);
        }
        
        UR_LOG("UringManager::setup_instance: success, ring_fd=%d, event_fd=%d",
               inst->ring.ring_fd, inst->event_fd);
        return true;
    }
    
    mutable std::mutex m_mutex;
    std::unordered_map<void*, std::shared_ptr<UringInstance>> m_instances;
};

inline UringManager& uring_manager() {
    return UringManager::instance();
}

inline void uring_cleanup_all() {
    UringManager::instance().cleanup_all();
}

#endif // HAVE_IO_URING