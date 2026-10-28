#pragma once

#include "buffer.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace ayafileio::core {

struct Completion {
    ReqType type{ReqType::Other};
    std::size_t value{0};
    std::intptr_t handle{-1};
    Buffer buffer{};
    IoError error{};
    std::shared_ptr<void> error_object{};
    bool return_count{false};

    bool failed() const noexcept { return error.kind != ErrorKind::None; }

    static Completion success(ReqType type, std::size_t value = 0, std::intptr_t handle = -1) {
        Completion result;
        result.type = type;
        result.value = value;
        result.handle = handle;
        return result;
    }

    static Completion counted(ReqType type, std::size_t value) {
        auto result = success(type, value);
        result.return_count = true;
        return result;
    }

    static Completion read(Buffer buffer, std::size_t size) {
        Completion result = success(ReqType::Read, size);
        result.buffer = std::move(buffer);
        return result;
    }

    static Completion failure(
        ReqType type,
        IoError error,
        std::shared_ptr<void> error_object = {}) {
        Completion result;
        result.type = type;
        result.error = std::move(error);
        result.error_object = std::move(error_object);
        return result;
    }
};

struct SchedulerContext {
    void* key{nullptr};
    std::weak_ptr<void> owner{};
};

class Operation {
public:
    virtual ~Operation() = default;

    virtual Buffer allocate_read_buffer(std::size_t size) {
        return Buffer::heap(size);
    }
    virtual SchedulerContext scheduler_context() { return {this, {}}; }
    virtual void submitted() = 0;
    virtual void complete(Completion result) = 0;
    virtual void complete_inline(Completion result) = 0;
    virtual void finished() = 0;
};

std::shared_ptr<Operation> make_noop_operation();

class CompletionScheduler {
public:
    virtual ~CompletionScheduler() = default;
    virtual uint32_t next_timeout_ms() = 0;
    virtual void flush_idle() = 0;
};

void set_completion_scheduler(CompletionScheduler* scheduler);
CompletionScheduler* completion_scheduler();

// ── 工作线程与宿主运行时的挂接点 ────────────────────────────────────────────
// 后端的 worker 线程不是宿主（CPython）创建的线程。若只在每次完成时
// PyGILState_Ensure，free-threading 构建会为每个 op 创建并销毁一次 thread state
// （实测 1.3M cycles/op）。宿主在 worker 启动/退出时各挂接一次，per-op 就只剩
// 抢一次 GIL（实测 ~40 ns）。core 不认识 CPython，只提供钩子；实现由 bindings 装。
using WorkerThreadHook = void (*)();
void set_worker_thread_hooks(WorkerThreadHook attach, WorkerThreadHook detach);
void worker_thread_attach();
void worker_thread_detach();

// RAII：在后端 worker 循环入口放一个，线程退出时自动解挂。
class WorkerThreadAnchor {
public:
    WorkerThreadAnchor() { worker_thread_attach(); }
    ~WorkerThreadAnchor() { worker_thread_detach(); }
    WorkerThreadAnchor(const WorkerThreadAnchor&) = delete;
    WorkerThreadAnchor& operator=(const WorkerThreadAnchor&) = delete;
};

} // namespace ayafileio::core
