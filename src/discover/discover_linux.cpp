#include "discover.hpp"

#ifdef HAVE_IO_URING

#include <liburing.h>
#include <cstdlib>
#include <cstring>

namespace ayafileio::discovery {

bool io_uring_available() {
    static const bool available = [] {
        io_uring ring;
        if (io_uring_queue_init(8, &ring, 0) == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    return available;
}

bool io_uring_sqpoll_supported() {
    static const bool supported = [] {
        if (!io_uring_available()) return false;
        io_uring ring;
        if (io_uring_queue_init(2, &ring, IORING_SETUP_SQPOLL) == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }();
    return supported;
}

std::unordered_map<std::string, core::CapabilityValue> io_uring_detail() {
    std::unordered_map<std::string, core::CapabilityValue> result;
    result["available"] = true;
    result["completion_model"] = true;

    io_uring_params params{};
    io_uring ring;
    if (io_uring_queue_init_params(8, &ring, &params) != 0) {
        result["probe_error"] = true;
        return result;
    }

    std::unordered_map<std::string, bool> features;
    const unsigned flags = params.features;
#ifdef IORING_FEAT_SINGLE_MMAP
    features["single_mmap"] = (flags & IORING_FEAT_SINGLE_MMAP) != 0;
#endif
#ifdef IORING_FEAT_NODROP
    features["nodrop"] = (flags & IORING_FEAT_NODROP) != 0;
#endif
#ifdef IORING_FEAT_SUBMIT_STABLE
    features["submit_stable"] = (flags & IORING_FEAT_SUBMIT_STABLE) != 0;
#endif
#ifdef IORING_FEAT_RW_CUR_POS
    features["rw_cur_pos"] = (flags & IORING_FEAT_RW_CUR_POS) != 0;
#endif
#ifdef IORING_FEAT_CUR_PERSONALITY
    features["cur_personality"] = (flags & IORING_FEAT_CUR_PERSONALITY) != 0;
#endif
#ifdef IORING_FEAT_FAST_POLL
    features["fast_poll"] = (flags & IORING_FEAT_FAST_POLL) != 0;
#endif
#ifdef IORING_FEAT_SQPOLL_NONFIXED
    features["sqpoll_nonfixed"] = (flags & IORING_FEAT_SQPOLL_NONFIXED) != 0;
#endif
#ifdef IORING_FEAT_EXT_ARG
    features["ext_arg"] = (flags & IORING_FEAT_EXT_ARG) != 0;
#endif
#ifdef IORING_FEAT_NATIVE_WORKERS
    features["native_workers"] = (flags & IORING_FEAT_NATIVE_WORKERS) != 0;
#endif
#ifdef IORING_FEAT_CQE_SKIP
    features["cqe_skip"] = (flags & IORING_FEAT_CQE_SKIP) != 0;
#endif
#ifdef IORING_FEAT_LINKED_FILE
    features["linked_file"] = (flags & IORING_FEAT_LINKED_FILE) != 0;
#endif
#ifdef IORING_FEAT_MIN_TIMEOUT
    features["min_timeout"] = (flags & IORING_FEAT_MIN_TIMEOUT) != 0;
#endif
    result["features"] = std::move(features);

    io_uring_probe* probe = io_uring_get_probe_ring(&ring);
    if (probe) {
        static const struct { int op; const char* name; } probe_ops[] = {
            {0, "nop"}, {1, "readv"}, {2, "writev"}, {3, "fsync"},
            {4, "read_fixed"}, {5, "write_fixed"}, {8, "sync_file_range"},
            {17, "fallocate"}, {18, "openat"}, {19, "close"}, {21, "statx"},
            {22, "read"}, {23, "write"}, {24, "fadvise"}, {25, "madvise"},
            {28, "openat2"}, {30, "splice"}, {35, "renameat"}, {36, "unlinkat"},
            {37, "mkdirat"}, {49, "read_multishot"},
        };
        std::unordered_map<std::string, bool> opcodes;
        for (const auto& item : probe_ops) {
            opcodes[item.name] = io_uring_opcode_supported(probe, item.op) != 0;
        }
        result["opcodes"] = std::move(opcodes);
        std::free(probe);
    } else {
        result["opcodes"] = nullptr;
    }

    io_uring_queue_exit(&ring);
    result["sqpoll"] = io_uring_sqpoll_supported();
    return result;
}

} // namespace ayafileio::discovery

#endif
