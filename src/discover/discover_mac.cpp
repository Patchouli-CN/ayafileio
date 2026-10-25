#include "discover.hpp"

#ifdef __APPLE__

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>

namespace ayafileio::discovery {

bool gcd_dispatch_io_available() {
    static const bool available = [] {
        dispatch_queue_t queue = dispatch_queue_create("com.ayafileio.test", DISPATCH_QUEUE_SERIAL);
        if (!queue) return false;

        char path[] = "/tmp/ayafileio_test_XXXXXX";
        const int fd = mkstemp(path);
        if (fd == -1) {
            dispatch_release(queue);
            return false;
        }
        dispatch_io_t channel = dispatch_io_create(
            DISPATCH_IO_RANDOM, fd, queue, ^(int) {});
        const bool ok = channel != nullptr;
        if (channel) {
            dispatch_io_close(channel, DISPATCH_IO_STOP);
            dispatch_release(channel);
        } else {
            close(fd);
        }
        unlink(path);
        dispatch_release(queue);
        return ok;
    }();
    return available;
}

std::unordered_map<std::string, core::CapabilityValue> gcd_detail() {
    std::unordered_map<std::string, core::CapabilityValue> result;
    const bool available = gcd_dispatch_io_available();
    result["dispatch_io"] = available;
    if (available) {
        result["completion_model"] = false;
        result["kernel_managed_workqueue"] = true;
        result["mincore_fastpath"] = true;
    }
    return result;
}

} // namespace ayafileio::discovery

#endif
