#include "py_api.hpp"

#include "py_async_file.hpp"
#include "py_error.hpp"
#include "py_future.hpp"
#include "../../core/completion.hpp"
#include "../../core/config.hpp"
#include "../../core/global_thread_pool.hpp"
#include "../../platform/native_file.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ayafileio::python {
namespace {

// 读整个文件：open + size + read + close 全在这一个函数里，由**一个**池任务执行。
//
// 这是 Node.js #65327「read small files in one thread pool round trip」的同形状实现：
// 把 N 跳压成 1 跳。之所以不能直接在 Python 侧用 asyncio.to_thread 包一个同步函数 ——
// 那是拿更贵的机制（Python 调用 + executor 提交 + GIL 往返）替换更便宜的 IOCP 完成包，
// 实测反而慢一倍（6.7k vs 13.3k files/s）；原生这一版实测 25.6k（2.1x）。
//
// 失败时通过 stage 指出是哪一步（open/stat/read），让错误文案与异步路径一致。
core::Buffer read_whole_blocking(const std::string& path,
                                 std::size_t* size,
                                 int* error,
                                 const char** stage) {
    *size = 0;
    *error = 0;
    *stage = "read";

    const auto handle = platform::file_open(path.c_str(), platform::kOpenRead);
    if (handle == platform::kFileInvalid) {
        *error = platform::native_file_last_error();
        *stage = "open";
        return {};
    }

    unsigned long long total = 0;
    if (!platform::file_size(handle, &total)) {
        *error = platform::native_file_last_error();
        *stage = "stat";
        platform::file_close(handle);
        return {};
    }

    auto buffer = core::Buffer::heap(static_cast<std::size_t>(total));
    std::size_t filled = 0;
    while (filled < total) {
        const auto got = platform::file_pread(
            handle, buffer.data() + filled,
            static_cast<std::size_t>(total) - filled,
            static_cast<long long>(filled));
        if (got < 0) {
            *error = platform::native_file_last_error();
            *stage = "read";
            break;
        }
        if (got == 0) break;   // stat 之后文件被截短：以实际读到的为准
        filled += static_cast<std::size_t>(got);
    }
    platform::file_close(handle);
    *size = filled;
    return buffer;
}

std::string failure_text(const char* stage, const std::string& path) {
    return std::string("Failed to ") + stage + " file: " + path;
}

} // namespace

nb::object py_read_whole_async(const char* path) {
    auto context = LoopContext::current();
    const std::string path_copy(path);

    auto operation = std::make_shared<PyOperation>(context, core::ReqType::Read);
    operation->submitted();

    auto& pool = core::GlobalThreadPool::instance();
    pool.ensure_started(core::effective_io_worker_count());
    pool.enqueue([operation, path_copy]() {
        const PyGILState_STATE state = PyGILState_Ensure();
        try {
            core::Buffer buffer;
            std::size_t size = 0;
            int error = 0;
            const char* stage = "read";
            {
                // 阻塞 I/O 期间放掉 GIL，否则池线程会互相堵住。
                GilRelease release;
                buffer = read_whole_blocking(path_copy, &size, &error, &stage);
            }
            if (error != 0) {
                operation->complete(core::Completion::failure(
                    core::ReqType::Read,
                    core::IoError::native(static_cast<std::uint32_t>(error),
                                          failure_text(stage, path_copy))));
            } else {
                operation->complete(core::Completion::read(std::move(buffer), size));
            }
        } catch (const core::IoException& error) {
            operation->complete(core::Completion::failure(
                core::ReqType::Read, error.error()));
        } catch (const std::exception& error) {
            operation->complete(core::Completion::failure(
                core::ReqType::Read, core::IoError::runtime(error.what())));
        }
        PyGILState_Release(state);
    });
    return operation->future_object();
}

} // namespace ayafileio::python
