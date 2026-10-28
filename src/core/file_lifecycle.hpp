#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 文件生命周期政策
//
// `src/backend/` 下四个后端（IOCP / io_uring / Dispatch I/O / 线程池）各自实现
// seek / tell / truncate。改造前它们的政策部分是**逐字节复制**的：同样的 whence
// 校验、同样的 "negative seek position" 文案、同样的「先校验、再 syscall、最后
// apply_truncate」顺序，各写一遍。
//
// 这类重复的危险不在于行数，而在于**漂移**：谁少写一句 apply_truncate，谁把文案
// 改了，都会变成「同一个 API 在不同后端上行为不同」。历史上这类修复是分开打的
// （ee5ab44 / 9ca83d4 / ab6b521 都是同一族问题）。
//
// 所以这个头把**政策**收在这里，后端只保留**原语**：
//
//     if (auto rejected = truncate_guard(size)) { …complete_inline(*rejected); return; }
//     if (::ftruncate(fd_, size) != 0) { …complete_inline(truncate_failed(errno)); return; }
//     …complete_inline(truncate_done(state_, size));
//
// 政策在这里只有一份，原语各平台各一份——这正是它们该有的比例。
// ─────────────────────────────────────────────────────────────────────────────

#include "completion.hpp"
#include "file_state.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace ayafileio::core {

//: seek 的完整政策。
//:
//: 顺序有意义：先拒绝非法 whence（不要先动 state），再交给 FileState 做
//: EOF 刷新与位置计算，最后拒绝负目标位置。失败时**不得**改动逻辑位置——
//: conformance 套件里 test_seek_rejects_invalid 守着这一点。
//:
//: Refresh 是后端提供的一次「向内核问真实大小」的回调；只有 whence==2 时才会被
//: 调用，其他情况由 FileState 自己决定要不要问。
template <typename Refresh>
Completion seek_completion(FileState& state, int64_t offset, int whence, Refresh&& refresh) {
    if (whence < 0 || whence > 2) {
        return Completion::failure(ReqType::Other, IoError::value("Invalid whence value"));
    }
    const auto position = state.seek(offset, whence, std::forward<Refresh>(refresh));
    if (!position) {
        return Completion::failure(ReqType::Other, IoError::value("negative seek position"));
    }
    return Completion::counted(ReqType::Other, *position);
}

//: tell 的完成结果（不是 bool：要带回位置）。
inline Completion tell_completion(const FileState& state) {
    return Completion::counted(ReqType::Other, state.tell());
}

//: truncate 的前置校验。
//:
//: 有值表示**已被拒绝**，调用方直接 complete_inline 后返回，不要执行 syscall。
//: 用返回值而不是抛异常，是因为后端的失败路径一律走 complete_inline——
//: 抛异常会被调用方当成提交期错误，语义不同。
inline std::optional<Completion> truncate_guard(int64_t size) {
    if (size < 0) {
        return Completion::failure(ReqType::Other, IoError::value("negative size not allowed"));
    }
    return std::nullopt;
}

//: truncate syscall 失败的统一结果。文案与错误码映射只在这里定义。
inline Completion truncate_failed(uint32_t native_code) {
    return Completion::failure(ReqType::Other, IoError::native(native_code, "truncate failed"));
}

//: truncate 成功：先让 FileState 更新缓存大小与夹紧位置，再给出完成结果。
//:
//: 顺序不能反——apply_truncate 会夹住 file_pos_，必须发生在成功结果交付之前，
//: 否则并发路径上可能有人先看到新的完成、再看到旧的位置。
inline Completion truncate_done(FileState& state, int64_t size) {
    state.apply_truncate(static_cast<uint64_t>(size));
    return Completion::success(ReqType::Other);
}

} // namespace ayafileio::core
