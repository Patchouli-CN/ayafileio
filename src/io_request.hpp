#pragma once
#include "globals.hpp"
#include "pool.hpp"
#include "utils/debug_log.hpp"
#include "utils/yuyuko_memlife.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

namespace ayafileio {

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
    Py_buffer      userBufView{};           // 缓冲区的 Py_buffer（零初始化）
    bool           isReadinto    = false;   // 标记：是否为 readinto 请求
    // 零拷贝写：持调用方缓冲区视图，内核直接读用户内存，省一次完整拷贝。
    // 与 readinto 共用 userBuf/userBufView，析构同样负责释放。
    bool           holdsWriteBuf = false;

    // 读零拷贝（owned）：非空时 I/O 直接读进这个预建的 PyBytes，
    // 完成路径直接把它作为 future 结果返回，省一次完整数据拷贝。
    // 短读（文件在读期间被外部截短）由完成路径负责收缩。
    PyObject      *preResult     = nullptr;

    char *buf() noexcept {
        if ((isReadinto || holdsWriteBuf) && userBufView.buf) return (char*)userBufView.buf;
        if (preResult) return PyBytes_AS_STRING(preResult);
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
        Py_XDECREF(preResult);
        if ((isReadinto || holdsWriteBuf) && userBufView.buf) {
            PyBuffer_Release(&userBufView);
        }
        Py_XDECREF(userBuf);
        if (poolBuf) pool_release(poolBuf);
        else if (!isReadinto && heapBuf) std::free(heapBuf);
    }
};

// ── IORequest freelist ──────────────────────────────────────────────────────
// 每 op new/delete 的分配器 churn 在小块高频 I/O 下可观。thread_local
// freelist 复用裸内存，placement new 重建对象（NSDMI/原子全部重置）。
// 与 Yuyuko 魂簿兼容：启用时照常在分配后登记、释放前注销，复用的地址
// 会被重新登记，check_access 不会误报。
namespace detail {
    static constexpr size_t REQ_FREELIST_MAX = 64;
    inline std::vector<void*>& req_freelist() {
        thread_local std::vector<void*> list;
        return list;
    }
    inline void* req_mem_acquire() {
        auto& fl = req_freelist();
        if (!fl.empty()) { void* p = fl.back(); fl.pop_back(); return p; }
        return ::operator new(sizeof(IORequest));
    }
    inline void req_mem_release(void* p) noexcept {
        auto& fl = req_freelist();
        if (fl.size() < REQ_FREELIST_MAX) { fl.push_back(p); return; }
        ::operator delete(p);
    }
} // namespace detail

#ifdef ENABLE_YUYUKO
inline IORequest* req_alloc_impl(const char* file, int line, const char* func) {
    IORequest* p = new (detail::req_mem_acquire()) IORequest;
    Yuyuko::register_new(p, sizeof(IORequest), file, line, func);
    return p;
}
inline void req_free_impl(IORequest* p, const char* file, int line, const char* func) {
    if (!p) return;
    if (Yuyuko::release_new(p, file, line, func)) {
        p->~IORequest();
        detail::req_mem_release(p);
    }
}
#else
inline IORequest* req_alloc_impl(const char*, int, const char*) {
    return new (detail::req_mem_acquire()) IORequest;
}
inline void req_free_impl(IORequest* p, const char*, int, const char*) {
    if (!p) return;
    p->~IORequest();
    detail::req_mem_release(p);
}
#endif

} // namespace ayafileio

// 调用点宏（带出处信息）
#define REQ_ALLOC()  ayafileio::req_alloc_impl(__FILE__, __LINE__, __FUNCTION__)
#define REQ_FREE(p)  ayafileio::req_free_impl((p), __FILE__, __LINE__, __FUNCTION__)
