#pragma once
#include "globals.hpp"
#include "pool.hpp"
#include "utils/debug_log.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdint>
#include <cstdlib>

// ════════════════════════════════════════════════════════════════════════════
// §5  IORequest
// ════════════════════════════════════════════════════════════════════════════

class IOBackendBase;
class ResultBatcher;

enum class ReqType : uint8_t { Read, Write, Other };

/// Lifecycle state for an IORequest — used to detect double‑delivery
/// of IOCP completions under extreme concurrency.
enum class IOState : uint8_t {
    PENDING,   // created, waiting for IOCP completion
    RESOLVED,  // I/O completed successfully (or sync‑done)
    REJECTED   // I/O failed, cancelled, or already processed
};

struct IORequest {
#ifdef _WIN32
    OVERLAPPED   ov{};
#endif
    IOBackendBase *file       = nullptr;
    ResultBatcher *batcher    = nullptr;
    PyObject      *future        = nullptr;
    PyObject      *set_result    = nullptr;
    PyObject      *set_exception = nullptr;
    PoolBuf       *poolBuf       = nullptr;
    char          *heapBuf       = nullptr;
    size_t         reqSize       = 0;
    ReqType        type          = ReqType::Other;
    std::atomic<IOState> state{IOState::PENDING};

    // readinto 专用字段
    PyObject      *userBuf       = nullptr;  // 用户提供的缓冲区对象（owned）
    Py_buffer      userBufView;             // 缓冲区的 Py_buffer（zeroed）
    bool           isReadinto    = false;   // 标记：是否为 readinto 请求

    char *buf() noexcept {
        if (isReadinto && userBufView.buf) return (char*)userBufView.buf;
        return poolBuf ? poolBuf->data : heapBuf;
    }

    // 接管缓冲区所有权（调用者负责释放）
    // 之后 buf() 返回 nullptr，析构不再触碰缓冲区
    void release_buffer_ownership() {
        if (poolBuf) {
            poolBuf->data = nullptr;
            delete poolBuf;
            poolBuf = nullptr;
        }
        heapBuf = nullptr;
    }

    ~IORequest() {
        UR_DEBUG_LOG("~IORequest req=%p future=%p set_result=%p set_exception=%p isReadinto=%d",
                     (void*)this, (void*)future, (void*)set_result, (void*)set_exception, isReadinto);
        Py_XDECREF(future);
        Py_XDECREF(set_result);
        Py_XDECREF(set_exception);
        if (isReadinto && userBufView.buf) {
            PyBuffer_Release(&userBufView);
        }
        Py_XDECREF(userBuf);
        if (poolBuf) pool_release(poolBuf);
        else if (!isReadinto && heapBuf) std::free(heapBuf);
    }
};
