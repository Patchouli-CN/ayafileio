#ifdef AYAFILEIO_VERBOSE_LOGGING

#define UR_LOG(fmt, ...) \
    do { \
        auto now = std::chrono::steady_clock::now(); \
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
            now.time_since_epoch()).count() % 1000000; \
        std::fprintf(stderr, "[%6ld][0x%zx] " fmt "\n", \
            ms, std::hash<std::thread::id>{}(std::this_thread::get_id()), ##__VA_ARGS__); \
        std::fflush(stderr); \
    } while(0)

#define UR_LOG0(fmt) \
    do { \
        auto now = std::chrono::steady_clock::now(); \
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
            now.time_since_epoch()).count() % 1000000; \
        std::fprintf(stderr, "[%6ld][0x%zx] " fmt "\n", \
            ms, std::hash<std::thread::id>{}(std::this_thread::get_id())); \
        std::fflush(stderr); \
    } while(0)

#define UR_DEBUG_LOG(fmt, ...)   UR_LOG(fmt, ##__VA_ARGS__)  // ← 关键！
#define UR_DEBUG_LOG0(fmt)       UR_LOG0(fmt)

#else

#define UR_LOG(fmt, ...)         ((void)0)
#define UR_LOG0(fmt)             ((void)0)
#define UR_DEBUG_LOG(fmt, ...)   ((void)0)
#define UR_DEBUG_LOG0(fmt)       ((void)0)

#endif