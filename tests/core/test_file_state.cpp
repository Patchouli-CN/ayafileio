// FileState 与生命周期政策的原生单测。
//
// 为什么放这里：`FileState` 是四个后端**共用**的位置/大小/追加语义所有者，而
// `core/file_lifecycle.hpp` 是它们共用的 seek/tell/truncate 政策。这两层纯 C++、
// 平台无关，却是历史上修复最密集的地方：
//
//     ee5ab44  POSIX 两个后端补上文件大小缓存刷新
//     9ca83d4  补上 io_uring 后端的大小缓存刷新
//     8a03ba8  seek 负偏移与非法模式串的静默偏差
//     aae3842  open 时不再无条件取文件大小（少一次 syscall）
//
// 这些改动当时只能靠 Python 端到端用例验证，而端到端覆盖不到「什么时候该问内核
// 要大小」这种内部时序。这里用可计数的 refresh 回调把它钉住。

#include "file_lifecycle.hpp"
#include "file_state.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace ayafileio::core;

// 计数型 refresh：既能返回大小，又能证明「问了几次内核」。
struct Probe {
    std::optional<uint64_t> size{100};
    int calls{0};

    auto callback() {
        return [this]() -> std::optional<uint64_t> {
            ++calls;
            return size;
        };
    }
};

void test_plan_read_sequential() {
    Probe probe;
    FileState state;
    state.set_cached_size(100);

    const auto first = state.plan_read(20, probe.callback());
    assert(!first.eof && first.offset == 0 && first.size == 20);

    const auto second = state.plan_read(-1, probe.callback());
    assert(!second.eof && second.offset == 20 && second.size == 80);

    const auto at_eof = state.plan_read(10, probe.callback());
    assert(at_eof.eof && at_eof.size == 0);

    // 请求大于剩余：截到剩余量
    state.set_file_pos(0);
    const auto clamped = state.plan_read(500, probe.callback());
    assert(!clamped.eof && clamped.size == 100);

    // 请求 0：既不是 EOF 也不该规划出 I/O
    state.set_file_pos(0);
    const auto zero = state.plan_read(0, probe.callback());
    assert(zero.eof && zero.size == 0);

    std::cout << "  plan_read sequential: ok\n";
}

void test_plan_read_refresh_only_at_eof() {
    // 不在 EOF 时**不允许**问内核要大小——这是热路径上省掉的 syscall（aae3842）。
    Probe probe;
    FileState state;
    state.set_cached_size(100);

    (void)state.plan_read(10, probe.callback());
    assert(probe.calls == 0 && "读到中途就 stat 了，说明每 op 都在问内核");

    // 到达 EOF 时才允许刷新；刷新后能看见外部追加。
    probe.size = 120;
    const auto at_eof = state.plan_read(10, probe.callback());
    assert(probe.calls == 1 && "EOF 判决点必须刷新一次大小缓存");
    assert(!at_eof.eof && at_eof.size == 10 && at_eof.offset == 100);

    std::cout << "  plan_read refresh policy: ok\n";
}

void test_plan_read_at() {
    Probe probe;
    FileState state;
    state.set_cached_size(100);

    const auto mid = state.plan_read_at(40, 10, probe.callback());
    assert(!mid.eof && mid.offset == 40 && mid.size == 10);

    const auto beyond = state.plan_read_at(200, 10, probe.callback());
    assert(beyond.eof && beyond.size == 0);

    // 跨越 EOF：截断
    const auto across = state.plan_read_at(95, 10, probe.callback());
    assert(!across.eof && across.size == 5);

    // 负偏移直接当 EOF 返回，不刷新、不动位置
    state.set_file_pos(7);
    const auto negative = state.plan_read_at(-1, 10, probe.callback());
    assert(negative.eof);
    assert(state.tell() == 7 && "read_at 失败不应改动逻辑位置");

    std::cout << "  plan_read_at: ok\n";
}

void test_reserve_write() {
    Probe probe;

    // 定位写：位置不变，缓存大小随写入扩张
    FileState positioned;
    positioned.set_cached_size(10);
    assert(positioned.reserve_write(20, 5) == 20);
    assert(positioned.cached_size() == 25);
    assert(positioned.tell() == 0 && "定位写不应推进逻辑位置");

    // 顺序写（position<0）：位置前进
    FileState sequential;
    sequential.set_cached_size(10);
    assert(sequential.reserve_write(-1, 4) == 10);
    assert(sequential.tell() == 14);
    assert(sequential.cached_size() == 14);

    // 追加模式：即使 seek(0) 之后，写入仍落在缓存大小处
    FileState append(true);
    append.set_cached_size(30);
    append.set_file_pos(0);
    assert(append.reserve_write(-1, 2) == 30 && "append 必须忽略逻辑位置");
    assert(append.tell() == 32);

    std::cout << "  reserve_write: ok\n";
}

void test_seek() {
    Probe probe;
    FileState state;
    state.set_cached_size(100);

    assert(state.seek(10, 0, probe.callback()) == std::optional<uint64_t>(10));
    assert(state.seek(5, 1, probe.callback()) == std::optional<uint64_t>(15));
    assert(state.seek(-5, 1, probe.callback()) == std::optional<uint64_t>(10));
    assert(state.seek(0, 2, probe.callback()) == std::optional<uint64_t>(100));

    // 负目标位置必须失败，且**不改动**已建立的位置
    assert(!state.seek(-1, 0, probe.callback()));
    assert(state.tell() == 100 && "失败的 seek 改动了逻辑位置");
    assert(!state.seek(-1000, 1, probe.callback()));
    assert(state.tell() == 100);

    // 非法 whence
    assert(!state.seek(0, 3, probe.callback()));
    assert(!state.seek(0, -1, probe.callback()));

    std::cout << "  seek: ok\n";
}

void test_apply_truncate() {
    FileState state;
    state.set_cached_size(100);
    state.set_file_pos(80);

    state.apply_truncate(50);
    assert(state.cached_size() == 50);
    assert(state.tell() == 50 && "truncate 之后位置必须被夹到新大小");

    state.set_file_pos(10);
    state.apply_truncate(200);
    assert(state.cached_size() == 200);
    assert(state.tell() == 10 && "扩大截断不应改动位置");

    std::cout << "  apply_truncate: ok\n";
}

void test_seek_completion_policy() {
    Probe probe;
    FileState state;
    state.set_cached_size(100);

    const auto ok = seek_completion(state, 10, 0, probe.callback());
    assert(!ok.failed() && ok.value == 10 && ok.return_count);

    const auto bad_whence = seek_completion(state, 0, 5, probe.callback());
    assert(bad_whence.failed() && bad_whence.error.kind == ErrorKind::Value);

    const auto negative = seek_completion(state, -1, 0, probe.callback());
    assert(negative.failed() && negative.error.kind == ErrorKind::Value);

    // 政策顺序：非法 whence 不该先动 state（否则失败也会改位置）
    state.set_file_pos(42);
    (void)seek_completion(state, 0, 9, probe.callback());
    assert(state.tell() == 42 && "非法 whence 在校验前就改动了状态");

    std::cout << "  seek_completion policy: ok\n";
}

void test_tell_completion_policy() {
    FileState state;
    state.set_cached_size(100);
    state.set_file_pos(37);

    const auto result = tell_completion(state);
    assert(!result.failed() && result.value == 37 && result.return_count);

    std::cout << "  tell_completion policy: ok\n";
}

void test_truncate_policy() {
    // 拒绝路径
    const auto rejected = truncate_guard(-1);
    assert(rejected.has_value());
    assert(rejected->failed() && rejected->error.kind == ErrorKind::Value);

    const auto accepted = truncate_guard(0);
    assert(!accepted.has_value() && "size=0 是合法的 truncate");

    // 失败结果
    const auto failed = truncate_failed(28);
    assert(failed.failed() && failed.error.kind == ErrorKind::Native);

    // 成功结果：必须夹住位置
    FileState state;
    state.set_cached_size(100);
    state.set_file_pos(90);
    const auto done = truncate_done(state, 40);
    assert(!done.failed());
    assert(state.cached_size() == 40);
    assert(state.tell() == 40 && "truncate_done 必须先 apply_truncate 再返回成功");

    std::cout << "  truncate policy: ok\n";
}

} // namespace

void run_file_state_tests() {
    std::cout << "file_state / lifecycle policy:\n";
    test_plan_read_sequential();
    test_plan_read_refresh_only_at_eof();
    test_plan_read_at();
    test_reserve_write();
    test_seek();
    test_apply_truncate();
    test_seek_completion_policy();
    test_tell_completion_policy();
    test_truncate_policy();
}
