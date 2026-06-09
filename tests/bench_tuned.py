"""
Config tuning comparison: default vs tuned ayafileio

Tests the impact of different configuration combinations on HDD performance.
"""

import asyncio
import sys
import time
import os
import gc
import tempfile
import shutil
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
    """Apply config, run test, then restore defaults"""
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
    nk = CONCURRENT // 1000
    title = i18n("ayafileio Config Tuning — {n}K Concurrent Random Read (HDD)").format(n=nk)
    print(f"╔{'═' * (len(title) + 4)}╗")
    print(f"║   {title}   ║")
    print(f"╚{'═' * (len(title) + 4)}╝")
    print(f"Backend: {ayafileio.get_backend_info()['backend']}")
    print(i18n("File: {size}MB, Chunk: {chunk}B, Concurrency: {n}").format(size=FILE_SIZE_MB, chunk=CHUNK_SIZE, n=f"{CONCURRENT:,}"))

    tmpdir = tempfile.mkdtemp(prefix="aya_tune_")
    path = os.path.join(tmpdir, "tune_data.bin")

    try:
        msg = i18n("Preparing test file...")
        print(f"\n{msg}", end=" ", flush=True)
        prepare_file(path, FILE_SIZE_MB)
        print("OK")

        # ── Baseline ───────────────────────────────────────────────────────
        print(f"\n{i18n('[1] Default config (baseline)')}")
        baseline = await test_config(i18n("Default"), {}, path, CONCURRENT)
        print(f"    {baseline['ops']:,.0f} ops/s  ({baseline['time']:.2f}s)")

        # ── Config sweep ───────────────────────────────────────────────────
        configs_to_test = [
            ("batch=128",       {"iocp_batch_size": 128}),
            ("batch=256",       {"iocp_batch_size": 256}),
            ("buf=128K",        {"buffer_size": 131072}),
            ("buf=256K",        {"buffer_size": 262144}),
            ("buf_pool=1024",   {"buffer_pool_max": 1024}),
            ("buf_pool=2048",   {"buffer_pool_max": 2048}),
            ("workers=4",       {"io_worker_count": 4}),
            ("workers=8",       {"io_worker_count": 8}),
            ("batch=256 + buf=256K",           {"iocp_batch_size": 256, "buffer_size": 262144}),
            ("batch=256 + buf_pool=2048",      {"iocp_batch_size": 256, "buffer_pool_max": 2048}),
            ("batch=256 + workers=4",          {"iocp_batch_size": 256, "io_worker_count": 4}),
            ("batch=256 + buf=256K + pool=2K", {"iocp_batch_size": 256, "buffer_size": 262144, "buffer_pool_max": 2048}),
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

    # ── Ranking ────────────────────────────────────────────────────────────
    results.sort(key=lambda r: r["ops"], reverse=True)

    rank_title = i18n("📊 Tuning Ranking  (baseline: {n} ops/s)").format(n=f"{baseline['ops']:,.0f}")
    header_rank = i18n("Rank")
    header_cfg = i18n("Config")
    header_time = i18n("Time")
    header_speedup = i18n("Speedup")
    header_errs = i18n("Errors")

    print(f"\n{'='*95}")
    print(rank_title)
    print(f"{'='*95}")
    print(f"{header_rank:>4s}  {header_cfg:<42s}  {'ops/s':>10s}  {header_time:>7s}  {header_speedup:>7s}  {header_errs}")
    print("-" * 95)

    for rank, r in enumerate(results, 1):
        speedup = r["ops"] / baseline["ops"]
        bar = "█" * int(speedup * 10) if speedup >= 1 else ""
        print(f"{rank:>3d}   {r['name']:<42s}  {r['ops']:>10,.0f}  {r['time']:>6.2f}s  {speedup:>6.2f}x  {r['errs']:>4d}  {bar}")

    best = results[0]
    print()
    print(i18n("🏆 Best config: {name}").format(name=best['name']))
    print(i18n("   Throughput: {n} ops/s ({ratio}x of default)").format(n=f"{best['ops']:,.0f}", ratio=f"{best['ops']/baseline['ops']:.2f}"))
    print(i18n("   Effective config: {cfg}").format(cfg=best['effective_cfg']))


if __name__ == "__main__":
    asyncio.run(main())
