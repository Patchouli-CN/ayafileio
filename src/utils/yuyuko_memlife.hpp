// yuyuko_memlife.hpp — 幽幽子内存生命周期追踪系统
//
// 编译：定义 ENABLE_ASAN → 全功能。不定义 → 零开销 stub。
//
// 特性：
//   - 追踪 new/delete 和 malloc/free 的完整生命周期
//   - Double Free 检测与拦截
//   - 多线程 Use-After-Free 检测
//   - 分配器不匹配检测（new vs malloc）
//   - CRC-32C 硬件加速内存完整性校验 (SSE4.2 / ARMv8-CRC)
//   - 内存泄漏检测（leak_check）
//   - 双缓冲异步日志：业务线程纳秒级返回，后台线程批量写文件
//
// 用法：
//   TRACKED_NEW(type)              → 追踪对象，使用 operator new/delete
//   TRACKED_ALLOC(size)            → 追踪原始内存块，使用 std::malloc/free
//   TRACKED_DELETE(ptr)            → 释放追踪对象
//   TRACKED_FREE(ptr)              → 释放追踪内存块
//   Yuyuko::check_access(ptr)      → 手动检查地址是否已被释放（基于魂簿）
//   Yuyuko::check_bounds(ptr,sz)   → 越界写入检测
//   Yuyuko::mem_snapshot(ptr)      → 保存内存 CRC 快照（4KB 粒度）
//   Yuyuko::mem_snapshot_ex(ptr,N) → 保存内存 CRC 快照（自定义 N 字节粒度）
//   Yuyuko::mem_check(ptr)         → 校验 CRC 快照，检测内存损坏
//   Yuyuko::mem_check_ex(ptr,N)    → 校验 CRC 快照（自定义粒度）
//   Yuyuko::leak_check()           → 程序退出时检测泄漏
//
// 异步日志：
//   Yuyuko::start_async_log("yuyuko.log")  → 启动异步日志线程
//   Yuyuko::stop_async_log()               → 停止并等待所有日志落盘
//   如果不调用 start_async_log，则使用同步 stderr 输出（兼容旧行为）

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <condition_variable>
#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ════════════════════════════════════════════════════════════════════════════
// 幽幽子命名空间
// ════════════════════════════════════════════════════════════════════════════

#ifdef ENABLE_ASAN

namespace Yuyuko {

// ════════════════════════════════════════════════════════════════════════════
// 前向声明（异步日志部分）
// ════════════════════════════════════════════════════════════════════════════
namespace detail {
    void async_log_callback(const char* msg);
}

// ════════════════════════════════════════════════════════════════════════════
// 工具函数
// ════════════════════════════════════════════════════════════════════════════

/// 线程本地缓存: thread_id (只计算一次)
inline const char* cached_thread_id() {
    thread_local char buf[20]={};
    if (buf[0]=='\0') {
        auto id=std::hash<std::thread::id>{}(std::this_thread::get_id());
        snprintf(buf,sizeof(buf),"0x%zx",id);
    }
    return buf;
}
/// 线程本地缓存: thread_name (只查询一次)
inline const char* cached_thread_name() {
    thread_local char buf[64]={};
    if (buf[0]=='\0') {
#ifdef _WIN32
        PWSTR name=nullptr;
        if(SUCCEEDED(GetThreadDescription(GetCurrentThread(),&name))&&name){
            WideCharToMultiByte(CP_UTF8,0,name,-1,buf,sizeof(buf),nullptr,nullptr);
            LocalFree(name);
        }else{snprintf(buf,sizeof(buf),"tid-%s",cached_thread_id());}
#else
        snprintf(buf,sizeof(buf),"tid-%s",cached_thread_id());
#endif
    }
    return buf;
}

/// 获取当前时间戳（微秒级，相对于启动时间）
static uint64_t current_time_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

/// 自动单位换算：微秒 → 人类可读时间字符串
/// @return 指向内部 thread_local 缓冲区的指针（下次调用会覆盖）
inline const char* format_duration_us(uint64_t us) {
    thread_local char buf[32];
    if (us < 1000)
        snprintf(buf, sizeof(buf), "%lluμs", (unsigned long long)us);
    else if (us < 1000000)
        snprintf(buf, sizeof(buf), "%.2fms", us / 1000.0);
    else if (us < 60000000)
        snprintf(buf, sizeof(buf), "%.2fs", us / 1000000.0);
    else {
        auto min = us / 60000000;
        auto sec = (us % 60000000) / 1000000;
        snprintf(buf, sizeof(buf), "%llumin%llus",
                 (unsigned long long)min, (unsigned long long)sec);
    }
    return buf;
}

// ════════════════════════════════════════════════════════════════════════════
// 双缓冲异步日志引擎
// ════════════════════════════════════════════════════════════════════════════

namespace detail {

// 日志缓冲区配置
static constexpr size_t ASYNC_BUF_CAPACITY = 4096;   // 单条日志最大长度
static constexpr size_t BUFFER_COUNT       = 64;      // 每个 EventBuffer 可容纳的日志条数

// 单个日志缓冲区
struct LogBuffer {
    char messages[BUFFER_COUNT][ASYNC_BUF_CAPACITY];
    size_t count = 0;  // 改用普通 size_t，手动管理
    
    bool full() const {
        return count >= BUFFER_COUNT;
    }
};

// 双缓冲区 — 用指针交换代替 memcpy 256KB
static LogBuffer  g_buf_A, g_buf_B;
static LogBuffer* g_active = &g_buf_A;   // 业务线程写入
static LogBuffer* g_flush  = &g_buf_B;   // 后台线程刷盘
static std::mutex g_log_mtx;             // 保护 g_active 写入 + 指针交换

// 后台线程控制
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
static std::jthread g_log_thread;
#else
static std::thread g_log_thread;
#endif
static std::atomic<bool> g_log_stop{false};
static std::condition_variable g_log_cv;
static std::mutex g_log_cv_mtx;
static FILE* g_log_file = nullptr;
static bool g_async_mode = false;

/// 指针交换 (只改 2 个指针, 不拷贝 256KB 数据)
static void swap_log_buffers() {
    LogBuffer* t = g_active; g_active = g_flush; g_flush = t;
    g_active->count = 0;
}

/// 业务线程推送日志 — g_log_mtx 加锁
static void push_log(const char* msg, size_t len) {
    if (!g_async_mode) {
        static std::mutex sync_mtx;
        std::lock_guard<std::mutex> lk(sync_mtx);
        fwrite(msg,1,len,stderr); fflush(stderr);
        return;
    }
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        if (g_active->count < BUFFER_COUNT) {
            size_t n = (len < ASYNC_BUF_CAPACITY-1) ? len : ASYNC_BUF_CAPACITY-1;
            memcpy(g_active->messages[g_active->count], msg, n);
            g_active->messages[g_active->count][n] = '\0';
            g_active->count++;
        }
        notify = g_active->full();
    }
    if (notify) g_log_cv.notify_one();
}

// 后台线程：批量消费日志
static void log_worker() {
    while (!g_log_stop.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(g_log_cv_mtx);
            g_log_cv.wait_for(lk, std::chrono::milliseconds(50),
                []{ return g_log_stop.load(std::memory_order_relaxed); });
        }
        { std::lock_guard<std::mutex> lk(g_log_mtx); swap_log_buffers(); }
        if (g_log_file) {
            for (size_t i=0; i<g_flush->count; ++i) fputs(g_flush->messages[i], g_log_file);
            fflush(g_log_file);
        }
        g_flush->count = 0;
    }
    if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; }
}

} // namespace detail

// ════════════════════════════════════════════════════════════════════════════
// 异步日志公共 API
// ════════════════════════════════════════════════════════════════════════════

/// 启动异步日志线程，所有日志输出到指定文件
/// @param filepath 日志文件路径
inline void start_async_log(const char* filepath = "yuyuko_memory.log") {
    if (detail::g_async_mode) return;  // 已经启动了
    
    detail::g_log_file = fopen(filepath, "w");
    if (!detail::g_log_file) {
        // 如果文件打不开，回退到 stderr 同步模式
        fprintf(stderr, "[Yuyuko] 无法打开日志文件 %s，将使用 stderr 同步输出\n", filepath);
        return;
    }
    
    detail::g_async_mode = true;
    detail::g_log_stop.store(false);
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    detail::g_log_thread = std::jthread(detail::log_worker);
#else
    detail::g_log_thread = std::thread(detail::log_worker);
#endif
    
    // 写一条启动日志
    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), 
        "[Yuyuko] 异步日志引擎启动，输出文件: %s\n", filepath);
    detail::push_log(buf, static_cast<size_t>(len));
}

/// 停止异步日志线程，等待所有日志落盘
inline void stop_async_log() {
    if (!detail::g_async_mode) return;
    
    // 写一条停止日志
    char buf[detail::ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), "[Yuyuko] 异步日志引擎停止\n");
    detail::push_log(buf, static_cast<size_t>(len));
    
    detail::g_log_stop.store(true);
    detail::g_log_cv.notify_one();

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    detail::g_log_thread = std::jthread{};
#else
    if (detail::g_log_thread.joinable()) {
        detail::g_log_thread.join();
    }
#endif

    detail::g_async_mode = false;
}

// ════════════════════════════════════════════════════════════════════════════
// 内部日志宏（供系统内部使用）
// ════════════════════════════════════════════════════════════════════════════

namespace detail {

// 内部日志函数：格式化并推送到日志系统
template<typename... Args>
static void yuyuko_logln(const char* fmt, Args... args) {
    char buf[ASYNC_BUF_CAPACITY];
    int len = snprintf(buf, sizeof(buf), fmt, args...);
    if (len > 0 && static_cast<size_t>(len) < sizeof(buf) - 2) {
        buf[len] = '\n';
        buf[len + 1] = '\0';
        push_log(buf, static_cast<size_t>(len + 1));
    } else {
        push_log(buf, static_cast<size_t>(len));
    }
}

} // namespace detail

// ════════════════════════════════════════════════════════════════════════════
// 分配器类型
// ════════════════════════════════════════════════════════════════════════════

/// 内存分配来源
enum class AllocSource : uint8_t {
    OPERATOR_NEW,   ///< 通过 operator new / operator delete 分配
    STD_MALLOC      ///< 通过 std::malloc / std::free 分配
};

// ════════════════════════════════════════════════════════════════════════════
// 灵魂记录
// ════════════════════════════════════════════════════════════════════════════

/// 单条内存分配记录 (固定大小 char 数组, 零堆分配)
/// 存活/释放状态由所在容器决定：g_soul_alive 或 g_soul_released
struct SoulRecord {
    void*    ptr;
    size_t   size;
    uint64_t timestamp;       // 分配/最后操作的时间戳 (μs)
    uint16_t line;            // 行号
    uint8_t  source;          // AllocSource
    char     _pad[1];         // 保持结构体对齐不变
    char     file[80];        // 源文件名
    char     func[64];        // 函数名
    char     thread_name[64]; // 线程名
    char     thread_id[20];   // 线程 ID (hex)
};
static_assert(sizeof(SoulRecord) <= 256, "SoulRecord too large");

/// 全局魂簿：三表分离
///   alive      — 存活分配 (leak_check 扫描此表)
///   released   — 已释放历史 (UAF / Double-Free 检测)
///   permanent  — 常驻对象标记 (排除出 leak_check, 如连接池/全局缓存)
static std::shared_mutex g_soul_mtx;
static std::unordered_map<void*, SoulRecord> g_soul_alive;
static std::unordered_map<void*, SoulRecord> g_soul_released;
static std::unordered_set<void*> g_soul_permanent;
/// 按地址排序的索引 — check_bounds 二分查找用 (O(log n))
static std::set<uintptr_t> g_soul_alive_range;
static std::set<uintptr_t> g_soul_released_range;
static bool _soul_book_init = []{
    g_soul_alive.reserve(65536);
    g_soul_released.reserve(16384);
    return true;
}();

/// 在魂簿中查找记录（先查 alive 再查 released，调用方需持有 g_soul_mtx）
static SoulRecord* find_soul(void* ptr) {
    auto it = g_soul_alive.find(ptr);
    if (it != g_soul_alive.end()) return &it->second;
    auto rit = g_soul_released.find(ptr);
    return (rit != g_soul_released.end()) ? &rit->second : nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
// 内部：统一的注册/释放实现
// ════════════════════════════════════════════════════════════════════════════

/// 截断安全拷贝
inline void str_copy(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0]='\0'; return; }
    size_t i=0; while(i+1<cap && src[i]){dst[i]=src[i];++i;} dst[i]='\0';
}

/// 注册一次内存分配（内部函数）
static void register_soul_impl(void* ptr, size_t size, AllocSource source,
                                const char* file, int line, const char* func) {
    SoulRecord sr;
    sr.ptr=ptr; sr.size=size;
    sr.source=static_cast<uint8_t>(source);
    sr.timestamp=current_time_us();
    sr.line=static_cast<uint16_t>(line);
    str_copy(sr.file,sizeof(sr.file),file);
    str_copy(sr.func,sizeof(sr.func),func);
    str_copy(sr.thread_name,sizeof(sr.thread_name),cached_thread_name());
    str_copy(sr.thread_id,sizeof(sr.thread_id),cached_thread_id());

    {
        std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
        // 清理前世记录：地址复用后不再是亡灵
        auto rit = g_soul_released.find(ptr);
        if (rit != g_soul_released.end()) {
            g_soul_released_range.erase(reinterpret_cast<uintptr_t>(ptr));
            g_soul_released.erase(rit);
        }
        // 如果该地址曾被标记为常驻，清除标记（新对象未必还是常驻）
        g_soul_permanent.erase(ptr);
        g_soul_alive[ptr] = sr;
        g_soul_alive_range.insert(reinterpret_cast<uintptr_t>(ptr));
    }

    detail::yuyuko_logln(
        "[Yuyuko] ALLOC %p size=%zu src=%s @ %s:%d %s() [%s/%s]",
        ptr, size,
        (source==AllocSource::OPERATOR_NEW)?"new":"malloc",
        file,line,func,sr.thread_name,sr.thread_id);
}

/// 记录一次内存释放（内部函数）
/// 将记录从 g_soul_alive 移送至 g_soul_released
/// @return true if the caller should free the memory
static bool release_soul_impl(void* ptr, AllocSource source,
                               const char* file, int line, const char* func) {
    if (!ptr) return false;
    const char* tn = cached_thread_name();
    const char* ti = cached_thread_id();
    uint64_t now = current_time_us();
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    std::unique_lock<std::shared_mutex> lk(g_soul_mtx);

    // ── 已释放检测 (Double Free) ──
    auto rit = g_soul_released.find(ptr);
    if (rit != g_soul_released.end()) {
        detail::yuyuko_logln("[Yuyuko] DOUBLE-FREE %p freed %s ago\n"
            "  1st: [%s/%s] %s:%d %s()\n  2nd: [%s/%s] %s:%d %s()",
            ptr, format_duration_us(now - rit->second.timestamp),
            rit->second.thread_name,rit->second.thread_id,
            rit->second.file,rit->second.line,rit->second.func,
            tn,ti,file,line,func);
        return false;
    }

    // ── 查找存活记录 ──
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) {
        detail::yuyuko_logln("[Yuyuko] FREE-UNKNOWN %p @ %s:%d [%s/%s]",ptr,file,line,tn,ti);
        return false;
    }

    SoulRecord& s = ait->second;

    // ── 分配器不匹配检测 ──
    if (s.source != static_cast<uint8_t>(source)) {
        detail::yuyuko_logln("[Yuyuko] MISMATCH %p reg=%s tried=%s @ %s:%d [%s/%s]",
            ptr,(s.source==0)?"new":"malloc",
            (source==AllocSource::OPERATOR_NEW)?"new":"malloc",file,line,tn,ti);
        return false;
    }

    // ── 正常释放：alive → released ──
    detail::yuyuko_logln("[Yuyuko] FREE %p life=%s src=%s [%s/%s]",
        ptr, format_duration_us(now - s.timestamp),
        (source==AllocSource::OPERATOR_NEW)?"new":"malloc",tn,ti);
    s.timestamp = now;
    g_soul_released.emplace(ptr, std::move(s));
    g_soul_alive.erase(ait);
    g_soul_alive_range.erase(addr);
    g_soul_released_range.insert(addr);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// 公开 API
// ════════════════════════════════════════════════════════════════════════════

inline void register_new(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::OPERATOR_NEW, file, line, func);
}

inline void register_malloc(void* ptr, size_t size, const char* file, int line, const char* func) {
    register_soul_impl(ptr, size, AllocSource::STD_MALLOC, file, line, func);
}

/// Bookkeeping only — does NOT free memory. Returns true if caller should free.
inline bool release_new(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::OPERATOR_NEW, file, line, func);
}

/// Bookkeeping only — does NOT free memory. Returns true if caller should free.
inline bool release_malloc(void* ptr, const char* file, int line, const char* func) {
    return release_soul_impl(ptr, AllocSource::STD_MALLOC, file, line, func);
}

/// 检查地址是否可安全访问 (use-after-free 检测)
inline bool check_access(void* ptr, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_released.find(ptr);
    if (it == g_soul_released.end()) return true;
    detail::yuyuko_logln(
        "[Yuyuko] USE-AFTER-FREE %p @ %s:%d %s() — %s ago [%s/%s]",
        ptr,file,line,func,format_duration_us(current_time_us() - it->second.timestamp),
        it->second.thread_name,it->second.thread_id);
    return false;
}

/// 在已分配块中查找包含地址范围 [ps, pe) 的 SoulRecord (O(log n))
/// 先查 alive 再查 released。调用方需持有 g_soul_mtx
static SoulRecord* find_containing_soul(uintptr_t ps, uintptr_t pe) {
    auto lookup = [&](const std::set<uintptr_t>& range,
                     const std::unordered_map<void*, SoulRecord>& map) -> SoulRecord* {
        if (range.empty()) return nullptr;
        auto it = range.upper_bound(ps);
        if (it == range.begin()) return nullptr;
        --it;
        auto sit = map.find(reinterpret_cast<void*>(*it));
        if (sit == map.end()) return nullptr;
        uintptr_t as = reinterpret_cast<uintptr_t>(sit->second.ptr);
        return (ps >= as && pe <= as + sit->second.size) ? const_cast<SoulRecord*>(&sit->second) : nullptr;
    };
    SoulRecord* s = lookup(g_soul_alive_range, g_soul_alive);
    return s ? s : lookup(g_soul_released_range, g_soul_released);
}

/// 检查 [ptr, ptr+size) 写入是否在已分配范围内 (越界检测)
/// O(1) 快路径 (ptr 精确命中分配起始) + O(log n) 慢路径 (内部/偏移指针)
inline bool check_bounds(void* ptr, size_t size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    uintptr_t ps = reinterpret_cast<uintptr_t>(ptr), pe = ps + size;

    // ── 快路径：ptr 恰好是某个已知分配 ──
    auto ait = g_soul_alive.find(ptr);
    if (ait != g_soul_alive.end()) {
        SoulRecord& s = ait->second;
        uintptr_t ae = reinterpret_cast<uintptr_t>(s.ptr) + s.size;
        if (pe > ae) {
            detail::yuyuko_logln("[Yuyuko] BUFFER-OVERFLOW %p+%zu @ %s:%d alloc=%zu @ %s:%d",
                ptr, size, file, line, s.size, s.file, s.line);
            return false;
        }
        return true;
    }

    // ── 慢路径：ptr 在已分配块的内部或前方 — 二分查找 (O(log n)) ──
    SoulRecord* s = find_containing_soul(ps, pe);
    if (!s) {
        // 不属于任何已知分配 — 可能是野指针
        detail::yuyuko_logln("[Yuyuko] WILD-WRITE %p+%zu @ %s:%d — not in any tracked allocation",
            ptr, size, file, line);
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// mem_check — CRC 分块快照检测裸写 / 绕过宏的非法内存操作
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr size_t CRC_CHUNK_SIZE = 4096;  // 分块 CRC 粒度

// ═══════════════════════════════════════════════════════════════════════════════
// CRC-32C (Castagnoli) — 多项式 0x1EDC6F41 (反射: 0x82F63B78)
// 硬件加速: x86 SSE4.2 (_mm_crc32_u64) / ARMv8-A (__crc32cd)
// ═══════════════════════════════════════════════════════════════════════════════

// ── 硬件 CRC-32C 分发 ────────────────────────────────────────────────────────

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#define YUYUKO_HW_CRC32C 1
namespace detail {
inline uint32_t hw_crc32c(uint32_t crc, const uint8_t* data, size_t len) {
    // 8 字节块 — _mm_crc32_u64 一次处理 8B, ~1 cycle/B
    while (len >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data, 8);  // 安全非对齐加载
        crc = (uint32_t)_mm_crc32_u64(crc, chunk);
        data += 8; len -= 8;
    }
    // 4 字节尾巴
    if (len >= 4) {
        uint32_t chunk;
        std::memcpy(&chunk, data, 4);
        crc = _mm_crc32_u32(crc, chunk);
        data += 4; len -= 4;
    }
    // 1 字节尾巴
    while (len-- > 0)
        crc = _mm_crc32_u8(crc, *data++);
    return crc;
}
} // namespace detail

#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define YUYUKO_HW_CRC32C 1
namespace detail {
inline uint32_t hw_crc32c(uint32_t crc, const uint8_t* data, size_t len) {
    while (len >= 8) {
        crc = __crc32cd(crc, *(const uint64_t*)data);
        data += 8; len -= 8;
    }
    while (len >= 4) {
        crc = __crc32cw(crc, *(const uint32_t*)data);
        data += 4; len -= 4;
    }
    while (len-- > 0)
        crc = __crc32cb(crc, *data++);
    return crc;
}
} // namespace detail
#endif

// ── 软件 CRC-32C 查表 (硬件不可用时) ──────────────────────────────────────────

namespace detail {
inline const uint32_t* crc32c_table() {
    static const uint32_t* tbl = []() -> const uint32_t* {
        static uint32_t t[256];
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1) ? 0x82F63B78u : 0);
            t[i] = c;
        }
        return t;
    }();
    return tbl;
}
} // namespace detail

/// CRC-32C 更新: 硬件加速优先, 回退到软件查表
inline uint32_t crc32c_update(uint32_t crc, const uint8_t* data, size_t len) {
#ifdef YUYUKO_HW_CRC32C
    return detail::hw_crc32c(crc, data, len);
#else
    const uint32_t* tbl = detail::crc32c_table();
    for (size_t i = 0; i < len; ++i)
        crc = tbl[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
#endif
}

/// 分块 CRC: 每 chunk_size 字节存一个 CRC-32C
/// 小块只存 1 个，大块存 ceil(size/chunk_size) 个
inline std::vector<uint32_t> crc32_chunked(const uint8_t* data, size_t size, size_t chunk_size) {
    if (chunk_size == 0) chunk_size = CRC_CHUNK_SIZE;
    std::vector<uint32_t> crcs;
    size_t n_chunks = (size + chunk_size - 1) / chunk_size;
    if (n_chunks == 0) n_chunks = 1;
    crcs.reserve(n_chunks);
    for (size_t off = 0; off < size; off += chunk_size) {
        size_t len = (off + chunk_size <= size) ? chunk_size : (size - off);
        crcs.push_back(crc32c_update(0xFFFFFFFFu, data + off, len));
    }
    return crcs;
}

/// 默认 4KB 分块 CRC（向后兼容）
inline std::vector<uint32_t> crc32_chunked(const uint8_t* data, size_t size) {
    return crc32_chunked(data, size, CRC_CHUNK_SIZE);
}

/// 内存快照记录 — 存每块 CRC 向量
struct MemCheckpoint {
    std::vector<uint32_t> crcs;
    uint64_t timestamp;
    size_t   chunk_size;  // CRC 分块粒度 (bytes)
    char     tag[32];
};

// ═══════════════════════════════════════════════════════════════════════════════
// 全局 CRC 检查点存储
// ═══════════════════════════════════════════════════════════════════════════════

static std::unordered_map<void*, MemCheckpoint> g_checkpoints;
static std::shared_mutex g_cp_mtx;

/// 保存 CRC 快照 (线程安全)，指定 chunk_size
inline void mem_snapshot_ex(void* ptr, size_t chunk_size, const char* tag = "default") {
    if (!ptr) return;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_alive.find(ptr);
    if (it == g_soul_alive.end()) return;
    size_t sz = it->second.size;
    lk.unlock();
    MemCheckpoint cp;
    cp.crcs = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, chunk_size);
    cp.timestamp = current_time_us();
    cp.chunk_size = chunk_size;
    { size_t i = 0; while (i < sizeof(cp.tag)-1 && tag[i]) { cp.tag[i]=tag[i]; ++i; } cp.tag[i]='\0'; }
    std::unique_lock<std::shared_mutex> clk(g_cp_mtx);
    g_checkpoints[ptr] = std::move(cp);
}

/// 保存 CRC 快照 (线程安全)，默认 4KB 粒度
inline void mem_snapshot(void* ptr, const char* tag = "default") {
    mem_snapshot_ex(ptr, CRC_CHUNK_SIZE, tag);
}

/// 校验快照 — 返回 true=一致 (使用快照时保存的 chunk_size)
inline bool mem_verify(void* ptr) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) return false;
    size_t sz = ait->second.size;
    slk.unlock();
    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto cit = g_checkpoints.find(ptr);
    if (cit == g_checkpoints.end()) return true;
    size_t cs = cit->second.chunk_size ? cit->second.chunk_size : CRC_CHUNK_SIZE;
    auto now = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, cs);
    return now == cit->second.crcs;
}

/// mem_check_ex: 检测裸写/野写，指定 chunk 粒度精确定位损坏
/// @param chunk_size 回退粒度（仅当快照无记录时使用，已有快照则用快照保存的粒度）
/// @return true=完整, false=已损坏
inline bool mem_check_ex(void* ptr, size_t chunk_size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    auto ait = g_soul_alive.find(ptr);
    if (ait == g_soul_alive.end()) {
        if (g_soul_released.find(ptr) != g_soul_released.end())
            detail::yuyuko_logln("[Yuyuko] CHECK %p FREED @ %s:%d %s()",ptr,file,line,func);
        else
            detail::yuyuko_logln("[Yuyuko] CHECK %p NOT TRACKED @ %s:%d %s()",ptr,file,line,func);
        return false;
    }
    const SoulRecord& s = ait->second;
    size_t sz = s.size;
    const char* af = s.file; int al = s.line;
    const char* ath = s.thread_name; const char* ati = s.thread_id;
    slk.unlock();

    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto cit = g_checkpoints.find(ptr);
    if (cit == g_checkpoints.end()) {
        clk.unlock();
        detail::yuyuko_logln("[Yuyuko] CHECK %p NO-SNAPSHOT @ %s:%d %s() — mem_snapshot first",
            ptr,file,line,func);
        return false;
    }
    const auto& old = cit->second.crcs;
    uint64_t elapsed = current_time_us() - cit->second.timestamp;
    const char* tag = cit->second.tag;
    // 使用快照时保存的粒度（若为 0 则回退到参数 chunk_size）
    size_t cs = cit->second.chunk_size ? cit->second.chunk_size : chunk_size;

    auto now_crcs = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, cs);

    if (now_crcs.size() != old.size()) {
        clk.unlock();
        detail::yuyuko_logln("[Yuyuko] CHECK %p SIZE-CHANGE chunks %zu→%zu @ %s:%d %s()",
            ptr, old.size(), now_crcs.size(), file, line, func);
        return false;
    }

    // 逐块比对 — 使用实际 chunk_size 计算偏移
    if (cs == 0) cs = CRC_CHUNK_SIZE;
    for (size_t i = 0; i < now_crcs.size(); ++i) {
        if (now_crcs[i] != old[i]) {
            size_t corrupt_off = i * cs;
            size_t corrupt_end = (corrupt_off + cs < sz) ? corrupt_off + cs : sz;
            clk.unlock();
            detail::yuyuko_logln(
                "[Yuyuko] MEM-CORRUPTION %p chunk[%zu] offset=%zu-%zu size=%zu cs=%zu @ %s:%d %s()\n"
                "  snapshot: [%s] crc[%zu]=0x%08X (%s ago)\n"
                "  now:      crc[%zu]=0x%08X (alloc'd %s:%d [%s/%s])",
                ptr, i, corrupt_off, corrupt_end, sz, cs, file, line, func,
                tag, i, old[i], format_duration_us(elapsed),
                i, now_crcs[i], af, al, ath, ati);
            return false;
        }
    }
    return true;
}

/// mem_check: 检测裸写/野写 — 默认 4KB 粒度（向后兼容）
inline bool mem_check(void* ptr, const char* file, int line, const char* func) {
    return mem_check_ex(ptr, CRC_CHUNK_SIZE, file, line, func);
}

// ═══════════════════════════════════════════════════════════════════════════════
// RAII 守卫 — 构造快照, 析构校验, 零手动负担
// ═══════════════════════════════════════════════════════════════════════════════

/// 内存守卫: 作用域内自动保护一块内存（默认 4KB CRC 粒度）
/// 构造时 MEM_SNAPSHOT, 析构时 MEM_CHECK, 损坏时日志自动包含调用点
class MemGuard {
    void*  m_ptr;
    const char* m_file;
    int    m_line;
    const char* m_func;
    bool   m_active;
public:
    MemGuard(void* ptr, const char* tag, const char* file, int line, const char* func)
        : m_ptr(ptr), m_file(file), m_line(line), m_func(func), m_active(ptr != nullptr) {
        if (m_active) mem_snapshot(ptr, tag);
    }
    ~MemGuard() {
        if (m_active) mem_check(m_ptr, m_file, m_line, m_func);
    }
    MemGuard(const MemGuard&) = delete;
    MemGuard& operator=(const MemGuard&) = delete;

    /// 提前释放守卫 (不再在析构时校验)
    void dismiss() { m_active = false; }
};

/// 内存守卫 (精确粒度版): 作用域内自动保护一块内存，可指定 CRC 分块精度
/// 构造时 mem_snapshot_ex(ptr, chunk_size), 析构时 mem_check_ex(ptr, chunk_size)
class MemGuardEx {
    void*  m_ptr;
    size_t m_chunk_size;
    const char* m_file;
    int    m_line;
    const char* m_func;
    bool   m_active;
public:
    MemGuardEx(void* ptr, size_t chunk_size, const char* tag,
               const char* file, int line, const char* func)
        : m_ptr(ptr), m_chunk_size(chunk_size), m_file(file), m_line(line), m_func(func), m_active(ptr != nullptr) {
        if (m_active) mem_snapshot_ex(ptr, chunk_size, tag);
    }
    ~MemGuardEx() {
        if (m_active) mem_check_ex(m_ptr, m_chunk_size, m_file, m_line, m_func);
    }
    MemGuardEx(const MemGuardEx&) = delete;
    MemGuardEx& operator=(const MemGuardEx&) = delete;

    /// 提前释放守卫 (不再在析构时校验)
    void dismiss() { m_active = false; }
};

/// 批量守卫: 同时保护多个内存块
class MemGuardBatch {
    struct Entry { void* ptr; const char* file; int line; const char* func; };
    std::vector<Entry> m_entries;
    bool m_active = true;
public:
    MemGuardBatch() = default;

    /// 添加一个内存块到批量守卫
    void add(void* ptr, const char* tag, const char* file, int line, const char* func) {
        if (ptr) {
            mem_snapshot(ptr, tag);
            m_entries.push_back({ptr, file, line, func});
        }
    }

    ~MemGuardBatch() {
        if (!m_active) return;
        for (auto& e : m_entries)
            mem_check(e.ptr, e.file, e.line, e.func);
    }

    MemGuardBatch(const MemGuardBatch&) = delete;
    MemGuardBatch& operator=(const MemGuardBatch&) = delete;
    void dismiss() { m_active = false; }
};

inline bool is_released(void* ptr) {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_released.find(ptr) != g_soul_released.end();
}
inline size_t get_size(void* ptr) {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    {
        auto it = g_soul_alive.find(ptr);
        if (it != g_soul_alive.end()) return it->second.size;
    }
    {
        auto it = g_soul_released.find(ptr);
        if (it != g_soul_released.end()) return it->second.size;
    }
    return 0;
}
inline size_t get_total_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_alive.size() + g_soul_released.size();
}
inline size_t get_alive_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_alive.size();  // 不含 permanent — 那是用户主动标记的
}
inline size_t get_permanent_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_permanent.size();
}

/// 标记常驻对象 — 排除出 leak_check
/// 用于连接池、全局缓存、socket 等程序生命周期内不释放的对象
inline void mark_permanent(void* ptr) {
    if (!ptr) return;
    std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
    auto it = g_soul_alive.find(ptr);
    if (it == g_soul_alive.end()) return;
    g_soul_permanent.insert(ptr);
}

/// 程序退出时检测内存泄漏 — O(leaks) 只遍历存活表, 跳过常驻对象
inline size_t leak_check() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    size_t total = g_soul_alive.size() + g_soul_released.size();
    size_t leaks = 0;
    for (auto& kv : g_soul_alive) {
        if (g_soul_permanent.count(kv.first)) continue;  // 跳过常驻对象
        const SoulRecord& sr = kv.second;
        ++leaks;
        detail::yuyuko_logln(
            "[Yuyuko] LEAK %p %zub [%s/%s] %s:%d %s() age=%s",
            sr.ptr,sr.size,sr.thread_name,sr.thread_id,
            sr.file,sr.line,sr.func,
            format_duration_us(current_time_us() - sr.timestamp));
    }
    size_t permanent = g_soul_permanent.size();
    detail::yuyuko_logln(leaks ? "[Yuyuko] leak: %zu/%zu (permanent: %zu)"
                               : "[Yuyuko] no leaks (permanent: %zu/%zu)",
                         leaks, total, permanent);
    return leaks;
}

/// 生成 HTML 内存状态报告
/// @param filepath 输出文件路径（如 "yuyuko_report.html"）
/// @param title 报告标题
inline void generate_html_report(const char* filepath = "yuyuko_report.html",
                                  const char* title = "幽幽子·内存魂簿报告") {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);

    FILE* f = fopen(filepath, "w");
    if (!f) return;

    // 统计
    size_t total     = g_soul_alive.size() + g_soul_released.size();
    size_t alive     = g_soul_alive.size();
    size_t permanent = g_soul_permanent.size();
    size_t released  = g_soul_released.size();
    size_t leaked_bytes = 0, total_bytes = 0;
    for (const auto& kv : g_soul_alive) {
        total_bytes += kv.second.size;
        if (!g_soul_permanent.count(kv.first)) leaked_bytes += kv.second.size;
    }
    for (const auto& kv : g_soul_released) total_bytes += kv.second.size;
    uint64_t now = current_time_us();

    // ═══════════ CSS + HTML 头部 (白玉楼主题) ═══════════
    fprintf(f,
R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>%s</title>
<style>
  *,*::before,*::after{margin:0;padding:0;box-sizing:border-box;}
  body{font-family:'Segoe UI','PingFang SC','Noto Serif SC','Microsoft YaHei',serif;background:linear-gradient(170deg,#0d0d1a 0%%,#1a0a2e 30%%,#0d1117 70%%);color:#cdd6f4;padding:20px;min-height:100vh;overflow-x:hidden;position:relative;}
  #sakura-canvas{position:fixed;top:0;left:0;width:100%%;height:100%%;pointer-events:none;z-index:0;}
  .content-wrap{position:relative;z-index:1;}
  .header{text-align:center;padding:40px 30px;margin-bottom:24px;overflow:hidden;position:relative;background:rgba(26,10,46,0.55);backdrop-filter:blur(16px) saturate(140%%);-webkit-backdrop-filter:blur(16px) saturate(140%%);border:1px solid rgba(255,121,198,0.18);border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}
  .header::before{content:'';position:absolute;top:-50%%;left:-50%%;width:200%%;height:200%%;background:radial-gradient(ellipse at center,rgba(255,121,198,0.08) 0%%,transparent 60%%);animation:headerGlow 4s ease-in-out infinite;}
  @keyframes headerGlow{0%%,100%%{transform:scale(1);opacity:0.6;}50%%{transform:scale(1.15);opacity:1;}}
  .header h1{font-size:2.4em;font-weight:300;letter-spacing:.05em;background:linear-gradient(135deg,#ff79c6,#bd93f9,#ff79c6);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;position:relative;z-index:1;animation:titleShimmer 3s ease-in-out infinite;background-size:200%% 100%%;}
  @keyframes titleShimmer{0%%,100%%{background-position:0%% 50%%;}50%%{background-position:100%% 50%%;}}
  .header .subtitle{color:rgba(205,214,244,.6);font-size:.9em;position:relative;z-index:1;margin-top:6px;}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:14px;margin-bottom:24px;}
  .card{background:rgba(26,10,46,.5);backdrop-filter:blur(12px) saturate(130%%);-webkit-backdrop-filter:blur(12px) saturate(130%%);border:1px solid rgba(255,121,198,.12);border-radius:14px;padding:22px 16px;text-align:center;transition:transform .25s,border-color .25s,box-shadow .25s;cursor:default;}
  .card:hover{transform:translateY(-3px);border-color:rgba(255,121,198,.35);box-shadow:0 8px 28px rgba(255,121,198,.15);}
  .card .number{font-size:2.3em;font-weight:700;letter-spacing:.02em;}
  .card .label{color:rgba(205,214,244,.55);margin-top:4px;font-size:.85em;}
  .card.total .number{color:#89b4fa;} .card.alive .number{color:#a6e3a1;} .card.released .number{color:#cba6f7;}
  .card.permanent .number{color:#f9e2af;} .card.leaked .number{color:#f38ba8;} .card.bytes .number{color:#fab387;font-size:1.6em;}
  .toolbar{display:flex;gap:8px;margin-bottom:18px;flex-wrap:wrap;}
  .toolbar button{padding:8px 18px;border:1px solid rgba(255,121,198,.2);border-radius:20px;background:rgba(26,10,46,.4);backdrop-filter:blur(8px);color:#cdd6f4;cursor:pointer;font-size:.88em;transition:all .25s;}
  .toolbar button:hover{background:rgba(255,121,198,.15);border-color:rgba(255,121,198,.45);}
  .toolbar button.active{background:rgba(203,166,247,.2);border-color:#cba6f7;color:#cba6f7;}
  .toolbar input{padding:8px 16px;border:1px solid rgba(255,121,198,.2);border-radius:20px;background:rgba(13,13,26,.6);backdrop-filter:blur(8px);color:#cdd6f4;font-size:.88em;flex:1;min-width:200px;outline:none;transition:border-color .25s;}
  .toolbar input:focus{border-color:rgba(255,121,198,.5);}
  .toolbar input::placeholder{color:rgba(205,214,244,.3);}
  .table-wrap{background:rgba(26,10,46,.42);backdrop-filter:blur(14px) saturate(120%%);-webkit-backdrop-filter:blur(14px) saturate(120%%);border:1px solid rgba(255,121,198,.14);border-radius:16px;overflow:hidden;}
  table{width:100%%;border-collapse:collapse;}
  th{background:rgba(30,12,50,.7);padding:13px 14px;text-align:left;font-weight:600;color:rgba(205,214,244,.65);font-size:.8em;cursor:pointer;user-select:none;letter-spacing:.03em;text-transform:uppercase;border-bottom:1px solid rgba(255,121,198,.1);}
  th:hover{color:#cdd6f4;} th .arrow{font-size:.65em;margin-left:3px;opacity:.5;}
  td{padding:11px 14px;border-bottom:1px solid rgba(255,121,198,.05);font-size:.88em;transition:background .15s;}
  tbody tr{transition:background .15s;}
  tbody tr:hover{background:rgba(255,121,198,.06)!important;}
  tbody tr.alive{border-left:3px solid #a6e3a1;}
  tbody tr.released_row{border-left:3px solid rgba(205,214,244,.15);opacity:.6;}
  .badge{display:inline-block;padding:3px 10px;border-radius:14px;font-size:.78em;font-weight:600;letter-spacing:.02em;}
  .badge-new{background:rgba(137,180,250,.15);color:#89b4fa;}
  .badge-malloc{background:rgba(249,226,175,.15);color:#f9e2af;}
  .badge-alive{background:rgba(166,227,161,.15);color:#a6e3a1;}
  .badge-released{background:rgba(205,214,244,.08);color:rgba(205,214,244,.5);}
  .badge-leak{background:rgba(243,139,168,.18);color:#f38ba8;}
  .badge-permanent{background:rgba(249,226,175,.2);color:#f9e2af;}
  .addr{font-family:'Cascadia Code','Fira Code','JetBrains Mono',monospace;color:#94e2d5;font-size:.9em;}
  .location{color:#89b4fa;cursor:pointer;} .location:hover{text-decoration:underline;color:#b4d0fb;}
  .thread-tag{color:#cba6f7;font-size:.83em;}
  .footer{text-align:center;color:rgba(205,214,244,.25);margin-top:28px;font-size:.8em;letter-spacing:.04em;}
  @keyframes petalFall{0%%{transform:translateY(-10vh) rotate(0deg) translateX(0);opacity:0;}10%%{opacity:.8;}90%%{opacity:.6;}100%%{transform:translateY(105vh) rotate(720deg) translateX(120px);opacity:0;}}
  @keyframes petalSway{0%%,100%%{transform:translateX(0);}25%%{transform:translateX(30px);}75%%{transform:translateX(-25px);}}
</style>
</head>
<body>
<canvas id="sakura-canvas"></canvas>
<div class="content-wrap">
)HTML", title);

    // ═══════════ 页面头部 ═══════════
    fprintf(f,
R"HTML(<div class="header">
  <h1>%s</h1>
  <div class="subtitle">白玉楼内存魂簿 · 幽幽子大人巡视中</div>
</div>
)HTML", title);

    // ═══════════ 统计卡片 ═══════════
    fprintf(f,
R"HTML(<div class="cards">
  <div class="card total">   <div class="number">%zu</div><div class="label">总计注册</div></div>
  <div class="card alive">   <div class="number">%zu</div><div class="label">存活中</div></div>
  <div class="card released"> <div class="number">%zu</div><div class="label">已安息</div></div>
  <div class="card permanent"><div class="number">%zu</div><div class="label">常驻标记</div></div>
  <div class="card leaked">  <div class="number">%zu</div><div class="label">可能泄漏</div></div>
  <div class="card bytes">   <div class="number">%zu B</div><div class="label">泄漏内存</div></div>
</div>
)HTML", total, alive, released, permanent, alive - permanent, leaked_bytes);

    // ═══════════ 工具栏 ═══════════
    fprintf(f,
R"HTML(<div class="toolbar">
  <button class="active" onclick="filter('all',this)">全部</button>
  <button onclick="filter('alive',this)">存活</button>
  <button onclick="filter('released',this)">已安息</button>
  <button onclick="filter('permanent',this)">常驻</button>
  <input type="text" placeholder="搜索文件名、函数名或地址..." oninput="search(this.value)">
</div>
)HTML");

    // ═══════════ 数据表格 ═══════════
    fprintf(f,
R"HTML(<div class="table-wrap"><table>
<thead><tr>
  <th onclick="sortTable(0)">状态<span class="arrow"></span></th>
  <th onclick="sortTable(1)">地址<span class="arrow"></span></th>
  <th onclick="sortTable(2)">大小<span class="arrow"></span></th>
  <th onclick="sortTable(3)">来源<span class="arrow"></span></th>
  <th onclick="sortTable(4)">分配位置<span class="arrow"></span></th>
  <th onclick="sortTable(5)">线程<span class="arrow"></span></th>
  <th onclick="sortTable(6)">存续时间<span class="arrow"></span></th>
</tr></thead><tbody>
)HTML");

    // ═══════════ 表格行 ── alive 在前, released 在后 ═══════════
    auto emit_row = [&](const SoulRecord& soul, bool is_released, bool is_permanent) {
        const char* row_class   = is_permanent ? "alive" : (is_released ? "released_row" : "alive");
        const char* status_badge = is_permanent ? "badge-new badge-permanent"
                                 : is_released ? "badge-released" : "badge-leak";
        const char* status_text  = is_permanent ? "常驻"
                                 : is_released ? "已安息" : "存活";
        const char* source_badge = (soul.source == 0) ? "badge-new" : "badge-malloc";
        const char* source_text  = (soul.source == 0) ? "new" : "malloc";
        const char* elapsed_str  = format_duration_us(now - soul.timestamp);

        fprintf(f,
R"HTML(<tr class="%s" data-status="%s">
  <td><span class="badge %s">%s</span></td>
  <td><span class="addr">%p</span></td>
  <td>%zu B</td>
  <td><span class="badge %s">%s</span></td>
  <td><span class="location" title="%s:%d">%s:%d</span><br><small style="color:rgba(205,214,244,.4)">%s()</small></td>
  <td><span class="thread-tag">%s</span></td>
  <td>%s</td>
</tr>
)HTML",
            row_class, is_permanent ? "permanent" : (is_released ? "released" : "alive"),
            status_badge, status_text,
            soul.ptr, soul.size,
            source_badge, source_text,
            soul.file, soul.line, soul.file, soul.line,
            soul.func, soul.thread_name, elapsed_str);
    };
    for (const auto& kv : g_soul_alive)
        emit_row(kv.second, false, g_soul_permanent.count(kv.first));
    for (const auto& kv : g_soul_released) emit_row(kv.second, true, false);

    // ═══════════ 尾部 + 樱花 JS ═══════════
    fprintf(f,
R"HTML(</tbody></table></div><!-- .table-wrap -->

<div class="footer">🦋 幽幽子内存魂簿报告 · 白玉楼出品</div>

<script>
(function sakura(){
  var c=document.getElementById('sakura-canvas'),x=c.getContext('2d'),w,h,p=[],t=0;
  function R(){w=c.width=window.innerWidth;h=c.height=window.innerHeight;}
  R();window.addEventListener('resize',R);
  for(var i=0;i<50;i++)p.push({x:Math.random()*w,y:Math.random()*h-h,r:3+Math.random()*6,s:.3+Math.random()*.5,o:Math.random()*9});
  function D(e){
    x.clearRect(0,0,w,h);t=e;
    for(var i=0;i<p.length;i++){var P=p[i];P.y+=P.s;P.x+=Math.sin(t/1000+P.o)*.4;if(P.y>h+20){P.y=-20;P.x=Math.random()*w;}
    x.beginPath();x.arc(P.x,P.y,P.r,0,Math.PI*2);
    x.fillStyle='rgba(255,182,193,'+(.35+.25*Math.sin(t/800+P.o)).toFixed(2)+')';x.fill();}
    requestAnimationFrame(D);
  }
  requestAnimationFrame(D);
})();
function filter(s,b){
  document.querySelectorAll('.toolbar button').forEach(function(x){x.classList.remove('active');});
  b.classList.add('active');
  document.querySelectorAll('tbody tr').forEach(function(r){
    r.style.display=s==='all'?'':r.dataset.status===s?'':'none';
  });
}
function search(q){
  q=q.toLowerCase();
  document.querySelectorAll('tbody tr').forEach(function(r){
    r.style.display=r.textContent.toLowerCase().includes(q)?'':'none';
  });
  if(!q){var b=document.querySelectorAll('.toolbar button');if(b.length)b[0].classList.add('active');}
}
var d={};
function sortTable(c){
  var t=document.querySelector('tbody'),r=Array.from(t.querySelectorAll('tr'));
  d['c'+c]=!(d['c'+c]||false);var o=d['c'+c]?1:-1;
  r.sort(function(a,b){var va=a.cells[c].textContent.trim(),vb=b.cells[c].textContent.trim();var na=parseFloat(va),nb=parseFloat(vb);if(!isNaN(na)&&!isNaN(nb))return(na-nb)*o;return va.localeCompare(vb)*o;});
  document.querySelectorAll('th .arrow').forEach(function(a){a.textContent='';});
  document.querySelectorAll('th')[c].querySelector('.arrow').textContent=o>0?'▲':'▼';
  r.forEach(function(x){t.appendChild(x);});
}
</script>
</body>
</html>
)HTML");

    fclose(f);

    if (!detail::g_async_mode) {
        fprintf(stderr, "[Yuyuko] HTML 报告已生成: %s\n", filepath);
    }
}
}

// ════════════════════════════════════════════════════════════════════════════
// 便捷宏 (ENABLE_ASAN 启用时)
// ════════════════════════════════════════════════════════════════════════════

#define TRACKED_NEW(type) \
    [](const char* file, int line, const char* func) -> type* { \
        type* ptr = new type; \
        Yuyuko::register_new(ptr, sizeof(type), file, line, func); \
        return ptr; \
    }(__FILE__, __LINE__, __FUNCTION__)

#define TRACKED_DELETE(ptr) \
    do { \
        if (ptr) { \
            if (Yuyuko::release_new(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                delete ptr; \
            } \
        } \
    } while(0)

#define TRACKED_ALLOC(size) \
    [](size_t sz, const char* file, int line, const char* func) -> void* { \
        void* ptr = std::malloc(sz); \
        if (ptr) Yuyuko::register_malloc(ptr, sz, file, line, func); \
        return ptr; \
    }(size, __FILE__, __LINE__, __FUNCTION__)

#define TRACKED_FREE(ptr) \
    do { \
        if (ptr) { \
            if (Yuyuko::release_malloc(ptr, __FILE__, __LINE__, __FUNCTION__)) { \
                std::free(ptr); \
            } \
        } \
    } while(0)

// ════════════════════════════════════════════════════════════════════════════
// 带边界检查的内存操作宏
// ════════════════════════════════════════════════════════════════════════════

#define TRACKED_MEMSET(ptr, val, size) \
    do { \
        if (Yuyuko::check_bounds((ptr), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memset((ptr), (val), (size)); \
        } \
    } while(0)

#define TRACKED_MEMCPY(dst, src, size) \
    do { \
        if (Yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memcpy((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_MEMMOVE(dst, src, size) \
    do { \
        if (Yuyuko::check_bounds((dst), (size), __FILE__, __LINE__, __FUNCTION__)) { \
            std::memmove((dst), (src), (size)); \
        } \
    } while(0)

#define TRACKED_STRCPY(dst, src) \
    TRACKED_MEMCPY((dst), (src), std::strlen(src) + 1)

#define MEM_SNAPSHOT(ptr, tag) Yuyuko::mem_snapshot((ptr), (tag))
#define MEM_VERIFY(ptr)        Yuyuko::mem_verify(ptr)
#define MEM_CHECK(ptr)         Yuyuko::mem_check((ptr), __FILE__, __LINE__, __FUNCTION__)

// ═══════════════════════════════════════════════════════════════════════════════
// RAII 守卫: 构造快照, 析构校验 — 零手动负担
// ═══════════════════════════════════════════════════════════════════════════════
//
// MEM_GUARD(ptr)                 作用域内自动保护, 默认 4KB CRC 粒度
// MEM_GUARD_TAG(ptr, "tag")      同上, 自定义 tag
// MEM_GUARD_EX(ptr, 精度)          精确粒度版, 指定 CRC 分块字节数
// MEM_GUARD_EX_TAG(ptr, 精度, tag) 精确粒度 + 自定义 tag
//
// 示例:
//   void foo() {
//       auto* p = TRACKED_ALLOC(1024);
//       MEM_GUARD(p);              // 4KB 粒度, 快速
//       MEM_GUARD_EX(p, 64);       // 64B 粒度, 精确定位到 64 字节内
//       do_stuff(p);               // 裸写破坏 p
//   }                              // 离开作用域 → 自动 MEM_CHECK → 检测到损坏!

#define MEM_GUARD(ptr) \
    Yuyuko::MemGuard _yuko_g_##__LINE__((ptr), __FUNCTION__, __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_TAG(ptr, tag) \
    Yuyuko::MemGuard _yuko_g_##__LINE__((ptr), (tag), __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_EX(ptr, chunk_size) \
    Yuyuko::MemGuardEx _yuko_gx_##__LINE__((ptr), (chunk_size), __FUNCTION__, __FILE__, __LINE__, __FUNCTION__)

#define MEM_GUARD_EX_TAG(ptr, chunk_size, tag) \
    Yuyuko::MemGuardEx _yuko_gx_##__LINE__((ptr), (chunk_size), (tag), __FILE__, __LINE__, __FUNCTION__)

#define PERMANENT_SOUL(ptr) Yuyuko::mark_permanent(ptr)

#define TRACKED_STRNCPY(dst, src, n) \
    do { \
        size_t len = std::strnlen((src), (n)); \
        if (Yuyuko::check_bounds((dst), (n), __FILE__, __LINE__, __FUNCTION__)) { \
            std::strncpy((dst), (src), (n)); \
        } \
    } while(0)

#else

namespace Yuyuko {
    inline void start_async_log(const char* = nullptr) {}
    inline void stop_async_log() {}
    inline void register_new(void*, size_t, const char*, int, const char*) {}
    inline void register_malloc(void*, size_t, const char*, int, const char*) {}
    inline bool release_new(void*, const char*, int, const char*) { return true; }
    inline bool release_malloc(void*, const char*, int, const char*) { return true; }
    inline bool check_access(void*, const char*, int, const char*) { return true; }
    inline bool check_bounds(void*, size_t, const char*, int, const char*) { return true; }
    inline bool is_released(void*) { return false; }
    inline size_t get_size(void*) { return 0; }
    inline size_t get_total_souls() { return 0; }
    inline size_t get_alive_souls() { return 0; }
    inline size_t get_permanent_souls() { return 0; }
    inline size_t leak_check() { return 0; }
    inline void mark_permanent(void*) {}
    inline void mem_snapshot(void*, const char* = "default") {}
    inline void mem_snapshot_ex(void*, size_t, const char* = "default") {}
    inline bool mem_verify(void*) { return true; }
    inline bool mem_check(void*, const char*, int, const char*) { return true; }
    inline bool mem_check_ex(void*, size_t, const char*, int, const char*) { return true; }
    class MemGuard {
    public:
        MemGuard(void*, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    class MemGuardEx {
    public:
        MemGuardEx(void*, size_t, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    class MemGuardBatch {
    public:
        void add(void*, const char*, const char*, int, const char*) {}
        void dismiss() {}
    };
    inline void generate_html_report(const char* = nullptr, const char* = nullptr) {}
} // namespace Yuyuko

#define TRACKED_NEW(type)   new type
#define TRACKED_DELETE(ptr) delete ptr
#define TRACKED_ALLOC(size) std::malloc(size)
#define TRACKED_FREE(ptr)   std::free(ptr)
#define TRACKED_MEMSET(ptr, val, size)   std::memset((ptr), (val), (size))
#define TRACKED_MEMCPY(dst, src, size)   std::memcpy((dst), (src), (size))
#define TRACKED_MEMMOVE(dst, src, size)  std::memmove((dst), (src), (size))
#define TRACKED_STRCPY(dst, src)         std::strcpy((dst), (src))
#define TRACKED_STRNCPY(dst, src, n)     std::strncpy((dst), (src), (n))
#define MEM_SNAPSHOT(ptr, tag) ((void)0)
#define MEM_VERIFY(ptr)        (true)
#define MEM_CHECK(ptr)         (true)
#define MEM_GUARD(ptr)         ((void)0)
#define MEM_GUARD_TAG(ptr, tag) ((void)0)
#define MEM_GUARD_EX(ptr, chunk_size) ((void)0)
#define MEM_GUARD_EX_TAG(ptr, chunk_size, tag) ((void)0)
#define PERMANENT_SOUL(ptr) ((void)0)

#endif // ENABLE_ASAN
