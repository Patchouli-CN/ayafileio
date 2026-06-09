"""
性能对比: ayafileio vs aiofiles vs sync(threadpool)

同等条件，单文件随机读取，测试真正的 I/O 并发能力。
"""

import asyncio
import sys
import time
import os
import gc
import tempfile
import shutil
import argparse
from pathlib import Path

if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).parent.parent))
import ayafileio

FILE_SIZE_MB = 20
CHUNK_SIZE = 256

# ════════════════════════════════════════════════════════════════════════════
# 准备
# ════════════════════════════════════════════════════════════════════════════

def prepare_file(path: str, size_mb: int):
    chunk = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 256
    target = size_mb * 1024 * 1024
    written = 0
    with open(path, "wb") as f:
        while written < target:
            f.write(chunk)
            written += len(chunk)

def mem_str():
    try:
        import psutil
        m = psutil.Process().memory_info()
        return f"RSS={m.rss/1024/1024:.0f}MB"
    except ImportError:
        return ""


# ════════════════════════════════════════════════════════════════════════════
# 三个选手
# ════════════════════════════════════════════════════════════════════════════

async def bench_ayafileio(path: str, concurrent: int):
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


async def bench_aiofiles(path: str, concurrent: int):
    try:
        import aiofiles
    except ImportError:
        raise RuntimeError("aiofiles not installed")

    file_size = os.path.getsize(path)
    max_offset = file_size - CHUNK_SIZE
    async with aiofiles.open(path, "rb") as f:
        async def rdr(i):
            await f.seek((i * 9973 + 123457) % max_offset)
            return len(await f.read(CHUNK_SIZE))
        tasks = [asyncio.create_task(rdr(i)) for i in range(concurrent)]
        results = await asyncio.gather(*tasks, return_exceptions=True)
    ok = sum(1 for r in results if isinstance(r, int) and r == CHUNK_SIZE)
    errs = sum(1 for r in results if isinstance(r, BaseException))
    return ok, errs


async def bench_sync_threadpool(path: str, concurrent: int):
    file_size = os.path.getsize(path)
    max_offset = file_size - CHUNK_SIZE

    def sync_read(offset: int) -> int:
        with open(path, "rb") as f:
            f.seek(offset)
            return len(f.read(CHUNK_SIZE))

    loop = asyncio.get_running_loop()
    tasks = [loop.run_in_executor(None, sync_read, (i * 9973 + 123457) % max_offset)
             for i in range(concurrent)]
    results = await asyncio.gather(*tasks, return_exceptions=True)
    ok = sum(1 for r in results if isinstance(r, int) and r == CHUNK_SIZE)
    errs = sum(1 for r in results if isinstance(r, BaseException))
    return ok, errs


# ════════════════════════════════════════════════════════════════════════════
# 单个测试运行
# ════════════════════════════════════════════════════════════════════════════

async def run_one(name: str, fn, path: str, concurrent: int):
    gc.collect()
    t0 = time.perf_counter()
    try:
        ok, errs = await fn(path, concurrent)
    except Exception as e:
        return {"name": name, "ok": 0, "errs": 0, "time": 0, "ops": 0, "error": str(e)}
    elapsed = time.perf_counter() - t0
    return {
        "name": name,
        "ok": ok, "errs": errs,
        "time": elapsed,
        "ops": ok / elapsed if elapsed > 0 else 0,
    }


# ════════════════════════════════════════════════════════════════════════════
# Main
# ════════════════════════════════════════════════════════════════════════════

async def main(concurrencies):
    print(f"╔══════════════════════════════════════════════════════════════╗")
    print(f"║   ayafileio vs aiofiles vs sync(threadpool) 性能对比        ║")
    print(f"╚══════════════════════════════════════════════════════════════╝")
    print(f"Python: {sys.version.split()[0]}, Backend: {ayafileio.get_backend_info()['backend']}")
    print(f"文件: {FILE_SIZE_MB}MB, 块大小: {CHUNK_SIZE}B, 随机读取")
    print(f"阶梯: {[f'{c//1000}K' for c in concurrencies]}")

    tmpdir = tempfile.mkdtemp(prefix="aya_bench_")
    read_path = os.path.join(tmpdir, "bench_data.bin")

    try:
        print(f"\n准备 {FILE_SIZE_MB}MB 测试文件...", end=" ", flush=True)
        t0 = time.perf_counter()
        prepare_file(read_path, FILE_SIZE_MB)
        print(f"{time.perf_counter() - t0:.1f}s")

        all_rows = []
        benches = [
            ("ayafileio (IOCP)", bench_ayafileio),
            ("aiofiles",          bench_aiofiles),
            ("sync (threadpool)", bench_sync_threadpool),
        ]

        for n in concurrencies:
            print(f"\n{'─'*60}")
            print(f" 并发: {n:,} ({n//1000}K)")
            print(f"{'─'*60}")

            row = {"concurrent": n, "results": []}
            for name, fn in benches:
                r = await run_one(name, fn, read_path, n)
                row["results"].append(r)
                if r.get("error"):
                    print(f"  {name:25s} ❌ {r['error']}")
                else:
                    print(f"  {name:25s} {r['ops']:>10,.0f} ops/s  ({r['time']:.2f}s, err={r['errs']})")
            all_rows.append(row)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    # ── 汇总 ──────────────────────────────────────────────────────────────
    print(f"\n{'='*90}")
    print("📊 性能对比总表")
    print(f"{'='*90}")
    print(f"{'并发':>7s}  {'ayafileio':>12s}  {'aiofiles':>12s}  {'sync(tp)':>12s}  {'vs aiofiles':>13s}  {'vs sync':>12s}")
    print("-" * 90)

    for row in all_rows:
        n = row["concurrent"]
        vals = {}
        for r in row["results"]:
            vals[r["name"]] = r["ops"] if not r.get("error") else 0

        aya = vals.get("ayafileio (IOCP)", 0)
        aio = vals.get("aiofiles", 0)
        sync = vals.get("sync (threadpool)", 0)

        sa = f"{aya/aio:.1f}x" if aio > 0 else "N/A"
        ss = f"{aya/sync:.1f}x" if sync > 0 else "N/A"

        print(f"{n:>5,}  {aya:>10,.0f}  {aio:>10,.0f}  {sync:>10,.0f}  {sa:>13s}  {ss:>12s}")

    print()
    last = all_rows[-1]["results"]
    aya_last = [r for r in last if "ayafileio" in r["name"]][0]
    aio_last = [r for r in last if "aiofiles" in r["name"]][0]
    if not aya_last.get("error") and not aio_last.get("error") and aio_last["ops"] > 0:
        speedup = aya_last["ops"] / aio_last["ops"]
        print(f"🏆 在最高并发下, ayafileio 比 aiofiles 快 {speedup:.1f}x")
        print(f"   ayafileio 的真正优势: 零后台线程、内核 IOCP 完成通知、无 GIL 竞争")

    aya_errs = any(r["errs"] > 0 for row in all_rows for r in row["results"] if "ayafileio" in r["name"])
    if not aya_errs:
        print(f"   ayafileio 在所有并发级别下: ✅ 零异常")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--max", type=int, default=100_000,
                        help="最大并发数 (默认 100K)")
    args = parser.parse_args()

    if args.max <= 1000:
        steps = [100, 500, 1000]
    elif args.max <= 10000:
        steps = [1000, 5000, 10000]
    elif args.max <= 50000:
        steps = [1000, 10000, 50000]
    else:
        steps = [1000, 10000, 50000, 100000]

    asyncio.run(main(steps))
