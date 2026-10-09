// ════════════════════════════════════════════════════════════════════════════
// discover.cpp — 运行时能力发现（实现）
// ════════════════════════════════════════════════════════════════════════════

#include "discover.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_IO_URING
#include <liburing.h>
#endif

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace ayafileio {

// ════════════════════════════════════════════════════════════════════════════
// OS / 内核版本
// ════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
std::string os_version() {
    // RtlGetVersion 不受 manifest 兼容模式影响，拿的是真实内核版本
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                         (unsigned long)vi.dwMajorVersion,
                         (unsigned long)vi.dwMinorVersion,
                         (unsigned long)vi.dwBuildNumber);
                return buf;
            }
        }
    }
    return "unknown";
}
#else
std::string os_version() {
    struct utsname u {};
    if (uname(&u) != 0) return "unknown";
    return std::string(u.sysname) + " " + u.release;
}
#endif

// ════════════════════════════════════════════════════════════════════════════
// Linux: io_uring
// ════════════════════════════════════════════════════════════════════════════

#ifdef HAVE_IO_URING

bool io_uring_sqpoll_supported() {
    // SQPOLL 实测：5.11 之前需要特权，版本号说了不算，建一个试试最诚实
    static const bool supported = []() {
        if (!io_uring_available()) return false;
        struct io_uring ring;
        if (io_uring_queue_init(2, &ring, IORING_SETUP_SQPOLL) == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    return supported;
}

bool io_uring_available() {
    static const bool available = []() {
        struct io_uring ring;
        if (io_uring_queue_init(8, &ring, 0) == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    return available;
}

py::dict io_uring_detail() {
    py::dict d;
    d["available"] = true;
    d["completion_model"] = true;  // SQE/CQE 提交-完成模型

    // 特性位：读 io_uring_queue_init_params 回填的 params.features——
    // 那是内核写进我们自己结构体的字段，uapi 自 5.1 起就有；不碰
    // ring.features（liburing 2.x 才把 features 收进 struct io_uring，
    // manylinux_2_28 的老 liburing 没有这个成员，发版 CI 实测编译失败）
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    struct io_uring ring;
    if (io_uring_queue_init_params(8, &ring, &params) != 0) {
        // 可用性探测通过但重建失败（fd 耗尽等瞬态）：报半个结果
        d["probe_error"] = true;
        return d;
    }

    // 逐位 #ifdef 防御：发行版 backport 的 liburing 头文件可能缺新宏
    unsigned f = params.features;
    py::dict feats;
#ifdef IORING_FEAT_SINGLE_MMAP
    feats["single_mmap"] = (f & IORING_FEAT_SINGLE_MMAP) != 0;
#endif
#ifdef IORING_FEAT_NODROP
    feats["nodrop"] = (f & IORING_FEAT_NODROP) != 0;
#endif
#ifdef IORING_FEAT_SUBMIT_STABLE
    feats["submit_stable"] = (f & IORING_FEAT_SUBMIT_STABLE) != 0;
#endif
#ifdef IORING_FEAT_RW_CUR_POS
    feats["rw_cur_pos"] = (f & IORING_FEAT_RW_CUR_POS) != 0;
#endif
#ifdef IORING_FEAT_CUR_PERSONALITY
    feats["cur_personality"] = (f & IORING_FEAT_CUR_PERSONALITY) != 0;
#endif
#ifdef IORING_FEAT_FAST_POLL
    feats["fast_poll"] = (f & IORING_FEAT_FAST_POLL) != 0;
#endif
#ifdef IORING_FEAT_SQPOLL_NONFIXED
    feats["sqpoll_nonfixed"] = (f & IORING_FEAT_SQPOLL_NONFIXED) != 0;
#endif
#ifdef IORING_FEAT_EXT_ARG
    feats["ext_arg"] = (f & IORING_FEAT_EXT_ARG) != 0;
#endif
#ifdef IORING_FEAT_NATIVE_WORKERS
    feats["native_workers"] = (f & IORING_FEAT_NATIVE_WORKERS) != 0;
#endif
#ifdef IORING_FEAT_CQE_SKIP
    feats["cqe_skip"] = (f & IORING_FEAT_CQE_SKIP) != 0;
#endif
#ifdef IORING_FEAT_LINKED_FILE
    feats["linked_file"] = (f & IORING_FEAT_LINKED_FILE) != 0;
#endif
#ifdef IORING_FEAT_MIN_TIMEOUT
    feats["min_timeout"] = (f & IORING_FEAT_MIN_TIMEOUT) != 0;
#endif
    d["features"] = feats;

    // opcode 能力探测：IORING_REGISTER_PROBE（内核 5.6+）。
    // 问内核"哪些操作真支持"，而不是按版本号猜——backport 内核上两者会不一致。
    //
    // opcode 编号是 uapi ABI（只增不改；编号对照内核
    // include/uapi/linux/io_uring.h 的 enum io_uring_op）。IORING_OP_* 是
    // 枚举常量而非宏，#ifdef 防御不到旧版头文件；直接用 ABI 编号反而更稳：
    // 编译不依赖头文件版本，运行时 io_uring_opcode_supported 自带边界检查
    // （op > last_op → 不支持）。
    struct io_uring_probe* probe = io_uring_get_probe_ring(&ring);
    if (probe) {
        static const struct { int op; const char* name; } kProbeOps[] = {
            { 0,  "nop" },
            { 1,  "readv" },
            { 2,  "writev" },
            { 3,  "fsync" },
            { 4,  "read_fixed" },
            { 5,  "write_fixed" },
            { 8,  "sync_file_range" },
            { 17, "fallocate" },
            { 18, "openat" },
            { 19, "close" },
            { 21, "statx" },
            { 22, "read" },
            { 23, "write" },
            { 24, "fadvise" },
            { 25, "madvise" },
            { 28, "openat2" },
            { 30, "splice" },
            { 35, "renameat" },
            { 36, "unlinkat" },
            { 37, "mkdirat" },
            { 49, "read_multishot" },
        };
        py::dict ops;
        for (const auto& k : kProbeOps) {
            ops[k.name] = io_uring_opcode_supported(probe, k.op) != 0;
        }
        d["opcodes"] = ops;
        // get_probe* 返回的是 malloc 内存；io_uring_free_probe 是
        // liburing 2.x 才有的包装，老版本直接 free（语义相同）
        free(probe);
    } else {
        d["opcodes"] = py::none();  // 内核 < 5.6，没有 PROBE 注册命令
    }

    io_uring_queue_exit(&ring);

    d["sqpoll"] = io_uring_sqpoll_supported();
    return d;
}

#endif // HAVE_IO_URING

// ════════════════════════════════════════════════════════════════════════════
// macOS: Dispatch I/O (GCD)
// ════════════════════════════════════════════════════════════════════════════

#ifdef __APPLE__

bool gcd_dispatch_io_available() {
    static const bool available = []() {
        // 尝试创建测试用的 dispatch queue
        dispatch_queue_t test_queue = dispatch_queue_create(
            "com.ayafileio.test",
            DISPATCH_QUEUE_SERIAL
        );
        if (!test_queue) {
            return false;
        }

        // 创建临时文件测试 Dispatch I/O
        char tmp_path[] = "/tmp/ayafileio_test_XXXXXX";
        int fd = mkstemp(tmp_path);
        if (fd == -1) {
            dispatch_release(test_queue);
            return false;
        }

        // 尝试创建 dispatch I/O channel
        dispatch_io_t test_channel = dispatch_io_create(
            DISPATCH_IO_RANDOM,
            fd,
            test_queue,
            ^(int error) {
                // cleanup handler - 文件描述符会在这里被关闭
            }
        );

        bool ok = (test_channel != nullptr);

        if (test_channel) {
            dispatch_io_close(test_channel, DISPATCH_IO_STOP);
            dispatch_release(test_channel);
        } else {
            // 如果 channel 创建失败，手动关闭 fd
            close(fd);
        }

        unlink(tmp_path);
        dispatch_release(test_queue);

        return ok;
    }();
    return available;
}

py::dict gcd_detail() {
    py::dict d;
    bool avail = gcd_dispatch_io_available();
    d["dispatch_io"] = avail;
    if (avail) {
        // 机制事实：GCD 文件通道由内核托管 workqueue 线程执行阻塞
        // pread/pwrite，不是 IOCP/io_uring 式的完成模型（libdispatch io.c
        // 的 _dispatch_operation_perform 就是一行 pread）；线程生命周期
        // 由内核 workqueue 机制管理，库本身不持有任何后台线程
        d["completion_model"] = false;
        d["kernel_managed_workqueue"] = true;
        d["mincore_fastpath"] = true;  // 缓存命中走 mincore + pread 内联
    }
    return d;
}

#endif // __APPLE__

// ════════════════════════════════════════════════════════════════════════════
// 跨平台汇总
// ════════════════════════════════════════════════════════════════════════════

py::dict backend_detail() {
#if defined(_WIN32)
    py::dict d;
    d["completion_model"] = true;  // IRP + IOCP：提交后无线程等待，中断报完成
    d["batch_harvest"] = true;     // GetQueuedCompletionStatusEx 批量收割
    d["handle_pool"] = true;       // 句柄池复用
    return d;
#elif defined(HAVE_IO_URING)
    if (!io_uring_available()) {
        py::dict d;
        d["available"] = false;
        d["completion_model"] = false;
        d["note"] = "thread pool fallback (io_uring unavailable)";
        return d;
    }
    return io_uring_detail();
#elif defined(__APPLE__)
    return gcd_detail();
#else
    py::dict d;
    d["completion_model"] = false;
    d["note"] = "thread pool fallback";
    return d;
#endif
}

} // namespace ayafileio
