#pragma once
#include "globals.hpp"
#include "pool.hpp"
#include "loop_handle.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// §5  IORequest
// ════════════════════════════════════════════════════════════════════════════

class IOBackendBase;

enum class ReqType : uint8_t { Read, Write, Other };

// io_request.hpp
struct IORequest {
#ifdef _WIN32
    OVERLAPPED   ov{};
#endif
    IOBackendBase  *file          = nullptr;
    LoopHandle  *loop_handle   = nullptr;
    PyObject    *future        = nullptr;
    PyObject    *set_result    = nullptr;
    PyObject    *set_exception = nullptr;
    PoolBuf     *poolBuf       = nullptr;
    char        *heapBuf       = nullptr;
    size_t       reqSize       = 0;
    ReqType      type          = ReqType::Other;
    
    // readinto 专用字段
    PyObject    *userBuf       = nullptr;  // 用户提供的缓冲区对象（owned）
    Py_buffer    userBufView;             // 缓冲区的 Py_buffer（zeroed）
    bool         isReadinto    = false;   // 标记：是否为 readinto 请求

    char *buf() noexcept {
        if (isReadinto && userBufView.buf) return (char*)userBufView.buf;
        return poolBuf ? poolBuf->data : heapBuf;
    }

    ~IORequest() {
        Py_XDECREF(future);
        Py_XDECREF(set_result);
        Py_XDECREF(set_exception);
        if (isReadinto && userBufView.buf) {
            PyBuffer_Release(&userBufView);
        }
        Py_XDECREF(userBuf);
        if (poolBuf) pool_release(poolBuf);
        else if (!isReadinto) delete[] heapBuf;  // readinto 模式不管理 heapBuf
    }
};
