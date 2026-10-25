#pragma once
#ifdef HAVE_IO_URING

#include "../../core/completion.hpp"
#include "../../core/io_request.hpp"

#include <atomic>
#include <deque>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ayafileio::backend::linux {

struct PendingSubmit {
    core::IORequest* request;
    int op;
    int fd;
    const void* buf;
    unsigned len;
    uint64_t offset;
};

struct UringInstance {
    io_uring ring{};
    bool ring_ready{false};
    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    std::atomic<bool> reaper_started{false};
    std::atomic<bool> running{true};
    std::mutex start_mutex;
    core::SchedulerContext scheduler;
    int event_fd{-1};
    unsigned queue_depth{256};
    unsigned flags{0};
    bool sqpoll{false};
    std::mutex submit_mtx;
    std::deque<PendingSubmit> overflow;
    std::mutex cq_mtx;

    using ReaperFunc = void (*)(UringInstance*);
    ReaperFunc reaper_func{nullptr};

    ~UringInstance();
    void stop_reaper();
};

class UringManager {
public:
    static UringManager& instance();

    std::shared_ptr<UringInstance> acquire(
        core::SchedulerContext scheduler,
        UringInstance::ReaperFunc reaper,
        unsigned queue_depth = 256,
        unsigned flags = 0,
        bool sqpoll = false);
    void start_reaper(const std::shared_ptr<UringInstance>& instance);
    void cleanup_all();

private:
    bool setup_instance(UringInstance* instance);

    std::mutex mutex_;
    std::unordered_map<void*, std::shared_ptr<UringInstance>> instances_;
};

inline UringManager& uring_manager() {
    return UringManager::instance();
}

inline void uring_cleanup_all() {
    UringManager::instance().cleanup_all();
}

void uring_prep_sqe(io_uring_sqe* sqe, const PendingSubmit& pending);
void uring_submit_or_queue(UringInstance* instance, const PendingSubmit& pending);
void uring_drain_overflow(UringInstance* instance);

} // namespace ayafileio::backend::linux

#endif
