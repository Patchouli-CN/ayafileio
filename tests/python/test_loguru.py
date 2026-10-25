"""真实负载测试：50 协程同时写 5 个文件（既是 pytest 用例，也是可直接跑的脚本）。

作为 pytest 用例时校验的是**数据完整性**（每个文件的字节数），性能数字只是
打印出来给人看——绝对吞吐随机器波动，不该进断言。
"""

import asyncio
import os
import sys
import tempfile
import time
from contextlib import AsyncExitStack
from pathlib import Path

import ayafileio

if __name__ == "__main__":  # 脚本模式才接管控制台；pytest 下会废掉 capture
    from _console import use_utf8_console

    use_utf8_console()

LOG_COUNT = 10_000
NUM_FILES = 5
CONCURRENT = 50
MESSAGE = "x" * 128 + "\n"
RAW_MESSAGE = MESSAGE.encode("utf-8")
# 文本模式 + newline=None：磁盘上 "\n" 会被翻译成 os.linesep
MESSAGE_BYTES = len(MESSAGE.replace("\n", os.linesep).encode("utf-8"))


def _cleanup(prefix: str) -> None:
    for path in Path(tempfile.gettempdir()).glob(f"{prefix}_concurrent_{os.getpid()}_*.log"):
        path.unlink()


async def _aya_round(log_count: int = LOG_COUNT, num_files: int = NUM_FILES) -> float:
    """ayafileio：N 个协程并发 append 到 num_files 个文件（打开不计入计时）。"""
    _cleanup("aya")
    paths = [
        Path(tempfile.gettempdir()) / f"aya_concurrent_{os.getpid()}_{i}.log"
        for i in range(num_files)
    ]
    async with AsyncExitStack() as stack:
        handles = [
            await stack.enter_async_context(
                ayafileio.open(str(path), "a", encoding="utf-8")
            )
            for path in paths
        ]

        async def write_one(index: int) -> None:
            await handles[index % num_files].write(MESSAGE)

        started = time.perf_counter()
        await asyncio.gather(*(write_one(i) for i in range(log_count)))
        return time.perf_counter() - started


async def _sync_round(log_count: int = LOG_COUNT, num_files: int = NUM_FILES) -> float:
    """对照：同步写 + asyncio.to_thread 模拟 50 个线程池工作者。

    句柄必须在启动线程**之前**打开：旧写法在 worker 里懒创建
    ``files[file_index] = open(...)``，多线程同时命中同一个 index 时会各开一个
    句柄、字典互相覆盖，未关闭的 BufferedWriter 里那部分数据也就永远没落盘
    （实测 10000 条只写出 1,167,966 / 1,290,000 字节）。以前只打印耗时不校验，
    这个竞态没人发现。
    """
    _cleanup("sync")
    paths = [
        Path(tempfile.gettempdir()) / f"sync_concurrent_{os.getpid()}_{i}.log"
        for i in range(num_files)
    ]
    handles: list = []
    try:
        # 二进制模式：BufferedWriter 有内部锁，多线程写是安全的。
        # 文本模式（open(..., encoding="utf-8")）的 TextIOWrapper 在 3.10 上
        # 没有自己的锁，多线程并发 write 会丢数据（CI 的 3.10 上实测
        # 1,290,000 → 1,162,161 字节，正好少 991 条），所以对照臂用二进制。
        handles.extend(open(path, "ab") for path in paths)  # noqa: SIM115

        def write_one(index: int) -> None:
            handles[index % num_files].write(RAW_MESSAGE)

        started = time.perf_counter()
        await asyncio.gather(*(asyncio.to_thread(write_one, i) for i in range(log_count)))
        return time.perf_counter() - started
    finally:
        for handle in handles:
            handle.close()


def _total_bytes(prefix: str) -> int:
    return sum(
        path.stat().st_size
        for path in Path(tempfile.gettempdir()).glob(f"{prefix}_concurrent_{os.getpid()}_*.log")
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
        expected = LOG_COUNT * len(RAW_MESSAGE)  # 二进制，无换行翻译
        written = _total_bytes("sync")
        assert written == expected, f"落盘字节数不对: {written} != {expected}"
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
    print(f"\n后端: {info['backend']}")
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
