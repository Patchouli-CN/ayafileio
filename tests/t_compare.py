#!/usr/bin/env python3
"""
ayafileio vs aiofiles — cross-platform benchmark (Windows / Linux / macOS).

Methodology:
  - Time-boxed rounds instead of fixed op counts: each measurement runs
    ~ROUND_SECONDS, so total runtime stays bounded on any machine.
  - One warmup + ROUNDS measured rounds per cell, median reported.
    Backends are interleaved round-by-round so page-cache state and
    machine load are shared fairly.
  - Concurrent scenarios use positioned I/O (read_at) or one handle per
    worker — never seek+read on a shared handle, which races by design.
  - Results are written to benchmark_results.json (summary) and
    benchmark_results_detailed.json (all raw rounds) in the CWD,
    which the CI benchmark job uploads as artifacts.

Scenarios:
  A. seq_read        sequential whole-file read, several chunk sizes (MB/s)
  B. seq_write       sequential file write, several chunk sizes (MB/s)
  C. small_read      sequential 4 KiB reads, per-op overhead (ops/s)
  D. rand_read_own   random 4 KiB positioned reads, one handle per worker (ops/s)
  E. rand_read_at    random 4 KiB read_at on a single shared handle,
                     ayafileio only — aiofiles has no positioned I/O (ops/s)
  F. batch_open      BATCH_FILES tiny files per batch, opened+read+closed
                     concurrently (files/s): ayafileio read_bytes_many
                     (parallel aopen + async reads), ayafileio
                     gather(read_bytes) (synchronous open on the loop thread
                     — the isolation row that attributes the lead to aopen),
                     and aiofiles' default gather(open+read+close).
  G. batch_open_only opens only, one batch per round (files/s): aopen vs
                     aiofiles open+close (two executor round-trips).

  Batch scenarios drain the Windows handle pool once per round, so every
  round starts cold; later batches within a round reuse cached handles,
  which is the out-of-the-box behavior of each library. On POSIX the batch
  is sized by doubling the real concurrent batch until the library itself
  refuses: getrlimit lies on macOS (XNU clips at MIN(RLIMIT_NOFILE,
  kern.maxfilesperproc) at open time, and GitHub's macOS runners sit on
  launchd's default 256), and a sequential probe cannot see the budget
  because the semaphore plus asynchronous reclamation make peak usage a
  multiple of the in-flight count.

aiofiles is opened with its defaults (buffered), ayafileio is unbuffered;
this reflects what a user gets out of the box from each library.
"""

import asyncio
import gc
import io
import json
import os
import platform
import random
import shutil
import statistics
import sys
import tempfile
import time

# Windows console/CI: force UTF-8 so non-ASCII output doesn't garble on cp1252
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import ayafileio

try:
    import aiofiles
    HAS_AIOFILES = True
except ImportError:
    HAS_AIOFILES = False

# ── Tuning ──────────────────────────────────────────────────────────────────
FILE_SIZE = 256 * 1024 * 1024          # shared data file for read scenarios
PREP_CHUNK = 4 * 1024 * 1024
ROUND_SECONDS = 1.0                    # measured time per round
WARMUP_SECONDS = 0.3
ROUNDS = 3
HARD_CAP_SECONDS = 30                  # per-round kill switch, never hit in practice

SEQ_CHUNKS = [64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024]
SMALL_CHUNK = 4096
CONCURRENCY_LEVELS = [1, 16, 64, 256]
RNG_SEED = 42

# Batch-open scenarios: many tiny files opened concurrently per batch.
# 64 B payloads keep the cost in open/teardown rather than read throughput.
BATCH_FILES = 1024
BATCH_PAYLOAD = 64


# ── Helpers ─────────────────────────────────────────────────────────────────

def fmt_rate(value: float, unit: str) -> str:
    if unit == "MB/s":
        return f"{value:8.1f}"
    if value >= 1_000_000:
        return f"{value / 1_000_000:7.2f}M"
    if value >= 1_000:
        return f"{value / 1_000:7.1f}K"
    return f"{value:8.0f}"


def prepare_file(path: str, size: int) -> None:
    """Create a file of exactly `size` bytes with non-repeating content."""
    block = random.Random(RNG_SEED).randbytes(PREP_CHUNK)
    with open(path, "wb") as f:
        written = 0
        while written < size:
            n = min(PREP_CHUNK, size - written)
            f.write(block[:n])
            written += n


def random_offsets(count: int, chunk: int) -> list[int]:
    rng = random.Random(RNG_SEED)
    max_off = (FILE_SIZE - chunk) // chunk
    return [rng.randrange(max_off) * chunk for _ in range(count)]


def prepare_batch_dir(dirpath: str, count: int) -> list[str]:
    """Create `count` tiny files; returns their paths in order."""
    os.makedirs(dirpath, exist_ok=True)
    payload = random.Random(RNG_SEED).randbytes(BATCH_PAYLOAD)
    paths = [os.path.join(dirpath, f"f{i:05d}.bin") for i in range(count)]
    for p in paths:
        with open(p, "wb") as f:
            f.write(payload)
    return paths


async def probe_batch_size(paths: list[str]) -> int:
    """Size the batch by doubling the real (concurrent) batch until it fails.

    Two macOS traps make every static model wrong here. First, getrlimit
    lies: XNU stores whatever RLIMIT_NOFILE you set and clips at
    MIN(RLIMIT_NOFILE, kern.maxfilesperproc) when a file is actually opened,
    so a reported 64K can enforce 256 (GitHub's macOS runners run under
    launchd's default 256, and raising it is not permitted). Second, a
    sequential probe cannot see the budget either: the semaphore keeps ~64
    files open at once and reclamation lags, so peak usage is several times
    the in-flight count. So probe with the library's own concurrent path —
    double the batch until read_bytes_many itself refuses, then settle for
    the largest size that passed.
    """
    if os.name != "posix":
        return min(BATCH_FILES, len(paths))
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        print(f"  fd limits: soft={soft} hard={hard}")
        if hard == resource.RLIM_INFINITY:
            hard = 65536
        if hard > soft:
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, 65536), hard))
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            print(f"  fd limits raised to: soft={soft} hard={hard}")
    except (ImportError, OSError, ValueError):
        pass
    size = 0
    n = 16
    while n <= len(paths):
        try:
            await ayafileio.read_bytes_many(paths[:n])
        except OSError:
            break
        size = n
        # A refused batch abandons in-flight siblings whose fds are reclaimed
        # asynchronously; give the reaper a moment before asking for more.
        gc.collect()
        await asyncio.sleep(0.25)
        n *= 2
    result = max(4, size)
    if result != BATCH_FILES:
        print(f"  note: batch size {result} (probed with the real API)")
    return result


# ── Scenario runners ────────────────────────────────────────────────────────
# Each runner performs one unit of work; the measurement loop drives them
# until the time budget is spent.

async def aya_seq_read(f, chunk: int, stop: float) -> tuple[int, int]:
    ops = 0
    while time.perf_counter() < stop:
        data = await f.read(chunk)
        if not data:
            await f.seek(0)
        ops += 1
    return ops, chunk * ops


async def aio_seq_read(f, chunk: int, stop: float) -> tuple[int, int]:
    ops = 0
    while time.perf_counter() < stop:
        data = await f.read(chunk)
        if not data:
            await f.seek(0)
        ops += 1
    return ops, chunk * ops


async def aya_seq_write(f, buf: bytes, stop: float) -> tuple[int, int]:
    ops = 0
    while time.perf_counter() < stop:
        await f.write(buf)
        ops += 1
        if await f.tell() >= FILE_SIZE:
            await f.seek(0)
    return ops, len(buf) * ops


async def aio_seq_write(f, buf: bytes, stop: float) -> tuple[int, int]:
    ops = 0
    while time.perf_counter() < stop:
        await f.write(buf)
        ops += 1
        if await f.tell() >= FILE_SIZE:
            await f.seek(0)
    return ops, len(buf) * ops


async def aya_rand_read_own(f, offsets: list[int], pos: list[int], chunk: int, stop: float) -> int:
    ops = 0
    i = pos[0] % len(offsets)
    while time.perf_counter() < stop:
        await f.read_at(offsets[i], chunk)
        i = (i + 1) % len(offsets)
        ops += 1
    pos[0] = i
    return ops


async def aio_rand_read_own(f, offsets: list[int], pos: list[int], chunk: int, stop: float) -> int:
    ops = 0
    i = pos[0]
    while time.perf_counter() < stop:
        await f.seek(offsets[i])
        await f.read(chunk)
        i = (i + 1) % len(offsets)
        ops += 1
    pos[0] = i
    return ops


# ── Batch-open runners (one batch = BATCH_FILES tiny files) ──────────────────

async def aya_batch_many(paths: list[str], stop: float) -> int:
    """read_bytes_many: parallel opens (aopen) + real async reads, one call."""
    files = 0
    while time.perf_counter() < stop:
        await ayafileio.read_bytes_many(paths)
        files += len(paths)
    return files


async def aya_batch_gather_rb(paths: list[str], stop: float) -> int:
    """gather(read_bytes): synchronous open() on the loop thread + async reads."""
    files = 0
    while time.perf_counter() < stop:
        await asyncio.gather(*(ayafileio.read_bytes(p) for p in paths))
        files += len(paths)
    return files


async def aio_batch_gather(paths: list[str], stop: float) -> int:
    """aiofiles at its defaults: open, read and close each ride the executor."""
    files = 0
    while time.perf_counter() < stop:
        async def one(p):
            async with aiofiles.open(p, "rb") as f:
                return await f.read()
        await asyncio.gather(*(one(p) for p in paths))
        files += len(paths)
    return files


async def aya_batch_open_only(paths: list[str], stop: float) -> int:
    """Exactly one batch of aopens, no reads — the batch is the unit."""
    async def one(p):
        async with ayafileio.aopen(p, "rb") as f:
            pass
    await asyncio.gather(*(one(p) for p in paths))
    return len(paths)


async def aio_batch_open_only(paths: list[str], stop: float) -> int:
    """Exactly one batch of aiofiles open+close (two executor round-trips)."""
    async def one(p):
        async with aiofiles.open(p, "rb") as f:
            pass
    await asyncio.gather(*(one(p) for p in paths))
    return len(paths)


# ── Measurement engine ──────────────────────────────────────────────────────

async def measure_once(kind: str, backend: str, path: str, chunk: int,
                       budget: float, concurrency: int = 1,
                        paths: list[str] | None = None) -> tuple[int, float]:
    """Run one measurement. Returns (ops, elapsed_seconds)."""
    stop = time.perf_counter() + budget
    start = time.perf_counter()

    if kind in ("seq_read", "small_read"):
        if backend == "aya":
            async with ayafileio.open(path, "rb") as f:
                ops, _ = await asyncio.wait_for(aya_seq_read(f, chunk, stop), HARD_CAP_SECONDS)
        else:
            async with aiofiles.open(path, "rb") as f:
                ops, _ = await asyncio.wait_for(aio_seq_read(f, chunk, stop), HARD_CAP_SECONDS)

    elif kind == "seq_write":
        buf = random.Random(1).randbytes(chunk)
        if backend == "aya":
            async with ayafileio.open(path, "r+b") as f:
                ops, _ = await asyncio.wait_for(aya_seq_write(f, buf, stop), HARD_CAP_SECONDS)
        else:
            async with aiofiles.open(path, "r+b") as f:
                ops, _ = await asyncio.wait_for(aio_seq_write(f, buf, stop), HARD_CAP_SECONDS)

    elif kind == "rand_read_own":
        offsets = random_offsets(4096, chunk)

        async def worker_aya():
            async with ayafileio.open(path, "rb") as f:
                return await aya_rand_read_own(f, offsets, [0], chunk, stop)

        async def worker_aio():
            async with aiofiles.open(path, "rb") as f:
                return await aio_rand_read_own(f, offsets, [0], chunk, stop)

        worker = worker_aya if backend == "aya" else worker_aio
        counts = await asyncio.wait_for(
            asyncio.gather(*(worker() for _ in range(concurrency))),
            HARD_CAP_SECONDS)
        ops = sum(counts)

    elif kind == "rand_read_at":
        # Single shared handle, positioned reads — ayafileio only.
        offsets = random_offsets(4096, chunk)

        async def worker(pos: list[int]):
            return await aya_rand_read_own(shared_f, offsets, pos, chunk, stop)

        async with ayafileio.open(path, "rb") as shared_f:
            counts = await asyncio.wait_for(
                asyncio.gather(*(worker([i * 1024]) for i in range(concurrency))),
                HARD_CAP_SECONDS)
        ops = sum(counts)

    elif kind in ("batch_open", "batch_open_only"):
        # Many tiny files per batch, all opened concurrently. The Windows
        # handle pool is drained first so every round starts cold; batches
        # repeated inside the round then reuse cached handles, which is the
        # out-of-the-box behavior of each library. The opens-only variants
        # measure exactly one batch per round (a time-boxed loop would be
        # dominated by pool hits, not by open cost).
        ayafileio.drain_handle_pool()
        if kind == "batch_open":
            runner = {"aya_many": aya_batch_many,
                      "aya_sync": aya_batch_gather_rb,
                      "aio": aio_batch_gather}[backend]
        else:
            runner = {"aya": aya_batch_open_only,
                      "aio": aio_batch_open_only}[backend]
        ops = await asyncio.wait_for(runner(paths, stop), HARD_CAP_SECONDS)

    else:
        raise ValueError(f"unknown scenario kind: {kind}")

    return ops, time.perf_counter() - start


async def run_cell(kind: str, backend: str, path: str, chunk: int,
                   concurrency: int = 1, paths: list[str] | None = None) -> dict:
    """Warmup + ROUNDS measured rounds for one cell. Returns raw round data."""
    await measure_once(kind, backend, path, chunk, WARMUP_SECONDS, concurrency, paths)
    rounds = []
    for _ in range(ROUNDS):
        ops, elapsed = await measure_once(kind, backend, path, chunk, ROUND_SECONDS, concurrency, paths)
        rounds.append({"ops": ops, "elapsed": round(elapsed, 4),
                       "ops_per_s": round(ops / elapsed, 1)})
    return {"rounds": rounds, "median_ops_per_s": statistics.median(r["ops_per_s"] for r in rounds)}


# ── Report ──────────────────────────────────────────────────────────────────

def print_table(title: str, unit: str, rows: list[dict]) -> None:
    print(f"\n== {title} ({unit}) ==")
    header = f"  {'cell':<12}"
    if HAS_AIOFILES:
        header += f"{'ayafileio':>12}{'aiofiles':>12}{'aya/aio':>9}"
    else:
        header += f"{'ayafileio':>12}"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for row in rows:
        line = f"  {row['label']:<12}"
        aya = row.get("aya")
        aio = row.get("aio")
        if aya is not None:
            line += f"{fmt_rate(aya, unit):>12}"
        if HAS_AIOFILES:
            line += f"{fmt_rate(aio, unit):>12}" if aio is not None else f"{'n/a':>12}"
            if aya is not None and aio:
                line += f"{aya / aio:8.2f}x"
            else:
                line += f"{'—':>9}"
        print(line)


async def main() -> int:
    info = ayafileio.get_backend_info()
    print("ayafileio benchmark — cross-platform comparison")
    print(f"  platform : {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"  python   : {platform.python_version()}")
    print(f"  backend  : {info['backend']} (truly async: {info['is_truly_async']})")
    print(f"  compare  : {'aiofiles (thread pool, buffered defaults)' if HAS_AIOFILES else 'not installed — ayafileio only'}")
    print(f"  rounds   : {ROUNDS}x{ROUND_SECONDS}s measured + {WARMUP_SECONDS}s warmup per cell")

    results = {"meta": {
        "platform": platform.system(),
        "platform_release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "backend": info,
        "has_aiofiles": HAS_AIOFILES,
        "file_size": FILE_SIZE,
        "round_seconds": ROUND_SECONDS,
        "rounds": ROUNDS,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }, "scenarios": {}}
    detailed = {"meta": results["meta"], "scenarios": {}}

    tmpdir = tempfile.mkdtemp(prefix="aya_bench_")
    data_path = os.path.join(tmpdir, "data.bin")
    write_path = os.path.join(tmpdir, "write.bin")

    try:
        print(f"\nPreparing {FILE_SIZE // (1024 * 1024)} MiB data file...", end="", flush=True)
        t0 = time.perf_counter()
        prepare_file(data_path, FILE_SIZE)
        prepare_file(write_path, FILE_SIZE)
        print(f" {time.perf_counter() - t0:.1f}s")

        backends = ["aya"] + (["aio"] if HAS_AIOFILES else [])

        # ── A. sequential read ────────────────────────────────────────────
        rows, cells, det = [], {}, {}
        for chunk in SEQ_CHUNKS:
            label = f"{chunk // 1024}KiB"
            row = {"label": label}
            for be in backends:
                r = await run_cell("seq_read", be, data_path, chunk)
                mb_s = r["median_ops_per_s"] * chunk / (1024 * 1024)
                row[be] = mb_s
                det[f"{label}/{be}"] = r
            cells[label] = row
            rows.append(row)
        print_table("A. Sequential read", "MB/s", rows)
        results["scenarios"]["seq_read"] = {"unit": "MB/s", "cells": cells}
        detailed["scenarios"]["seq_read"] = det

        # ── B. sequential write ───────────────────────────────────────────
        rows, cells, det = [], {}, {}
        for chunk in SEQ_CHUNKS:
            label = f"{chunk // 1024}KiB"
            row = {"label": label}
            for be in backends:
                r = await run_cell("seq_write", be, write_path, chunk)
                mb_s = r["median_ops_per_s"] * chunk / (1024 * 1024)
                row[be] = mb_s
                det[f"{label}/{be}"] = r
            cells[label] = row
            rows.append(row)
        print_table("B. Sequential write", "MB/s", rows)
        results["scenarios"]["seq_write"] = {"unit": "MB/s", "cells": cells}
        detailed["scenarios"]["seq_write"] = det

        # ── C. small sequential read (per-op overhead) ────────────────────
        rows, cells, det = [], {}, {}
        row = {"label": f"{SMALL_CHUNK // 1024}KiB"}
        for be in backends:
            r = await run_cell("small_read", be, data_path, SMALL_CHUNK)
            row[be] = r["median_ops_per_s"]
            det[f"{be}"] = r
        rows.append(row)
        cells[row["label"]] = row
        print_table("C. Small sequential read", "ops/s", rows)
        results["scenarios"]["small_read"] = {"unit": "ops/s", "cells": cells}
        detailed["scenarios"]["small_read"] = det

        # ── D. random read, one handle per worker ─────────────────────────
        rows, cells, det = [], {}, {}
        for conc in CONCURRENCY_LEVELS:
            label = f"x{conc}"
            row = {"label": label}
            for be in backends:
                r = await run_cell("rand_read_own", be, data_path, SMALL_CHUNK, conc)
                row[be] = r["median_ops_per_s"]
                det[f"{label}/{be}"] = r
            cells[label] = row
            rows.append(row)
        print_table("D. Random 4KiB read (own handle)", "ops/s", rows)
        results["scenarios"]["rand_read_own"] = {"unit": "ops/s", "cells": cells}
        detailed["scenarios"]["rand_read_own"] = det

        # ── E. random read_at, shared handle (ayafileio only) ─────────────
        rows, cells, det = [], {}, {}
        for conc in CONCURRENCY_LEVELS:
            label = f"x{conc}"
            r = await run_cell("rand_read_at", "aya", data_path, SMALL_CHUNK, conc)
            row = {"label": label, "aya": r["median_ops_per_s"]}
            det[label] = r
            cells[label] = row
            rows.append(row)
        print_table("E. Random 4KiB read_at (shared handle)", "ops/s", rows)
        results["scenarios"]["rand_read_at"] = {"unit": "ops/s", "cells": cells}
        detailed["scenarios"]["rand_read_at"] = det

        # ── F. batch open: many tiny files, all opened concurrently ────────
        rows, cells, det = [], {}, {}
        batch_dir = os.path.join(tmpdir, "batch")
        all_batch_paths = prepare_batch_dir(batch_dir, BATCH_FILES)
        n_batch = await probe_batch_size(all_batch_paths)
        batch_paths = all_batch_paths[:n_batch]
        r_aio = await run_cell("batch_open", "aio", data_path, SMALL_CHUNK, paths=batch_paths)
        r_many = await run_cell("batch_open", "aya_many", data_path, SMALL_CHUNK, paths=batch_paths)
        r_sync = await run_cell("batch_open", "aya_sync", data_path, SMALL_CHUNK, paths=batch_paths)
        aio_rate = r_aio["median_ops_per_s"]
        for label, r in (("read_bytes_many", r_many), ("gather(rb)", r_sync)):
            row = {"label": label, "aya": r["median_ops_per_s"], "aio": aio_rate}
            cells[label] = row
            rows.append(row)
            det[label] = r
        det["aiofiles"] = r_aio
        print_table(f"F. Batch open ({n_batch} files x {BATCH_PAYLOAD}B)", "files/s", rows)
        results["scenarios"]["batch_open"] = {"unit": "files/s", "batch_files": n_batch, "cells": cells}
        detailed["scenarios"]["batch_open"] = det

        # ── G. batch open, opens only (aopen vs executor round-trips) ──────
        rows, cells, det = [], {}, {}
        r_aio_o = await run_cell("batch_open_only", "aio", data_path, SMALL_CHUNK, paths=batch_paths)
        r_aya_o = await run_cell("batch_open_only", "aya", data_path, SMALL_CHUNK, paths=batch_paths)
        row = {"label": "aopen only", "aya": r_aya_o["median_ops_per_s"],
               "aio": r_aio_o["median_ops_per_s"]}
        cells["aopen only"] = row
        rows.append(row)
        det["aopen only"] = r_aya_o
        det["aiofiles"] = r_aio_o
        print_table(f"G. Batch open, opens only ({n_batch} files)", "files/s", rows)
        results["scenarios"]["batch_open_only"] = {"unit": "files/s", "batch_files": n_batch, "cells": cells}
        detailed["scenarios"]["batch_open_only"] = det

    finally:
        for p in (data_path, write_path):
            try:
                os.unlink(p)
            except OSError:
                pass
        shutil.rmtree(os.path.join(tmpdir, "batch"), ignore_errors=True)
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass

    with open("benchmark_results.json", "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    with open("benchmark_results_detailed.json", "w", encoding="utf-8") as f:
        json.dump(detailed, f, indent=2)
    print("\nWrote benchmark_results.json + benchmark_results_detailed.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
