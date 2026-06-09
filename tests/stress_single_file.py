"""
Single-file 500K concurrent stress test

The real hardcore scenario: massive concurrent IOCP read/write on a
single shared file handle, without wasting time on open/close overhead.

Usage:
  python tests/stress_single_file.py --step
"""

import asyncio
import sys
import time
import os
import gc
import tempfile
import argparse
from pathlib import Path

if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).parent.parent))
from i18n import i18n
import ayafileio

DEFAULT_CONCURRENT = 500_000
FILE_SIZE_MB = 10


def print_mem(label: str = ""):
    """Print current memory usage"""
    try:
        import psutil
        mem = psutil.Process().memory_info()
        print(f"  [{label}] RSS={mem.rss/1024/1024:.1f}MB, VMS={mem.vms/1024/1024:.1f}MB")
    except ImportError:
        pass


# ════════════════════════════════════════════════════════════════════════════
# Prepare test file
# ════════════════════════════════════════════════════════════════════════════

def prepare_file(path: str, size_mb: int):
    """Sync write test file (avoid async overhead)"""
    msg = i18n("Preparing test file ({size}MB)...").format(size=size_mb)
    print(msg)
    t0 = time.perf_counter()
    chunk = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 256  # ~10KB
    written = 0
    target = size_mb * 1024 * 1024
    with open(path, "wb") as f:
        while written < target:
            f.write(chunk)
            written += len(chunk)
    elapsed = time.perf_counter() - t0
    size_actual = os.path.getsize(path) / 1024 / 1024
    print(i18n("  File created: {size}MB, time {elapsed}s").format(size=f"{size_actual:.1f}", elapsed=f"{elapsed:.2f}"))


# ════════════════════════════════════════════════════════════════════════════
# Staircase concurrency tests
# ════════════════════════════════════════════════════════════════════════════

async def test_read_concurrent(path: str, concurrent: int, chunk_size: int = 256):
    """N concurrent reads sharing a single file handle — true IOCP stress test."""
    file_size = os.path.getsize(path)
    max_offset = file_size - chunk_size

    print(i18n("  ── Read {n} concurrent (chunk={chunk}B) ──").format(n=f"{concurrent:,}", chunk=chunk_size))
    print_mem(i18n("Before test"))

    gc.collect()
    gc.disable()

    async with ayafileio.open(path, "rb") as f:
        async def read_one(i: int):
            offset = (i * 9973 + 123457) % max_offset
            await f.seek(offset)
            data = await f.read(chunk_size)
            return len(data)

        t0 = time.perf_counter()

        tasks = []
        batch = 20000
        for start in range(0, concurrent, batch):
            end = min(start + batch, concurrent)
            for i in range(start, end):
                tasks.append(asyncio.create_task(read_one(i)))
            elapsed = time.perf_counter() - t0
            if start % 100000 == 0 or end == concurrent:
                msg = i18n("    Created {end} / {concurrent} ({elapsed}s)").format(
                    end=f"{end:,}", concurrent=f"{concurrent:,}", elapsed=f"{elapsed:.1f}")
                print(msg, end="\r")
        t_create = time.perf_counter() - t0
        msg = i18n("    Tasks created: {n} tasks, {t}s").format(n=f"{len(tasks):,}", t=f"{t_create:.2f}")
        print(f"\n{msg}")

        print_mem(i18n("After task creation"))

        # Wait for all to complete
        t_wait = time.perf_counter()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        t_gather = time.perf_counter() - t_wait

    successes = sum(1 for r in results if isinstance(r, int) and r == chunk_size)
    partials = sum(1 for r in results if isinstance(r, int) and r != chunk_size)
    exceptions = sum(1 for r in results if isinstance(r, BaseException))

    print(i18n("    Wait time: {t}s").format(t=f"{t_gather:.2f}"))
    print(i18n("    Success: {s}  |  Partial read: {p}  |  Errors: {e}").format(
        s=f"{successes:,}", p=f"{partials:,}", e=f"{exceptions:,}"))
    if successes > 0:
        print(i18n("    Throughput: {n} ops/s").format(n=f"{successes / t_gather:,.0f}"))

    if exceptions:
        exc_types = {}
        samples = []
        for r in results:
            if isinstance(r, BaseException):
                t = type(r).__name__
                exc_types[t] = exc_types.get(t, 0) + 1
                if len(samples) < 5:
                    samples.append(f"      {t}: {r}")
        print(i18n("    Exception types: {types}").format(types=str(exc_types)))
        print(i18n("    Samples:"))
        print("\n".join(samples))

    gc.enable()
    return successes, exceptions, t_gather


async def test_write_concurrent(path: str, concurrent: int, line_size: int = 64):
    """N concurrent append writes on a single file handle — IOCP write stress test."""
    print(i18n("  ── Append Write {n} concurrent (line={line}B) ──").format(n=f"{concurrent:,}", line=line_size))
    print_mem(i18n("Before test"))

    gc.collect()
    gc.disable()

    async with ayafileio.open(path, "a", encoding="utf-8") as f:
        async def write_one(i: int):
            data = f"{i:08d}_" + "X" * (line_size - 9) + "\n"
            n = await f.write(data)
            return n

        t0 = time.perf_counter()

        tasks = []
        batch = 20000
        for start in range(0, concurrent, batch):
            end = min(start + batch, concurrent)
            for i in range(start, end):
                tasks.append(asyncio.create_task(write_one(i)))
            elapsed = time.perf_counter() - t0
            if start % 100000 == 0 or end == concurrent:
                msg = i18n("    Created {end} / {concurrent} ({elapsed}s)").format(
                    end=f"{end:,}", concurrent=f"{concurrent:,}", elapsed=f"{elapsed:.1f}")
                print(msg, end="\r")
        t_create = time.perf_counter() - t0
        msg = i18n("    Tasks created: {n} tasks, {t}s").format(n=f"{len(tasks):,}", t=f"{t_create:.2f}")
        print(f"\n{msg}")

        print_mem(i18n("After task creation"))

        t_wait = time.perf_counter()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        t_gather = time.perf_counter() - t_wait

    expected = line_size + 1
    successes = sum(1 for r in results if isinstance(r, int) and r == expected)
    partials = sum(1 for r in results if isinstance(r, int) and r != expected)
    exceptions = sum(1 for r in results if isinstance(r, BaseException))

    print(i18n("    Wait time: {t}s").format(t=f"{t_gather:.2f}"))
    print(i18n("    Success: {s}  |  Partial write: {p}  |  Errors: {e}").format(
        s=f"{successes:,}", p=f"{partials:,}", e=f"{exceptions:,}"))
    if successes > 0:
        print(i18n("    Throughput: {n} ops/s").format(n=f"{successes / t_gather:,.0f}"))

    if exceptions:
        exc_types = {}
        samples = []
        for r in results:
            if isinstance(r, BaseException):
                t = type(r).__name__
                exc_types[t] = exc_types.get(t, 0) + 1
                if len(samples) < 5:
                    samples.append(f"      {t}: {r}")
        print(i18n("    Exception types: {types}").format(types=str(exc_types)))
        print(i18n("    Samples:"))
        print("\n".join(samples))

    gc.enable()
    return successes, exceptions, t_gather


# ════════════════════════════════════════════════════════════════════════════
# Staircase: 10K → 50K → 100K → 250K → 500K
# ════════════════════════════════════════════════════════════════════════════

STEPS = [10_000, 50_000, 100_000, 250_000, 500_000]


async def main(concurrent: int = DEFAULT_CONCURRENT):
    title = i18n("ayafileio Single-File Concurrent Stress Test — IOCP True Async Limit")
    print(f"╔{'═' * (len(title) + 4)}╗")
    print(f"║   {title}   ║")
    print(f"╚{'═' * (len(title) + 4)}╝")
    print(f"Python: {sys.version}")
    print(f"Backend: {ayafileio.get_backend_info()}")
    print(f"Event Loop: {type(asyncio.get_running_loop()).__name__}")
    print_mem(i18n("Startup"))

    tmpdir = tempfile.mkdtemp(prefix="aya_single_")

    try:
        # ── Prepare files ──────────────────────────────────────────────
        read_path = os.path.join(tmpdir, "read_test.bin")
        write_path = os.path.join(tmpdir, "write_test.bin")
        prepare_file(read_path, FILE_SIZE_MB)
        with open(write_path, "wb") as f:
            f.write(b"# ayafileio stress test\n")

        # ── Determine test steps ───────────────────────────────────────
        if concurrent in STEPS:
            steps = [s for s in STEPS if s <= concurrent]
        else:
            steps = STEPS

        all_results = {}

        # ── Phase 1: Staircase concurrent read ────────────────────────
        print(f"\n{'='*60}")
        print(i18n("Phase 1: Single-File Concurrent Read"))
        print(f"{'='*60}")
        for step in steps:
            try:
                s, e, t = await test_read_concurrent(read_path, step)
                all_results[f"read_{step//1000}K"] = {"successes": s, "exceptions": e, "time": t}
            except Exception as ex:
                msg = i18n("    ❌ {k}K read crash: {err}").format(k=step//1000, err=f"{type(ex).__name__}: {ex}")
                print(msg)
                import traceback
                traceback.print_exc()
                all_results[f"read_{step//1000}K"] = {"error": str(ex)}
                break

        # ── Phase 2: Staircase concurrent append write ─────────────────
        print(f"\n{'='*60}")
        print(i18n("Phase 2: Single-File Concurrent Append Write"))
        print(f"{'='*60}")
        for step in steps:
            try:
                s, e, t = await test_write_concurrent(write_path, step)
                all_results[f"write_{step//1000}K"] = {"successes": s, "exceptions": e, "time": t}
            except Exception as ex:
                msg = i18n("    ❌ {k}K write crash: {err}").format(k=step//1000, err=f"{type(ex).__name__}: {ex}")
                print(msg)
                import traceback
                traceback.print_exc()
                all_results[f"write_{step//1000}K"] = {"error": str(ex)}
                break

        # ── Summary ──────────────────────────────────────────────────
        print(f"\n{'='*60}")
        print(i18n("📊 Stress Test Summary"))
        print(f"{'='*60}")
        for k, v in all_results.items():
            if "error" in v:
                print(f"  {k:20s}: ❌ {v['error'][:80]}")
            else:
                tp = v["successes"] / v["time"] if v["time"] > 0 else 0
                status = "✅" if v["exceptions"] == 0 else f"⚠️ ({v['exceptions']} err)"
                print(f"  {k:20s}: {status}  {v['successes']:,} ops  {v['time']:.2f}s  {tp:,.0f} ops/s")

    finally:
        print(i18n("Cleaning {tmpdir}...").format(tmpdir=f"\n{tmpdir}"))
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--concurrent", type=int, default=DEFAULT_CONCURRENT,
                        help=f"Concurrency (default {DEFAULT_CONCURRENT:,})")
    parser.add_argument("--step", action="store_true",
                        help="Staircase progression (10K→50K→100K→250K→500K)")
    args = parser.parse_args()

    target = args.concurrent
    asyncio.run(main(target))
