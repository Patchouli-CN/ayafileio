import asyncio
import timeit
import ayafileio
import os
import sys
import faulthandler
import platform
import tempfile
import gc
faulthandler.enable()

def is_ci() -> bool:
    ci_vars = [
        "CI", "GITHUB_ACTIONS", "GITLAB_CI", "JENKINS_HOME",
        "TRAVIS", "CIRCLECI", "APPVEYOR", "DRONE", "BUILD_ID",
    ]
    return any(os.environ.get(var) for var in ci_vars)

if sys.platform == "win32" and is_ci():
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# ── per-platform fd limit detection ───────────────────────────────────────────
def _get_fd_limit() -> int:
    if sys.platform == "win32":
        return 50000  # Windows HANDLE table is per-process, effectively unlimited
    try:
        import resource
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        return max(10, int(soft * 0.8) - 50)
    except Exception:
        return 200  # conservative fallback (macOS without resource module)

SAFE_CONCURRENCY = _get_fd_limit()

print(f"平台: {platform.system()} | fd 安全并发上限: {SAFE_CONCURRENCY}")
print("应用调优配置...")
ayafileio.configure({
    "buffer_size": 512 * 1024,
    "buffer_pool_max": 1024,
    "close_timeout_ms": 3000,
})

CHUNK_SIZE = 512
CONCURRENCY = min(50000, SAFE_CONCURRENCY)
ITERATIONS_PER_TASK = 10
TOTAL_OPS = CONCURRENCY * ITERATIONS_PER_TASK

# 写测试每个 worker 创建独立文件
# CI: 共享磁盘上大量 CreateFileW 非常慢（>5 min），降低并发
WRITE_CONCURRENCY = min(CONCURRENCY, 2000) if is_ci() else CONCURRENCY

# ── temp directory for all test files ─────────────────────────────────────────
TEST_DIR = tempfile.mkdtemp(prefix="ayafileio_critical_")
READ_FILE = os.path.join(TEST_DIR, "read_test.bin")
WRITE_PREFIX = os.path.join(TEST_DIR, "write_")

# ── create read test file (100MB, enough to bust cache) ───────────────────────
READ_FILE_SIZE_MB = 100
print(f"正在生成 {READ_FILE_SIZE_MB}MB 读取测试文件...")
with open(READ_FILE, "wb") as f:
    f.write(b"0" * 1024 * 1024 * READ_FILE_SIZE_MB)
print("生成完毕。")

# ── pre-create write test files (empty, one per worker) ───────────────────────
# Pre-create outside the timed section so CreateFileW overhead doesn't skew results.
print(f"正在预创建 {WRITE_CONCURRENCY} 个写入测试文件...")
_write_files = [f"{WRITE_PREFIX}{i}" for i in range(WRITE_CONCURRENCY)]
for wf in _write_files:
    open(wf, "wb").close()
print("预创建完毕。")
print()

def _report_failures(results, label, total):
    """Report exceptions from gather results without aborting."""
    failures = [r for r in results if isinstance(r, BaseException)]
    ok = total - len(failures)
    if failures:
        first = failures[0]
        print(f"  [{label}] {ok}/{total} 成功, {len(failures)} 失败 (首次错误: {type(first).__name__}: {first})")
    else:
        print(f"  [{label}] {ok}/{total} 全部成功")

async def stress_ayafileio():
    async def worker():
        buf = bytearray(CHUNK_SIZE)
        async with ayafileio.open(READ_FILE, "rb") as f:
            for _ in range(ITERATIONS_PER_TASK):
                await f.readinto(buf)

    t = timeit.default_timer()
    tasks = [worker() for _ in range(CONCURRENCY)]
    results = await asyncio.gather(*tasks, return_exceptions=True)
    elapsed = timeit.default_timer() - t

    _report_failures(results, "read", CONCURRENCY)
    qps = TOTAL_OPS / elapsed
    print(f"[ayafileio] {TOTAL_OPS} 次读取 (512B) 耗时: {elapsed:.3f}s | QPS: {qps:.0f}")


async def stress_write():
    async def worker(idx):
        buf = bytes(CHUNK_SIZE)
        async with ayafileio.open(_write_files[idx], "wb") as f:
            for _ in range(ITERATIONS_PER_TASK):
                await f.write(buf)

    t = timeit.default_timer()
    tasks = [worker(i) for i in range(WRITE_CONCURRENCY)]
    results = await asyncio.gather(*tasks, return_exceptions=True)
    total_write_ops = WRITE_CONCURRENCY * ITERATIONS_PER_TASK
    elapsed = timeit.default_timer() - t

    _report_failures(results, "write", WRITE_CONCURRENCY)
    qps = total_write_ops / elapsed
    print(f"[ayafileio write] {total_write_ops} 次写入 (512B) 耗时: {elapsed:.3f}s | QPS: {qps:.0f}")


def cleanup():
    """Remove all test files."""
    import shutil
    try:
        shutil.rmtree(TEST_DIR, ignore_errors=True)
    except Exception:
        pass


async def main():
    try:
        print(f"=== DDoS 压力测试 ===")
        print(f"协程数: {CONCURRENCY} | 每协程循环: {ITERATIONS_PER_TASK} 次 | 数据块: {CHUNK_SIZE}B | 写并发: {WRITE_CONCURRENCY}")
        print(f"总操作数: {TOTAL_OPS}")
        print()

        gc.collect()
        gc.disable()

        print("开始测试 ayafileio (真异步)...")
        await stress_ayafileio()

        gc.collect()

        print(f"\n并发写测试 (协程数: {WRITE_CONCURRENCY})")
        await stress_write()

        print(f"\n=== 测试结束 ===")
    finally:
        gc.enable()
        print("清理测试文件...")
        cleanup()
        print("清理完毕。")


asyncio.run(main())
