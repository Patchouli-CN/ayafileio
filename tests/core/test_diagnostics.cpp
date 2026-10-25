#include "io_request.hpp"
#include "yuyuko.hpp"
#include "yuyuko/memory_tracker.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    namespace yuyuko = ayafileio::diagnostics::yuyuko;

    const char* log_path = "ayafileio_diagnostics_test.log";
    const char* report_path = "ayafileio_diagnostics_test.html";
    std::remove(log_path);
    std::remove(report_path);

    yuyuko::start_async_log(log_path);
    void* memory = TRACKED_ALLOC(64);
    assert(memory != nullptr);
    assert(yuyuko::get_size(memory) == 64);
    assert(yuyuko::get_alive_souls() == 1);

    TRACKED_MEMSET(memory, 0, 64);
    MEM_SNAPSHOT(memory, "diagnostics-test");
    assert(MEM_VERIFY(memory));
    const char payload[] = "ayafileio";
    TRACKED_MEMCPY(memory, payload, sizeof(payload));
    assert(MEM_SNAPSHOT_UPDATE(memory, 0, sizeof(payload)));
    assert(MEM_VERIFY(memory));
    assert(MEM_CHECK(memory));
    {
        MEM_GUARD_BATCH_DEF(guard);
        MEM_GUARD_BATCH_ADD(guard, memory, "batch");
    }

    PERMANENT_SOUL(memory);
    assert(yuyuko::get_permanent_souls() == 1);
    assert(yuyuko::leak_check() == 0);
    TRACKED_FREE(memory);
    assert(yuyuko::is_released(memory));
    assert(yuyuko::leak_check() == 0);

    yuyuko::generate_html_report(report_path, "ayafileio diagnostics test");
    const auto report = read_file(report_path);
    assert(report.find("ayafileio diagnostics test") != std::string::npos);
    assert(report.find("ayafileio") != std::string::npos);

    yuyuko::stop_async_log();
    const auto log = read_file(log_path);
    assert(log.find("ALLOC") != std::string::npos);
    assert(log.find("FREE") != std::string::npos);
    assert(log.find("异步日志引擎停止") != std::string::npos);

#ifdef AYAFILEIO_TEST_YUYUKO_HOOKS
    ayafileio::diagnostics::init_request_hooks();
    auto* request = ayafileio::core::req_alloc_impl(__FILE__, __LINE__, __FUNCTION__);
    assert(yuyuko::get_size(request) == sizeof(ayafileio::core::IORequest));
    ayafileio::core::req_free_impl(request, __FILE__, __LINE__, __FUNCTION__);
    assert(yuyuko::is_released(request));
#endif

    assert(std::remove(log_path) == 0);
    assert(std::remove(report_path) == 0);
    std::cout << "diagnostics tests passed\n";
    return 0;
}
