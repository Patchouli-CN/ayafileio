"""
配置调优对比：默认 vs 调参后的 ayafileio

在 HDD 上测试不同配置组合对性能的影响。
"""

import asyncio
import sys
import time
import os
import gc
import tempfile
import shutil
import itertools
from pathlib import Path

if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).parent.parent))
import ayafileio

FILE_SIZE_MB = 20
CHUNK_SIZE = 256
CONCURRENT = 100_000


def prepare_file(path, size_mb):
    chunk = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 256
    target = size_mb * 1024 * 1024
    written = 0
    with open(path, "wb") as f:
        while written < target:
            f.write(chunk)
            written += len(chunk)


async def run_read_test(path, concurrent):
    file_size = os.path.getsize(path)
    max_offset = file_size - CHUNK_SIZE

    async with ayafileio.open(path, "rb") as f:
        async def rdr(i):
            await f.seek((i * 9973 + 123457) % max_offset)
            return len(await f.read(CHUNK_SIZE))
        tasks = [asyncio.create_task(rdr(i)) for i in range(concurrent)]
        results = await asyncio.gather(*tasks, return_exceptions=True)

    ok = sum(1 for r in results if isinstance(r, int) and r == CHUNK_SIZE)
    errs = sum(1 for r in results if isinstance(r, BaseException))
    return ok, errs


async def test_config(name, config_updates, path, concurrent):
    """应用配置，跑测试，然后恢复默认"""
    ayafileio.reset_config()
    if config_updates:
        ayafileio.configure(config_updates)

    cfg = ayafileio.get_config()
    gc.collect()

    t0 = time.perf_counter()
    ok, errs = await run_read_test(path, concurrent)
    elapsed = time.perf_counter() - t0

    ayafileio.reset_config()
    return {
        "name": name,
        "config": config_updates or "(default)",
        "ok": ok, "errs": errs,
        "time": elapsed,
        "ops": ok / elapsed if elapsed > 0 else 0,
        "effective_cfg": {k: cfg[k] for k in ["iocp_batch_size", "buffer_size", "buffer_pool_max", "io_worker_count"]},
    }


async def main():
    print(f"╔══════════════════════════════════════════════════════════════╗")
    print(f"║   ayafileio 配置调优 — {CONCURRENT//1000}K 并发随机读 (HDD)         ║")
    print(f"╚══════════════════════════════════════════════════════════════╝")
    print(f"Backend: {ayafileio.get_backend_info()['backend']}")
    print(f"文件: {FILE_SIZE_MB}MB, 块: {CHUNK_SIZE}B, 并发: {CONCURRENT:,}")

    tmpdir = tempfile.mkdtemp(prefix="aya_tune_")
    path = os.path.join(tmpdir, "tune_data.bin")

    try:
        print(f"\n准备测试文件...", end=" ", flush=True)
        prepare_file(path, FILE_SIZE_MB)
        print("OK")

        # ── 默认配置基线 ──────────────────────────────────────────────────
        print(f"\n[1] 默认配置 (基线)")
        baseline = await test_config("默认", {}, path, CONCURRENT)
        print(f"    {baseline['ops']:,.0f} ops/s  ({baseline['time']:.2f}s)")

        # ── 逐参数扫描 ────────────────────────────────────────────────────
        configs_to_test = [
            # (名称, 配置)
            ("batch=128",       {"iocp_batch_size": 128}),
            ("batch=256",       {"iocp_batch_size": 256}),
            ("buf=128K",        {"buffer_size": 131072}),
            ("buf=256K",        {"buffer_size": 262144}),
            ("buf_pool=1024",   {"buffer_pool_max": 1024}),
            ("buf_pool=2048",   {"buffer_pool_max": 2048}),
            ("workers=4",       {"io_worker_count": 4}),
            ("workers=8",       {"io_worker_count": 8}),
            # 组合拳
            ("batch=256 + buf=256K",           {"iocp_batch_size": 256, "buffer_size": 262144}),
            ("batch=256 + buf_pool=2048",      {"iocp_batch_size": 256, "buffer_pool_max": 2048}),
            ("batch=256 + workers=4",          {"iocp_batch_size": 256, "io_worker_count": 4}),
            ("batch=256 + buf=256K + pool=2K", {"iocp_batch_size": 256, "buffer_size": 262144, "buffer_pool_max": 2048}),
            # 极限组合
            ("MAX: batch=256 + buf=512K + pool=4K + workers=8",
             {"iocp_batch_size": 256, "buffer_size": 524288, "buffer_pool_max": 4096, "io_worker_count": 8}),
        ]

        results = [baseline]
        for i, (name, cfg) in enumerate(configs_to_test, 2):
            print(f"\n[{i}] {name}")
            r = await test_config(name, cfg, path, CONCURRENT)
            speedup = r["ops"] / baseline["ops"]
            status = "🟢" if speedup > 1.05 else ("🟡" if speedup > 0.95 else "🔴")
            print(f"    {r['ops']:,.0f} ops/s  ({r['time']:.2f}s)  → {speedup:.2f}x {status}")
            results.append(r)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    # ── 排名 ──────────────────────────────────────────────────────────────
    results.sort(key=lambda r: r["ops"], reverse=True)

    print(f"\n{'='*95}")
    print(f"📊 调优排名  (基线: {baseline['ops']:,.0f} ops/s)")
    print(f"{'='*95}")
    print(f"{'排名':>4s}  {'配置':<42s}  {'ops/s':>10s}  {'耗时':>7s}  {'加速比':>7s}  {'异常'}")
    print("-" * 95)

    for rank, r in enumerate(results, 1):
        speedup = r["ops"] / baseline["ops"]
        bar = "█" * int(speedup * 10) if speedup >= 1 else ""
        print(f"{rank:>3d}   {r['name']:<42s}  {r['ops']:>10,.0f}  {r['time']:>6.2f}s  {speedup:>6.2f}x  {r['errs']:>4d}  {bar}")

    best = results[0]
    print(f"\n🏆 最佳配置: {best['name']}")
    print(f"   吞吐量: {best['ops']:,.0f} ops/s (默认的 {best['ops']/baseline['ops']:.2f}x)")
    print(f"   有效配置: {best['effective_cfg']}")


if __name__ == "__main__":
    asyncio.run(main())
