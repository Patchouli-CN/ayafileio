"""真实负载测试：50 协程同时写 5 个文件"""

import io
import sys
import asyncio
import tempfile
import time
from pathlib import Path
import ayafileio
from ayafileio import open as aya_open

# 设置 Windows 控制台编码
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")


# linux性能调优
if sys.platform == "linux":
    ayafileio.configure(
        {
            "io_uring_queue_depth": 512,
            "io_uring_sqpoll": True,
            "buffer_size": 256 * 1024,
        }
    )


async def test_aya(LOG_COUNT, CONCURRENT, NUM_FILES):
    """ayafileio 50 协程并发写 5 个文件"""
    message = "x" * 128 + "\n"
    files = {}

    async def write_one(file_idx, task_idx):
        path = Path(tempfile.gettempdir()) / f"aya_concurrent_{file_idx}.log"
        if file_idx not in files:
            files[file_idx] = aya_open(path, "a")
        await files[file_idx].write(message)

    tasks = []
    t0 = time.perf_counter()
    for i in range(LOG_COUNT):
        tasks.append(write_one(i % NUM_FILES, i))
    await asyncio.gather(*tasks)

    for f in files.values():
        await f.close()
    elapsed = time.perf_counter() - t0

    for p in Path(tempfile.gettempdir()).glob("aya_concurrent_*.log"):
        p.unlink()

    return elapsed


async def test_sync(LOG_COUNT, CONCURRENT, NUM_FILES):
    """同步写入 — via asyncio.to_thread 模拟 50 线程池并发"""
    message = "x" * 128 + "\n"
    files = {}

    def write_one(file_idx, task_idx):
        path = Path(tempfile.gettempdir()) / f"sync_concurrent_{file_idx}.log"
        if file_idx not in files:
            files[file_idx] = open(path, "a")
        files[file_idx].write(message)

    tasks = []
    t0 = time.perf_counter()
    for i in range(LOG_COUNT):
        tasks.append(asyncio.to_thread(write_one, i % NUM_FILES, i))
    await asyncio.gather(*tasks)

    for f in files.values():
        f.close()
    elapsed = time.perf_counter() - t0

    for p in Path(tempfile.gettempdir()).glob("sync_concurrent_*.log"):
        p.unlink()

    return elapsed


async def main():
    import ayafileio

    info = ayafileio.get_backend_info()
    print(f"\n后端: {info['backend']} (真异步: {info['is_truly_async']})")

    LOG_COUNT = 10_000
    NUM_FILES = 5

    print(f"\n📊 日志: {LOG_COUNT} 条 × 128 字节, 并发写 {NUM_FILES} 个文件")
    print()

    t_aya = await test_aya(LOG_COUNT, 50, NUM_FILES)
    print(f"🍃 ayafileio (IOCP):     {t_aya:.3f}s  ({LOG_COUNT / t_aya:.0f} 条/秒)")

    t_sync = await test_sync(LOG_COUNT, 50, NUM_FILES)
    print(f"📄 同步 + 线程池:         {t_sync:.3f}s  ({LOG_COUNT / t_sync:.0f} 条/秒)")

    print(f"\n{'=' * 50}")
    print(f"🚀 并发写入提速: {t_sync / t_aya:.1f}x")
    print(f"{'=' * 50}")


if __name__ == "__main__":
    asyncio.run(main())
