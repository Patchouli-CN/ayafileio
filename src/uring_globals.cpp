// src/uring_globals.cpp
#ifdef HAVE_IO_URING

#include "uring_pool.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

std::atomic<bool> g_uring_running{false};
std::mutex g_uring_instances_mtx;
std::unordered_map<void*, std::shared_ptr<UringInstance>> g_uring_instances;

#endif // HAVE_IO_URING