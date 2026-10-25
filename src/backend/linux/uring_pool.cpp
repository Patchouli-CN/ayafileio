#ifdef HAVE_IO_URING

#include "uring_pool.hpp"

#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace ayafileio::backend::linux {

UringInstance::~UringInstance() {
    stop_reaper();
    if (ring_ready) io_uring_queue_exit(&ring);
    if (event_fd != -1) close(event_fd);
}

void UringInstance::stop_reaper() {
    running.store(false, std::memory_order_release);
    reaper_stop.store(true, std::memory_order_release);
    if (event_fd != -1) {
        const uint64_t value = 1;
        (void)write(event_fd, &value, sizeof(value));
    }
    if (reaper_thread.joinable()) reaper_thread.join();
}

UringManager& UringManager::instance() {
    static UringManager manager;
    return manager;
}

std::shared_ptr<UringInstance> UringManager::acquire(
    core::SchedulerContext scheduler,
    UringInstance::ReaperFunc reaper,
    unsigned queue_depth,
    unsigned flags,
    bool sqpoll) {
    std::lock_guard lock(mutex_);
    const auto it = instances_.find(scheduler.key);
    if (it != instances_.end()) {
        if (it->second && it->second->running.load(std::memory_order_acquire) &&
            !it->second->scheduler.owner.expired()) {
            return it->second;
        }
        if (it->second) it->second->stop_reaper();
        instances_.erase(it);
    }

    auto instance = std::make_shared<UringInstance>();
    instance->scheduler = std::move(scheduler);
    instance->queue_depth = queue_depth;
    instance->flags = flags;
    instance->sqpoll = sqpoll;
    instance->reaper_func = reaper;
    if (!setup_instance(instance.get())) return nullptr;
    instances_[instance->scheduler.key] = instance;
    return instance;
}

void UringManager::start_reaper(const std::shared_ptr<UringInstance>& instance) {
    std::lock_guard lock(instance->start_mutex);
    if (instance->reaper_started.exchange(true)) return;
    instance->reaper_thread = std::thread(instance->reaper_func, instance.get());
}

void UringManager::cleanup_all() {
    std::lock_guard lock(mutex_);
    for (auto& [key, instance] : instances_) {
        if (instance) instance->stop_reaper();
    }
    instances_.clear();
}

bool UringManager::setup_instance(UringInstance* instance) {
    instance->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (instance->event_fd == -1) return false;

    unsigned actual_flags = instance->sqpoll ? IORING_SETUP_SQPOLL : 0;
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
    int result = io_uring_queue_init(
        instance->queue_depth,
        &instance->ring,
        actual_flags | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG);
    if (result < 0) {
        result = io_uring_queue_init(instance->queue_depth, &instance->ring, actual_flags);
    }
    if (result < 0) {
        close(instance->event_fd);
        instance->event_fd = -1;
        return false;
    }
    instance->ring_ready = true;

    io_uring_sqe* sqe = io_uring_get_sqe(&instance->ring);
    if (!sqe) {
        io_uring_queue_exit(&instance->ring);
        instance->ring_ready = false;
        close(instance->event_fd);
        instance->event_fd = -1;
        return false;
    }
    io_uring_prep_poll_add(sqe, instance->event_fd, POLLIN);
    io_uring_sqe_set_data(sqe, nullptr);
    if (io_uring_submit(&instance->ring) < 0) {
        io_uring_queue_exit(&instance->ring);
        instance->ring_ready = false;
        close(instance->event_fd);
        instance->event_fd = -1;
        return false;
    }
    return true;
}

void uring_prep_sqe(io_uring_sqe* sqe, const PendingSubmit& pending) {
    if (pending.op == IORING_OP_READ) {
        io_uring_prep_read(sqe, pending.fd, const_cast<void*>(pending.buf), pending.len, pending.offset);
    } else if (pending.op == IORING_OP_WRITE) {
        io_uring_prep_write(sqe, pending.fd, pending.buf, pending.len, pending.offset);
    } else if (pending.op == IORING_OP_FSYNC) {
        io_uring_prep_fsync(sqe, pending.fd, 0);
    } else if (pending.op == IORING_OP_OPENAT) {
        io_uring_prep_openat(
            sqe, pending.fd, static_cast<const char*>(pending.buf),
            static_cast<int>(pending.len), static_cast<mode_t>(pending.offset));
    }
    io_uring_sqe_set_data(sqe, pending.request);
}

void uring_submit_or_queue(UringInstance* instance, const PendingSubmit& pending) {
    std::lock_guard lock(instance->submit_mtx);
    if (!instance->overflow.empty()) {
        instance->overflow.push_back(pending);
        return;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(&instance->ring);
    if (!sqe) {
        instance->overflow.push_back(pending);
        return;
    }
    uring_prep_sqe(sqe, pending);
    io_uring_submit(&instance->ring);
}

void uring_drain_overflow(UringInstance* instance) {
    std::lock_guard lock(instance->submit_mtx);
    bool prepared = false;
    while (!instance->overflow.empty()) {
        io_uring_sqe* sqe = io_uring_get_sqe(&instance->ring);
        if (!sqe) {
            if (!prepared) break;
            io_uring_submit(&instance->ring);
            prepared = false;
            continue;
        }
        uring_prep_sqe(sqe, instance->overflow.front());
        instance->overflow.pop_front();
        prepared = true;
    }
    if (prepared) io_uring_submit(&instance->ring);
}

} // namespace ayafileio::backend::linux

#endif
