import asyncio
import timeit
import ayafileio
import os
import io
import sys
import faulthandler
faulthandler.enable() 

if sys.platform == "win32":
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

print("应用 Windows IOCP 极限调优...")
ayafileio.configure(
    {
        "buffer_size": 512 * 1024,  # 提升缓冲区大小
        "buffer_pool_max": 1024,  # 加大缓冲区池
        "close_timeout_ms": 3000,  # 优化关闭超时
    }
)

TEST_FILE = "test_1gb.bin"
CHUNK_SIZE = 512  # 极小块，放大调度开销
CONCURRENCY = 50000  # 高并发
ITERATIONS_PER_TASK = 10  # 每个协程循环读 10 次
TOTAL_OPS = CONCURRENCY * ITERATIONS_PER_TASK  # 50000 次操作

# 确保有测试文件
if not os.path.exists(TEST_FILE):
    print("正在生成 1GB 测试文件...")
    with open(TEST_FILE, "wb") as f:
        f.write(b"0" * 1024 * 1024 * 1024)
    print("生成完毕。")

async def stress_ayafileio():
    async def worker():
        buf = bytearray(CHUNK_SIZE)
        async with ayafileio.open(TEST_FILE, "rb") as f:
            for _ in range(ITERATIONS_PER_TASK):
                await f.readinto(buf)

    t = timeit.default_timer()
    tasks = [worker() for _ in range(CONCURRENCY)]
    await asyncio.gather(*tasks)
    elapsed = timeit.default_timer() - t
    qps = TOTAL_OPS / elapsed
    print(
        f"[ayafileio] {TOTAL_OPS} 次读取 (512B) 耗时: {elapsed:.3f}s | QPS: {qps:.0f}"
    )


async def main():
    print(f"\n=== DDoS 压力测试 ===")
    print(
        f"协程数: {CONCURRENCY} | 每协程循环: {ITERATIONS_PER_TASK} 次 | 数据块: {CHUNK_SIZE}B"
    )
    print(f"总操作数: {TOTAL_OPS}\n")

    print("\n开始测试 ayafileio (真异步)...")
    await stress_ayafileio()

    print("\n=== 测试结束 ===")


asyncio.run(main())
