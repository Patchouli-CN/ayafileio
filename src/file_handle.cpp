#include "file_handle.hpp"
#include "discover.h"          // effective_backend()
#ifdef _WIN32
#include "backends/windows_io_backend.hpp"
#include "backends/thread_io_backend.hpp"
#elif defined(__APPLE__)
#include "backends/macos_gcd_backend.hpp"
#include "backends/thread_io_backend.hpp"
#else
#ifdef HAVE_IO_URING
#include "backends/io_uring_backend.hpp"
#endif
#include "backends/thread_io_backend.hpp"
#include "config.hpp"
#include "global_thread_pool.hpp"
#include "result_batcher.hpp"
#include <algorithm>
#include <thread>
#endif

namespace ayafileio {

// 从路径文件打开
FileHandle::FileHandle(const std::string &path, const std::string &mode)
    : FileHandle(path, mode, nullptr, nullptr) {}

FileHandle::FileHandle(const std::string &path, const std::string &mode,
                       PyObject *loop, PyObject *create_future) {
#ifndef _WIN32
    // POSIX 后端惰性绑定事件循环（首次 I/O 时），注入参数仅 Windows 使用
    (void)loop; (void)create_future;
#endif
    // 后端由 configure({"backend": ...}) 决定：Auto 走平台探测，强制值直接
    // 生效（可用性在 configure 时已校验，effective_backend 再兜一次底）。
    const BackendKind kind = effective_backend();

#ifdef _WIN32
    // Windows 目前只有 IOCP 后端：线程池后端的 Windows 移植还在收尾（原语层已
    // 就位，但全套用例下仍有崩溃/内存暴涨），configure({"backend": "thread_pool"})
    // 在 Windows 上会被 backend_available() 拒绝，走不到这里。
    if (kind == BackendKind::ThreadPool) {
        m_backend = new ThreadIOBackend(path, mode);
    } else if (loop) {
        // open_async 注入路径：工作线程无 running loop，用预取的 loop
        m_backend = new WindowsIOBackend(path, mode, loop, create_future);
    } else {
        m_backend = new WindowsIOBackend(path, mode);
    }
#else
#  ifdef __APPLE__
    if (kind == BackendKind::DispatchIo) {
        try {
            m_backend = new MacOSGCDBackend(path, mode);
            return;
        } catch (const std::exception &) {
            // GCD 后端创建失败 → 降级到线程池
        }
    }
#  endif
#  ifdef HAVE_IO_URING
    if (kind == BackendKind::IoUring) {
        try {
            m_backend = new IOUringBackend(path, mode);
            return;
        } catch (const std::exception &) {
            // io_uring 后端创建失败 → 降级到线程池
        }
    }
#  endif
    (void)kind;
    m_backend = new ThreadIOBackend(path, mode);
#endif
}

FileHandle::~FileHandle() {
    delete m_backend;
}

// 从 fd 打开
FileHandle::FileHandle(int fd, const std::string& mode, bool owns_fd) {
    const BackendKind kind = effective_backend();
#ifdef _WIN32
    (void)kind;
    m_backend = new WindowsIOBackend(fd, mode, owns_fd);
#else
#  ifdef __APPLE__
    if (kind == BackendKind::DispatchIo) {
        try {
            m_backend = new MacOSGCDBackend(fd, mode, owns_fd);
            return;
        } catch (const std::exception&) {
            // 降级到线程池
        }
    }
#  endif
#  ifdef HAVE_IO_URING
    if (kind == BackendKind::IoUring) {
        try {
            m_backend = new IOUringBackend(fd, mode, owns_fd);
            return;
        } catch (const std::exception&) {
            // 降级到线程池
        }
    }
#  endif
    (void)kind;
    m_backend = new ThreadIOBackend(fd, mode, owns_fd);
#endif
}

bool try_native_aopen(PyObject* loop, PyObject* future,
                      ResultBatcher* batcher,
                      const char* path, const char* mode) {
#ifdef HAVE_IO_URING
    // 后端被强制成 thread_pool 时不能悄悄走 io_uring 的原生 OPENAT：
    // 否则 configure({"backend": "thread_pool"}) 之后 aopen 仍然用 io_uring，
    // 能力矩阵和实际行为对不上，CI 的 thread_pool 覆盖也就落空了。
    if (effective_backend() != BackendKind::IoUring) return false;
    return submit_native_aopen(loop, future, batcher, path, mode);
#else
    (void)loop;
    (void)future;
    (void)batcher;
    (void)path;
    (void)mode;
    return false;
#endif
}

uint64_t native_aopen_submission_count() {
#ifdef HAVE_IO_URING
    return io_uring_native_aopen_submission_count();
#else
    return 0;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// 异步打开（原 bindings.cpp 的 open_async lambda）
//
// ── 引用计数契约（动这段之前请先读完）─────────────────────────────────────
// * `future`      ：CallNoArgs 产生，本函数返回给调用方（steal 一份）。
//                   线程池 worker 持有**自己**的那一份（Py_INCREF），结尾还回。
//                   曾经两者共享同一份引用：正常情况下 batcher 里 set_result
//                   绑定方法临时持有的那份恰好盖住窗口，但并发大量失败
//                   （如 EMFILE）触发立即 flush 时窗口会被击中 → future 提前
//                   释放 → await 方读到已释放对象 → 段错误（macOS CI 实测）。
// * `loop`        ：本函数持有自己的一份，worker 结尾还回去。Session 在
//                   create_session 里对 loop 自行 Py_INCREF、在 ~Session 里
//                   Py_DECREF——那是它自己那份。以前把本函数这份当成 Session
//                   的「续命来源」留着不放，于是每次 aopen() 泄漏一个事件循环
//                   引用，loop.close() 之后对象依然回收不了。
// * `create_future` / `set_result`：同样由 worker 结尾还回。
// ════════════════════════════════════════════════════════════════════════════

py::object py_open_async(const char* path, const char* mode) {
    PyObject* loop = PyObject_CallNoArgs(g_get_running_loop);
    if (!loop) throw py::python_error();
    PyObject* create_future = PyObject_GetAttr(loop, g_str_create_future);
    if (!create_future) { Py_DECREF(loop); throw py::python_error(); }
    PyObject* future = PyObject_CallNoArgs(create_future);
    if (!future) { Py_DECREF(create_future); Py_DECREF(loop); throw py::python_error(); }
    ResultBatcher* batcher = get_or_create_batcher(loop);
    PyObject* set_result = PyObject_GetAttr(future, g_str_set_result);
    if (!set_result) {
        Py_DECREF(future); Py_DECREF(create_future); Py_DECREF(loop);
        throw py::python_error();
    }

    batcher->op_submitted();

    // 平台分发：Linux 优先 io_uring OPENAT；不支持时返回 false。
    // 回退会继续使用同一个提交计数，由线程池完成路径负责配平。
    if (try_native_aopen(loop, future, batcher, path, mode)) {
        Py_DECREF(create_future);
        Py_DECREF(loop);
        Py_DECREF(set_result);
        return py::steal<py::object>(py::handle(future));
    }

    Py_INCREF(future);   // worker 自己那一份，见文件头的契约说明

    // GlobalThreadPool 是按需启动的：IOCP/uring/GCD 后端都不会启动
    // 它，只有线程池后端会 ensure_started——open_async 可能是第一个
    // 用户，必须先确保 worker 在跑，否则任务永远躺在队列里
    unsigned workers = ayafileio::config().io_worker_count();
    if (workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 1;
        workers = std::max(1u, std::min(hc * 2u, 16u));
    }
    GlobalThreadPool::instance().ensure_started(workers);

    std::string p(path), m(mode);
    GlobalThreadPool::instance().enqueue(
        [batcher, future, set_result, loop, create_future, p, m]() {
            PyGILState_STATE gs = PyGILState_Ensure();

            PyObject* set_fn = nullptr;
            PyObject* val = nullptr;
            try {
                auto* pf = new PyAsyncFile(p.c_str(), m.c_str(), loop, create_future);
                py::object obj = py::cast(pf, py::rv_policy::take_ownership);
                val = obj.ptr();
                Py_INCREF(val);   // batcher 持一个引用；obj 析构归还另一个
                set_fn = set_result;
                Py_INCREF(set_fn);
            } catch (py::python_error &e) {
                e.restore();
                val = fetch_current_exc();
                set_fn = PyObject_GetAttr(future, g_str_set_exception);
                if (!set_fn) PyErr_Clear();
            } catch (const py::builtin_exception &e) {
                // nanobind 内建异常（value_error 是返回 builtin_exception
                // 的工厂函数而非类型，不能直接 catch）。构造路径唯一来源
                // 是 parse_mode 的非法模式 → ValueError
                PyErr_SetString(g_ValueError, e.what());
                val = fetch_current_exc();
                set_fn = PyObject_GetAttr(future, g_str_set_exception);
                if (!set_fn) PyErr_Clear();
            } catch (const std::exception &e) {
                PyErr_SetString(g_OSError, e.what());
                val = fetch_current_exc();
                set_fn = PyObject_GetAttr(future, g_str_set_exception);
                if (!set_fn) PyErr_Clear();
            }

            // 与 complete_ok 相同的批量 flush 策略
            bool last = batcher->op_completed();
            if (set_fn && val) {
                bool threshold = batcher->push(set_fn, val);
                if (threshold || last) batcher->flush();
            } else {
                Py_XDECREF(set_fn);
                Py_XDECREF(val);
            }

            Py_DECREF(create_future);
            Py_DECREF(loop);
            Py_DECREF(future);
            Py_DECREF(set_result);
            PyGILState_Release(gs);
        });

    return py::steal<py::object>(py::handle(future));
}

} // namespace ayafileio