// yuyuko_memlife.hpp — 幽幽子内存生命周期追踪系统
//
// 编译：
//   定义 ENABLE_ASAN → 全功能追踪。
//   不定义 ENABLE_ASAN → 所有宏/函数变为零开销 stub。
//
// 特性：
//   - 追踪 new/delete 和 malloc/free 的完整生命周期
//   - Double Free 检测与拦截
//   - 多线程 Use-After-Free 检测
//   - 分配器不匹配检测（new vs malloc）
//   - CRC-32C 硬件加速内存完整性校验 (SSE4.2 / ARMv8-CRC, μs 级精度)
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
// RAII 守卫：
//   MEM_GUARD(ptr)                 → 默认 4KB 粒度
//   MEM_GUARD_TAG(ptr, "tag")      → 自定义 tag
//   MEM_GUARD_EX(ptr, 精度)          → 指定 CRC 分块字节数
//   MEM_GUARD_EX_TAG(ptr, 精度, tag) → 自定义粒度 + tag
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
    detail::g_log_thread = std::jthread{};  // jthread dtor auto-joins
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
struct SoulRecord {
    void*    ptr;
    size_t   size;
    uint64_t timestamp;       // 分配/最后操作的时间戳 (μs)
    uint16_t line;            // 行号
    uint8_t  source;          // AllocSource
    uint8_t  released;        // 0=alive, 1=freed
    char     file[80];        // 源文件名
    char     func[64];        // 函数名
    char     thread_name[64]; // 线程名
    char     thread_id[20];   // 线程 ID (hex)
};
static_assert(sizeof(SoulRecord) <= 256, "SoulRecord too large");

/// 全局魂簿：记录所有被追踪的内存块
static std::shared_mutex g_soul_mtx;
static std::unordered_map<void*, SoulRecord> g_soul_book;
/// 按地址排序的索引 — check_bounds 二分查找用 (O(log n) vs 旧版 O(n))
static std::set<uintptr_t> g_soul_range;
static bool _soul_book_init = []{ g_soul_book.reserve(65536); return true; }();

/// 在魂簿中查找记录（调用方需持有 g_soul_mtx）
static SoulRecord* find_soul(void* ptr) {
    auto it = g_soul_book.find(ptr);
    return (it != g_soul_book.end()) ? &it->second : nullptr;
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
    sr.released=0;
    str_copy(sr.file,sizeof(sr.file),file);
    str_copy(sr.func,sizeof(sr.func),func);
    str_copy(sr.thread_name,sizeof(sr.thread_name),cached_thread_name());
    str_copy(sr.thread_id,sizeof(sr.thread_id),cached_thread_id());

    { std::unique_lock<std::shared_mutex> lk(g_soul_mtx); g_soul_book[ptr] = sr; g_soul_range.insert(reinterpret_cast<uintptr_t>(ptr)); }

    detail::yuyuko_logln(
        "[Yuyuko] ALLOC %p size=%zu src=%s @ %s:%d %s() [%s/%s]",
        ptr, size,
        (source==AllocSource::OPERATOR_NEW)?"new":"malloc",
        file,line,func,sr.thread_name,sr.thread_id);
}

/// 记录一次内存释放（内部函数）
/// @return true if the caller should free the memory
static bool release_soul_impl(void* ptr, AllocSource source,
                               const char* file, int line, const char* func) {
    if (!ptr) return false;
    const char* tn = cached_thread_name();
    const char* ti = cached_thread_id();
    uint64_t now = current_time_us();
    bool should_free = true;

    {
        std::unique_lock<std::shared_mutex> lk(g_soul_mtx);
        SoulRecord* s = find_soul(ptr);

        if (!s) {
            detail::yuyuko_logln("[Yuyuko] FREE-UNKNOWN %p @ %s:%d [%s/%s]",ptr,file,line,tn,ti);
        } else if (s->source != static_cast<uint8_t>(source)) {
            detail::yuyuko_logln("[Yuyuko] MISMATCH %p reg=%s tried=%s @ %s:%d [%s/%s]",
                ptr,(s->source==0)?"new":"malloc",
                (source==AllocSource::OPERATOR_NEW)?"new":"malloc",file,line,tn,ti);
        } else if (s->released) {
            detail::yuyuko_logln("[Yuyuko] DOUBLE-FREE %p freed %s ago\n"
                "  1st: [%s/%s] %s:%d %s()\n  2nd: [%s/%s] %s:%d %s()",
                ptr, format_duration_us(now - s->timestamp),
                s->thread_name,s->thread_id,s->file,s->line,s->func,tn,ti,file,line,func);
            should_free = false;
        } else {
            detail::yuyuko_logln("[Yuyuko] FREE %p life=%s src=%s [%s/%s]",
                ptr, format_duration_us(now - s->timestamp),
                (source==AllocSource::OPERATOR_NEW)?"new":"malloc",tn,ti);
            s->released=1; s->timestamp=now;
            g_soul_range.erase(reinterpret_cast<uintptr_t>(ptr));
        }
    }
    return should_free;
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
    SoulRecord* s = find_soul(ptr);
    if (!s || !s->released) return true;
    detail::yuyuko_logln(
        "[Yuyuko] USE-AFTER-FREE %p @ %s:%d %s() — %s ago [%s/%s]",
        ptr,file,line,func,format_duration_us(current_time_us() - s->timestamp),
        s->thread_name,s->thread_id);
    return false;
}

/// 检查 [ptr, ptr+size) 写入是否在已分配范围内 (越界检测)
/// 在已分配块中查找包含地址范围 [ps, pe) 的 SoulRecord (O(log n))
/// 调用方需持有 g_soul_mtx
static SoulRecord* find_containing_soul(uintptr_t ps, uintptr_t pe) {
    if (g_soul_range.empty()) return nullptr;
    auto it = g_soul_range.upper_bound(ps);
    if (it == g_soul_range.begin()) return nullptr;
    --it;
    uintptr_t start = *it;
    SoulRecord* s = find_soul(reinterpret_cast<void*>(start));
    if (!s || s->released) return nullptr;
    uintptr_t as = start, ae = as + s->size;
    return (ps >= as && pe <= ae) ? s : nullptr;
}

/// 检查 [ptr, ptr+size) 写入是否在已分配范围内 (越界检测)
/// O(1) 快路径 (ptr 精确命中分配起始) + O(log n) 慢路径 (内部/偏移指针)
inline bool check_bounds(void* ptr, size_t size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    uintptr_t ps = reinterpret_cast<uintptr_t>(ptr), pe = ps + size;

    // ── 快路径：ptr 恰好是某个已知分配的起始地址 ──
    SoulRecord* s = find_soul(ptr);
    if (s) {
        if (s->released) {
            detail::yuyuko_logln("[Yuyuko] USE-AFTER-FREE-WRITE %p @ %s:%d", ptr, file, line);
            return false;
        }
        uintptr_t ae = reinterpret_cast<uintptr_t>(s->ptr) + s->size;
        if (pe > ae) {
            detail::yuyuko_logln("[Yuyuko] BUFFER-OVERFLOW %p+%zu @ %s:%d alloc=%zu @ %s:%d",
                ptr, size, file, line, s->size, s->file, s->line);
            return false;
        }
        return true;
    }

    // ── 慢路径：ptr 在已分配块的内部或前方 — 二分查找 (O(log n)) ──
    s = find_containing_soul(ps, pe);
    if (!s) {
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
    SoulRecord* s = find_soul(ptr);
    if (!s || s->released) return;
    size_t sz = s->size;
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
    SoulRecord* s = find_soul(ptr);
    if (!s || s->released) return false;
    size_t sz = s->size;
    slk.unlock();
    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto it = g_checkpoints.find(ptr);
    if (it == g_checkpoints.end()) return true;
    size_t cs = it->second.chunk_size ? it->second.chunk_size : CRC_CHUNK_SIZE;
    auto now = crc32_chunked(reinterpret_cast<const uint8_t*>(ptr), sz, cs);
    return now == it->second.crcs;
}

/// mem_check_ex: 检测裸写/野写，指定 chunk 粒度精确定位损坏
/// @param chunk_size 回退粒度（仅当快照无记录时使用，已有快照则用快照保存的粒度）
/// @return true=完整, false=已损坏
inline bool mem_check_ex(void* ptr, size_t chunk_size, const char* file, int line, const char* func) {
    if (!ptr) return true;
    std::shared_lock<std::shared_mutex> slk(g_soul_mtx);
    SoulRecord* s = find_soul(ptr);
    if (!s) { detail::yuyuko_logln("[Yuyuko] CHECK %p NOT TRACKED @ %s:%d %s()",ptr,file,line,func); return false; }
    if (s->released) { detail::yuyuko_logln("[Yuyuko] CHECK %p FREED @ %s:%d %s()",ptr,file,line,func); return false; }
    size_t sz = s->size;
    const char* af = s->file; int al = s->line;
    const char* ath = s->thread_name; const char* ati = s->thread_id;
    slk.unlock();

    std::shared_lock<std::shared_mutex> clk(g_cp_mtx);
    auto it = g_checkpoints.find(ptr);
    if (it == g_checkpoints.end()) {
        clk.unlock();
        detail::yuyuko_logln("[Yuyuko] CHECK %p NO-SNAPSHOT @ %s:%d %s() — mem_snapshot first",
            ptr,file,line,func);
        return false;
    }
    const auto& old = it->second.crcs;
    uint64_t elapsed = current_time_us() - it->second.timestamp;
    const char* tag = it->second.tag;
    // 使用快照时保存的粒度（若为 0 则回退到参数 chunk_size）
    size_t cs = it->second.chunk_size ? it->second.chunk_size : chunk_size;

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
    auto* s=find_soul(ptr); return s?s->released:true;
}
inline size_t get_size(void* ptr) {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    auto* s=find_soul(ptr); return s?s->size:0;
}
inline size_t get_total_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    return g_soul_book.size();
}
inline size_t get_alive_souls() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    size_t n=0; for(auto& kv:g_soul_book)n+=!kv.second.released; return n;
}

/// 程序退出时检测内存泄漏
inline size_t leak_check() {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);
    size_t leaks=0;
    for (auto& kv : g_soul_book) {
        const SoulRecord& sr=kv.second;
        if (sr.released) continue;
        ++leaks;
        detail::yuyuko_logln(
            "[Yuyuko] LEAK %p %zub [%s/%s] %s:%d %s() age=%s",
            sr.ptr,sr.size,sr.thread_name,sr.thread_id,
            sr.file,sr.line,sr.func,
            format_duration_us(current_time_us() - sr.timestamp));
    }
    detail::yuyuko_logln(leaks?"[Yuyuko] leak: %zu/%zu":"[Yuyuko] no leaks",leaks,g_soul_book.size());
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
    size_t total = g_soul_book.size();
    size_t alive = 0;
    size_t leaked_bytes = 0;
    size_t total_bytes = 0;
    for (const auto& kv : g_soul_book) {
        total_bytes += kv.second.size;
        if (!kv.second.released) {
            alive++;
            leaked_bytes += kv.second.size;
        }
    }
    size_t released = total - alive;
    uint64_t now = current_time_us();
    
    // ═══════════════ HTML 头部 ═══════════════
    fprintf(f, 
R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>%s</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif;
    background: #0d1117;
    color: #c9d1d9;
    padding: 20px;
  }
  .header {
    text-align: center;
    padding: 30px;
    background: linear-gradient(135deg, #1a1a2e 0%%, #16213e 50%%, #0f3460 100%%);
    border-radius: 12px;
    margin-bottom: 20px;
    border: 1px solid #30363d;
  }
  .header h1 {
    font-size: 2em;
    color: #ff79c6;
    margin-bottom: 8px;
  }
  .header .subtitle {
    color: #8b949e;
    font-size: 0.9em;
  }
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 16px;
    margin-bottom: 20px;
  }
  .card {
    background: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
    padding: 20px;
    text-align: center;
  }
  .card .number {
    font-size: 2.5em;
    font-weight: bold;
  }
  .card .label {
    color: #8b949e;
    margin-top: 4px;
    font-size: 0.9em;
  }
  .card.total .number { color: #58a6ff; }
  .card.alive .number { color: #3fb950; }
  .card.released .number { color: #d2a8ff; }
  .card.leaked .number { color: #f85149; }
  .card.bytes .number { color: #d2991d; font-size: 1.8em; }
  
  .toolbar {
    display: flex;
    gap: 10px;
    margin-bottom: 16px;
    flex-wrap: wrap;
  }
  .toolbar button {
    padding: 8px 16px;
    border: 1px solid #30363d;
    border-radius: 6px;
    background: #21262d;
    color: #c9d1d9;
    cursor: pointer;
    font-size: 0.9em;
    transition: all 0.2s;
  }
  .toolbar button:hover { background: #30363d; }
  .toolbar button.active { background: #1f6feb; border-color: #1f6feb; }
  .toolbar input {
    padding: 8px 12px;
    border: 1px solid #30363d;
    border-radius: 6px;
    background: #0d1117;
    color: #c9d1d9;
    font-size: 0.9em;
    flex: 1;
    min-width: 200px;
  }
  
  table {
    width: 100%%;
    border-collapse: collapse;
    background: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
    overflow: hidden;
  }
  th {
    background: #21262d;
    padding: 12px;
    text-align: left;
    font-weight: 600;
    color: #8b949e;
    font-size: 0.85em;
    cursor: pointer;
    user-select: none;
  }
  th:hover { color: #c9d1d9; }
  th .arrow { font-size: 0.7em; margin-left: 4px; }
  td {
    padding: 10px 12px;
    border-top: 1px solid #21262d;
    font-size: 0.9em;
  }
  tr:hover { background: #1c2128; }
  tr.alive { border-left: 3px solid #3fb950; }
  tr.released_row { border-left: 3px solid #484f58; opacity: 0.65; }
  
  .badge {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 0.8em;
    font-weight: 600;
  }
  .badge-new { background: #1f6feb22; color: #58a6ff; }
  .badge-malloc { background: #d2991d22; color: #d2991d; }
  .badge-alive { background: #3fb95022; color: #3fb950; }
  .badge-released { background: #484f5822; color: #8b949e; }
  .badge-leak { background: #f8514922; color: #f85149; }
  
  .addr { font-family: 'Cascadia Code', 'Fira Code', monospace; color: #7ee787; }
  .location { color: #58a6ff; cursor: pointer; }
  .location:hover { text-decoration: underline; }
  .thread-tag { color: #d2a8ff; font-size: 0.85em; }
  
  .footer {
    text-align: center;
    color: #484f58;
    margin-top: 20px;
    font-size: 0.85em;
  }
</style>
</head>
<body>
)", title);
    
    // ═══════════════ 页面头部 ═══════════════
    fprintf(f, R"(
<div class="header">
  <h1>🦋 %s</h1>
  <div class="subtitle">白玉楼内存魂簿 · 生成时间：系统运行时</div>
</div>
)", title);
    
    // ═══════════════ 统计卡片 ═══════════════
    fprintf(f, R"(
<div class="cards">
  <div class="card total">
    <div class="number">%zu</div>
    <div class="label">📋 总计注册灵魂</div>
  </div>
  <div class="card alive">
    <div class="number">%zu</div>
    <div class="label">🟢 存活中</div>
  </div>
  <div class="card released">
    <div class="number">%zu</div>
    <div class="label">🟣 已安息</div>
  </div>
  <div class="card leaked">
    <div class="number">%zu</div>
    <div class="label">🔴 可能泄漏</div>
  </div>
  <div class="card bytes">
    <div class="number">%zu B</div>
    <div class="label">💾 存活内存</div>
  </div>
</div>
)", total, alive, released, alive, leaked_bytes);
    
    // ═══════════════ 工具栏 ═══════════════
    fprintf(f, R"HTML(
<div class="toolbar">
  <button class="active" onclick="filter('all', this)">全部</button>
  <button onclick="filter('alive', this)">🟢 存活</button>
  <button onclick="filter('released', this)">🟣 已安息</button>
  <input type="text" placeholder="🔍 搜索文件名、函数名或地址..." oninput="search(this.value)">
</div>
)HTML");
    
    // ═══════════════ 数据表格 ═══════════════
    fprintf(f, R"HTML(
<table>
<thead>
<tr>
  <th onclick="sortTable(0)">状态<span class="arrow"></span></th>
  <th onclick="sortTable(1)">地址<span class="arrow"></span></th>
  <th onclick="sortTable(2)">大小<span class="arrow"></span></th>
  <th onclick="sortTable(3)">来源<span class="arrow"></span></th>
  <th onclick="sortTable(4)">分配位置<span class="arrow"></span></th>
  <th onclick="sortTable(5)">线程<span class="arrow"></span></th>
  <th onclick="sortTable(6)">存续时间<span class="arrow"></span></th>
</tr>
</thead>
<tbody>
)HTML");
    
    // 遍历魂簿，生成每一行
    for (const auto& kv : g_soul_book) {
        const SoulRecord& soul = kv.second;
        
        const char* row_class = soul.released ? "released_row" : "alive";
        const char* status_badge;
        const char* status_text;
        
        if (soul.released) {
            status_badge = "badge-released";
            status_text = "已安息";
        } else {
            status_badge = "badge-leak";
            status_text = "存活";
        }
        
        const char* source_badge = (soul.source == 0)
            ? "badge-new" : "badge-malloc";
        const char* source_text = (soul.source == 0)
            ? "new" : "malloc";
        
        const char* elapsed_str = format_duration_us(now - soul.timestamp);

        fprintf(f, R"HTML(
<tr class="%s" data-status="%s">
  <td><span class="badge %s">%s</span></td>
  <td><span class="addr">%p</span></td>
  <td>%zu B</td>
  <td><span class="badge %s">%s</span></td>
  <td><span class="location" title="%s:%d">%s:%d</span><br><small style="color:#8b949e">%s()</small></td>
  <td><span class="thread-tag">%s</span></td>
  <td>%s</td>
</tr>
)HTML",
            row_class,
            soul.released ? "released" : "alive",
            status_badge, status_text,
            soul.ptr,
            soul.size,
            source_badge, source_text,
            soul.file, soul.line, soul.file, soul.line,
            soul.func,
            soul.thread_name,
            elapsed_str
        );
    }
    
    // ═══════════════ HTML 尾部 + JavaScript ═══════════════
    fprintf(f, R"HTML(
</tbody>
</table>

<div class="footer">
  🦋 幽幽子内存魂簿报告 · 由 Yuyuko::generate_html_report() 生成 · 白玉楼出品
</div>

<script>
// 筛选功能
function filter(status, btn) {
  document.querySelectorAll('.toolbar button').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  document.querySelectorAll('tbody tr').forEach(row => {
    if (status === 'all') row.style.display = '';
    else row.style.display = row.dataset.status === status ? '' : 'none';
  });
}

// 搜索功能
function search(query) {
  const q = query.toLowerCase();
  document.querySelectorAll('tbody tr').forEach(row => {
    const text = row.textContent.toLowerCase();
    row.style.display = text.includes(q) ? '' : 'none';
  });
  // 搜索时取消筛选按钮的高亮
  document.querySelectorAll('.toolbar button').forEach(b => b.classList.remove('active'));
  if (!q) document.querySelector('.toolbar button').classList.add('active');
}

// 排序功能
let sortDir = {};
function sortTable(col) {
  const tbody = document.querySelector('tbody');
  const rows = Array.from(tbody.querySelectorAll('tr'));
  const key = 'col' + col;
  sortDir[key] = !(sortDir[key] || false);
  const dir = sortDir[key] ? 1 : -1;
  
  rows.sort((a, b) => {
    let va = a.cells[col].textContent.trim();
    let vb = b.cells[col].textContent.trim();
    // 尝试按数字排序
    let na = parseFloat(va);
    let nb = parseFloat(vb);
    if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dir;
    return va.localeCompare(vb) * dir;
  });
  
  // 更新箭头
  document.querySelectorAll('th .arrow').forEach(a => a.textContent = '');
  const th = document.querySelectorAll('th')[col];
  th.querySelector('.arrow').textContent = dir > 0 ? '▲' : '▼';
  
  rows.forEach(row => tbody.appendChild(row));
}
</script>
</body>
</html>
)HTML");
    
    fclose(f);
    
    // 同步模式下输出到终端；异步模式下已经在锁内，直接 fprintf stderr
    if (!detail::g_async_mode) {
        fprintf(stderr, "[Yuyuko] HTML 报告已生成: %s\n", filepath);
    }
}

} // namespace Yuyuko

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

#define TRACKED_STRNCPY(dst, src, n) \
    do { \
        size_t len = std::strnlen((src), (n)); \
        if (Yuyuko::check_bounds((dst), (n), __FILE__, __LINE__, __FUNCTION__)) { \
            std::strncpy((dst), (src), (n)); \
        } \
    } while(0)

#else // ENABLE_ASAN 未启用 — 零开销 stub

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
    inline size_t leak_check() { return 0; }

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

#endif // ENABLE_ASAN
