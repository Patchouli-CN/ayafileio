#include "yuyuko.hpp"

#include "../core/io_request.hpp"
#include "yuyuko/memory_tracker.hpp"

namespace ayafileio::diagnostics {

void init_request_hooks() {
#ifdef ENABLE_YUYUKO
    core::set_request_lifecycle_hooks({
        [](void* pointer, std::size_t size, const char* file, int line, const char* func) {
            yuyuko::register_new(pointer, size, file, line, func);
        },
        [](void* pointer, const char* file, int line, const char* func) {
            return yuyuko::release_new(pointer, file, line, func);
        },
        [](void* pointer, const char* file, int line, const char* func) {
            return yuyuko::check_access(pointer, file, line, func);
        },
    });
#else
    core::set_request_lifecycle_hooks({});
#endif
}

} // namespace ayafileio::diagnostics
