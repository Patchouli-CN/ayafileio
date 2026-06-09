"""
Performance comparison: ayafileio vs aiofiles vs sync(threadpool)

Single-file random read — testing true async I/O capability.
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
from i18n import i18n
import ayafileio

FILE_SIZE_MB = 20
CHUNK_SIZE = 256

# ════════════════════════════════════════════════════════════════════════════
# Setup
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
# Three contenders
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
# Single test runner
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
    title = i18n("ayafileio vs aiofiles vs sync(threadpool) Performance Comparison")
    print(f"╔{'═' * (len(title) + 4)}╗")
    print(f"║   {title}   ║")
    print(f"╚{'═' * (len(title) + 4)}╝")
    print(f"Python: {sys.version.split()[0]}, Backend: {ayafileio.get_backend_info()['backend']}")
    print(i18n("File: {size}MB, Chunk: {chunk}B, Random Read").format(size=FILE_SIZE_MB, chunk=CHUNK_SIZE))
    steps_str = ", ".join(f"{c//1000}K" for c in concurrencies)
    print(i18n("Steps: {steps}").format(steps=steps_str))

    tmpdir = tempfile.mkdtemp(prefix="aya_bench_")
    read_path = os.path.join(tmpdir, "bench_data.bin")

    try:
        msg = i18n("Preparing {size}MB test file...").format(size=FILE_SIZE_MB)
        print(f"\n{msg}", end=" ", flush=True)
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
            k = n // 1000
            print(f"\n{'─'*60}")
            print(i18n(" Concurrency: {n} ({k}K)").format(n=f"{n:,}", k=k))
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

    # ── Summary ────────────────────────────────────────────────────────────
    summary_title = i18n("📊 Performance Comparison Summary")
    header_concur = i18n("Concurrency")
    header_vs_aio = i18n("vs aiofiles")
    header_vs_sync = i18n("vs sync")

    print(f"\n{'='*90}")
    print(summary_title)
    print(f"{'='*90}")
    print(f"{header_concur:>7s}  {'ayafileio':>12s}  {'aiofiles':>12s}  {'sync(tp)':>12s}  {header_vs_aio:>13s}  {header_vs_sync:>12s}")
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
        print(i18n("🏆 At max concurrency, ayafileio is {x:.1f}x faster than aiofiles").format(x=speedup))
        print(i18n("   ayafileio advantage: zero background threads, kernel IOCP completion, zero GIL contention"))

    aya_errs = any(r["errs"] > 0 for row in all_rows for r in row["results"] if "ayafileio" in r["name"])
    if not aya_errs:
        print(i18n("   ayafileio at all concurrency levels: ✅ zero errors"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--max", type=int, default=100_000,
                        help="Maximum concurrency (default 100K)")
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
