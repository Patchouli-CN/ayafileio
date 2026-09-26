#pragma once
#ifdef HAVE_IO_URING

#include <liburing.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <memory>
#include <thread>
#include <vector>
#include <Python.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include "utils/debug_log.hpp"

namespace ayafileio {

class IOUringBackend;
struct IORequest;

#define THREAD_ID_HASH() std::hash<std::thread::id>{}(std::this_thread::get_id())

extern std::atomic<bool> g_uring_running;
extern std::mutex g_uring_instances_mtx;
extern std::unordered_map<void*, std::shared_ptr<struct UringInstance>> g_uring_instances;

// 一个待提交的 I/O（原始字段形式，可安全跨线程传递）
struct PendingSubmit {
    IORequest*  req;
    int         op;
    int         fd;
    const void* buf;
    unsigned    len;
    uint64_t    offset;
};

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

    // SQ 提交序列化 + 溢出队列（背压）：
    // io_uring 的 SQ 不是多线程安全的，所有 get_sqe/submit 都必须持
    // submit_mtx。SQ 打满时不再给调用方报 EBUSY，而是排入 overflow，
    // 由 reaper 在每次 CQE 排空后补提交（uring_drain_overflow）。
    std::mutex submit_mtx;
    std::deque<PendingSubmit> overflow;
    
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
        
        unsigned actual_flags = 0;
        if (inst->sqpoll) actual_flags |= IORING_SETUP_SQPOLL;

        // 优先启用 5.19+ 的 COOP_TASKRUN + TASKRUN_FLAG：完成通过协作式
        // task_work 递送，免去每个完成一次的软中断调度。旧内核不认这些
        // flag 会返回 EINVAL，静默回退到无优化 flag。
        // 注意：刻意不用 IORING_SETUP_SINGLE_ISSUER —— 提交来自多个
        // Python 线程及 reaper 的溢出补提交，违反 single-issuer 约束。
        // 这两个常量是稳定的内核 UAPI（bit 8/9），但老版本 liburing 头
        // 文件（如 manylinux 容器里的）尚未定义 —— 编译期补上。
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
        int ret = io_uring_queue_init(inst->queue_depth, &inst->ring,
                                      actual_flags | IORING_SETUP_COOP_TASKRUN
                                                   | IORING_SETUP_TASKRUN_FLAG);
        if (ret < 0) {
            ret = io_uring_queue_init(inst->queue_depth, &inst->ring, actual_flags);
        }
        if (ret < 0) {
            UR_LOG("UringManager::setup_instance: io_uring_queue_init failed, ret=%d, errno=%d", ret, errno);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        
        // 提交 eventfd poll 请求
        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
        if (!sqe) {
            UR_LOG("UringManager::setup_instance: failed to get sqe");
            io_uring_queue_exit(&inst->ring);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        
        io_uring_prep_poll_add(sqe, inst->event_fd, POLLIN);
        io_uring_sqe_set_data(sqe, nullptr);
        ret = io_uring_submit(&inst->ring);
        if (ret < 0) {
            UR_LOG("UringManager::setup_instance: submit poll failed, ret=%d", ret);
            io_uring_queue_exit(&inst->ring);
            ::close(inst->event_fd);
            inst->event_fd = -1;
            return false;
        }
        
        // 不等待 CQE，让 reaper 线程处理
        UR_LOG("UringManager::setup_instance: success, ring_fd=%d, event_fd=%d (poll submitted, not waited)",
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

// ── SQ 提交辅助（背压模式）────────────────────────────────────────────────
// 全部须持 inst->submit_mtx 调用（除 uring_submit_or_queue / drain 自持锁）。

inline void uring_prep_sqe(struct io_uring_sqe* sqe, const PendingSubmit& ps) {
    if (ps.op == IORING_OP_READ) [[likely]]
        io_uring_prep_read(sqe, ps.fd, const_cast<void*>(ps.buf), ps.len, ps.offset);
    else if (ps.op == IORING_OP_WRITE) [[likely]]
        io_uring_prep_write(sqe, ps.fd, ps.buf, ps.len, ps.offset);
    else if (ps.op == IORING_OP_FSYNC)
        io_uring_prep_fsync(sqe, ps.fd, 0);
    io_uring_sqe_set_data(sqe, ps.req);
}

// 提交一个 I/O；SQ 满或已有排队项时排入溢出队列（保序），由 reaper 补提交
inline void uring_submit_or_queue(UringInstance* inst, const PendingSubmit& ps) {
    std::lock_guard<std::mutex> lk(inst->submit_mtx);
    if (!inst->overflow.empty()) {
        inst->overflow.push_back(ps);
        return;
    }
    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
    if (!sqe) {
        inst->overflow.push_back(ps);
        return;
    }
    uring_prep_sqe(sqe, ps);
    io_uring_submit(&inst->ring);
}

// reaper 在每次 CQE 排空后调用：尽量把溢出队列补进 SQ
inline void uring_drain_overflow(UringInstance* inst) {
    std::lock_guard<std::mutex> lk(inst->submit_mtx);
    bool prepared = false;
    while (!inst->overflow.empty()) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst->ring);
        if (!sqe) {
            if (!prepared) break;       // SQ 满且无可冲项 —— 下批 CQE 再试
            io_uring_submit(&inst->ring);  // 冲掉已预备项，SQ 立即腾出
            prepared = false;
            continue;
        }
        uring_prep_sqe(sqe, inst->overflow.front());
        inst->overflow.pop_front();
        prepared = true;
    }
    if (prepared) io_uring_submit(&inst->ring);
}

} // namespace ayafileio

#endif // HAVE_IO_URING