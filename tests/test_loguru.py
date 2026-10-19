"""真实负载测试：50 协程同时写 5 个文件（既是 pytest 用例，也是可直接跑的脚本）。

作为 pytest 用例时校验的是**数据完整性**（每个文件的字节数），性能数字只是
打印出来给人看——绝对吞吐随机器波动，不该进断言。
"""

import asyncio
import os
import sys
import tempfile
import time
from pathlib import Path

import ayafileio
from ayafileio import open as aya_open

if __name__ == "__main__":  # 脚本模式才接管控制台；pytest 下会废掉 capture
    from _console import use_utf8_console

    use_utf8_console()

LOG_COUNT = 10_000
NUM_FILES = 5
CONCURRENT = 50
MESSAGE = "x" * 128 + "\n"
# 文本模式 + newline=None：磁盘上 "\n" 会被翻译成 os.linesep
MESSAGE_BYTES = len(MESSAGE.replace("\n", os.linesep).encode("utf-8"))


def _cleanup(prefix: str) -> None:
    for path in Path(tempfile.gettempdir()).glob(f"{prefix}_concurrent_*.log"):
        path.unlink()


async def _aya_round(log_count: int = LOG_COUNT, num_files: int = NUM_FILES) -> float:
    """ayafileio：N 个协程并发 append 到 num_files 个文件。"""
    files: dict[int, object] = {}
    _cleanup("aya")
    try:
        async def write_one(index: int) -> None:
            file_index = index % num_files
            path = Path(tempfile.gettempdir()) / f"aya_concurrent_{file_index}.log"
            if file_index not in files:
                files[file_index] = aya_open(path, "a")
            await files[file_index].write(MESSAGE)

        started = time.perf_counter()
        await asyncio.gather(*(write_one(i) for i in range(log_count)))
        return time.perf_counter() - started
    finally:
        for handle in files.values():
            await handle.close()


async def _sync_round(log_count: int = LOG_COUNT, num_files: int = NUM_FILES) -> float:
    """对照：同步写 + asyncio.to_thread 模拟 50 个线程池工作者。"""
    files: dict[int, object] = {}
    _cleanup("sync")
    try:
        def write_one(index: int) -> None:
            file_index = index % num_files
            path = Path(tempfile.gettempdir()) / f"sync_concurrent_{file_index}.log"
            if file_index not in files:
                files[file_index] = open(path, "a")  # noqa: SIM115 — 批次结束统一关
            files[file_index].write(MESSAGE)

        started = time.perf_counter()
        await asyncio.gather(*(asyncio.to_thread(write_one, i) for i in range(log_count)))
        return time.perf_counter() - started
    finally:
        for handle in files.values():
            handle.close()


def _total_bytes(prefix: str) -> int:
    return sum(
        path.stat().st_size
        for path in Path(tempfile.gettempdir()).glob(f"{prefix}_concurrent_*.log")
    )


async def test_aya_concurrent_log_writes() -> None:
    """50 协程并发写 5 个文件：条数对得上、字节数对得上。"""
    elapsed = await _aya_round()
    try:
        assert elapsed > 0
        written = _total_bytes("aya")
        assert written == LOG_COUNT * MESSAGE_BYTES, (
            f"落盘字节数不对: {written} != {LOG_COUNT * MESSAGE_BYTES}"
        )
        print(f"\n🍃 ayafileio: {elapsed:.3f}s ({LOG_COUNT / elapsed:,.0f} 条/秒)")
    finally:
        _cleanup("aya")


async def test_sync_thread_pool_baseline() -> None:
    """同一负载的同步 + 线程池对照，顺带验字节数（没有断言提速比）。"""
    elapsed = await _sync_round()
    try:
        assert elapsed > 0
        written = _total_bytes("sync")
        assert written == LOG_COUNT * MESSAGE_BYTES, (
            f"落盘字节数不对: {written} != {LOG_COUNT * MESSAGE_BYTES}"
        )
        print(f"📄 同步+线程池: {elapsed:.3f}s ({LOG_COUNT / elapsed:,.0f} 条/秒)")
    finally:
        _cleanup("sync")


async def main() -> None:
    """脚本模式：把两个数放在一起算提速比（性能数字只在这里出现）。"""
    # Linux 性能调优只在脚本模式做——pytest 下改全局配置会传染其它用例
    if sys.platform == "linux":
        ayafileio.configure(
            {
                "io_uring_queue_depth": 512,
                "io_uring_sqpoll": True,
                "buffer_size": 256 * 1024,
            }
        )

    info = ayafileio.get_backend_info()
    print(f"\n后端: {info['backend']} (真异步: {info['is_truly_async']})")
    print(f"\n📊 日志: {LOG_COUNT} 条 × {len(MESSAGE)} 字节, 并发写 {NUM_FILES} 个文件\n")

    t_aya = await _aya_round()
    print(f"🍃 ayafileio ({info['backend']}): {t_aya:.3f}s ({LOG_COUNT / t_aya:,.0f} 条/秒)")
    t_sync = await _sync_round()
    print(f"📄 同步 + 线程池:        {t_sync:.3f}s ({LOG_COUNT / t_sync:,.0f} 条/秒)")

    print(f"\n{'=' * 50}")
    print(f"🚀 并发写入提速: {t_sync / t_aya:.1f}x")
    print(f"{'=' * 50}")


if __name__ == "__main__":
    asyncio.run(main())
