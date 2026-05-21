import asyncio
import timeit
import ayafileio
import os
import io
import sys
import faulthandler
import platform
faulthandler.enable()

def is_ci() -> bool:
    """检测是否在持续集成(CI)环境中运行"""
    ci_vars = [
        "CI",              # 通用，GitHub Actions / Travis / GitLab CI 等都会设置
        "GITHUB_ACTIONS",  # GitHub Actions
        "GITLAB_CI",       # GitLab CI
        "JENKINS_HOME",    # Jenkins
        "TRAVIS",          # Travis CI
        "CIRCLECI",        # CircleCI
        "APPVEYOR",        # AppVeyor
        "DRONE",           # Drone CI
        "BUILD_ID",        # Jenkins / Google Cloud Build
    ]
    return any(os.environ.get(var) for var in ci_vars)

if sys.platform == "win32" and is_ci():
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# ── per-platform fd limit detection ───────────────────────────────────────
def _get_fd_limit() -> int:
    """Return a safe concurrency cap based on the platform's fd limit."""
    if sys.platform == "win32":
        return 50000  # Windows HANDLE limit is per-process and very high
    try:
        import resource
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        # 80% of soft limit, minus ~50 for stdio / event loop / other overhead
        return max(10, int(soft * 0.8) - 50)
    except Exception:
        return 200  # conservative fallback for macOS without resource module

SAFE_CONCURRENCY = _get_fd_limit()

print(f"平台: {platform.system()} | fd 安全并发上限: {SAFE_CONCURRENCY}")
print("应用 IOCP 极限调优..." if sys.platform == "win32" else "应用调优配置...")
ayafileio.configure(
    {
        "buffer_size": 512 * 1024,  # 提升缓冲区大小
        "buffer_pool_max": 1024,  # 加大缓冲区池
        "close_timeout_ms": 3000,  # 优化关闭超时
    }
)

TEST_FILE = "test_1gb.bin"
CHUNK_SIZE = 512  # 极小块，放大调度开销
CONCURRENCY = min(50000, SAFE_CONCURRENCY)  # 高并发，但不超过系统 fd 上限
ITERATIONS_PER_TASK = 10  # 每个协程循环读 10 次
TOTAL_OPS = CONCURRENCY * ITERATIONS_PER_TASK  # 总操作数

# 写测试每个 worker 创建独立文件，CI 共享磁盘上 50K 次 CreateFileW
# 非常慢（>5 min）。CI 环境降低并发，本地保持高并发。
WRITE_CONCURRENCY = min(CONCURRENCY, 2000) if is_ci() else CONCURRENCY

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

async def stress_write():
    # Each worker gets its own temp file — avoids CREATE_ALWAYS thrashing
    # on a single path that causes kernel resource exhaustion on Windows.
    import tempfile
    tmpdir = tempfile.mkdtemp(prefix="ayafileio_write_")

    async def worker(idx: int):
        buf = bytes(CHUNK_SIZE)
        path = os.path.join(tmpdir, f"w{idx}")
        async with ayafileio.open(path, "wb") as f:
            for _ in range(ITERATIONS_PER_TASK):
                await f.write(buf)

    total_ops = WRITE_CONCURRENCY * ITERATIONS_PER_TASK
    t = timeit.default_timer()
    tasks = [worker(i) for i in range(WRITE_CONCURRENCY)]
    await asyncio.gather(*tasks)
    elapsed = timeit.default_timer() - t
    qps = total_ops / elapsed
    print(
        f"[ayafileio write] {total_ops} 次写入 (512B) 耗时: {elapsed:.3f}s | QPS: {qps:.0f}"
    )
    # 清理临时目录
    import shutil
    try:
        shutil.rmtree(tmpdir)
    except Exception:
        pass

async def main():
    print(f"\n=== DDoS 压力测试 ===")
    print(
        f"协程数: {CONCURRENCY} | 每协程循环: {ITERATIONS_PER_TASK} 次 | 数据块: {CHUNK_SIZE}B"
    )
    print(f"总操作数: {TOTAL_OPS}\n")

    print("\n开始测试 ayafileio (真异步)...")
    await stress_ayafileio()
    print(f"\n并发写测试 (协程数: {WRITE_CONCURRENCY})")
    await stress_write()

    print("\n=== 测试结束 ===")


asyncio.run(main())
