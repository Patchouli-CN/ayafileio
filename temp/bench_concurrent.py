"""High-concurrency positional-read benchmark: ayafileio vs threadpool pread.

A) Random 4KB pread, 20k ops per level, in-flight cap 16/64/256/1024
   - ayafileio read_at (one shared handle, true parallel)
   - ayafileio read_many (batch submit)
   - os.pread via run_in_executor (what aiofiles-style threadpool can do;
     aiofiles itself has no pread, seek+read on a shared handle is racy)
B) Sequential read chunk scaling 64K/256K/1M/4M: ayafileio read_at vs aiofiles
"""

import asyncio
import io
import os
import sys
import tempfile
import time
import gc
from concurrent.futures import ThreadPoolExecutor

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

import aiofiles
import ayafileio

FILE_MB = 256
CHUNK = 4096
TOTAL_OPS = 20_000
LEVELS = (16, 64, 256, 1024)


def make_offsets(total: int, max_off: int) -> list[int]:
    return [((i * 9973 + 123457) % max_off) & ~4095 for i in range(total)]


async def aya_read_at(path: str, offsets: list[int], inflight: int) -> float:
    sem = asyncio.Semaphore(inflight)
    async with ayafileio.open(path, "rb") as f:
        async def one(off: int) -> int:
            async with sem:
                return len(await f.read_at(off, CHUNK))
        t0 = time.perf_counter()
        results = await asyncio.gather(*(one(o) for o in offsets))
    assert all(r == CHUNK for r in results)
    return time.perf_counter() - t0


async def aya_read_many(path: str, offsets: list[int], inflight: int) -> float:
    # submit in batches of `inflight` spans
    t0 = time.perf_counter()
    async with ayafileio.open(path, "rb") as f:
        for i in range(0, len(offsets), inflight):
            batch = offsets[i : i + inflight]
            results = await f.read_many([(o, CHUNK) for o in batch])
            assert all(len(r) == CHUNK for r in results)
    return time.perf_counter() - t0


async def tp_pread(path: str, offsets: list[int], inflight: int) -> float:
    # Windows has no os.pread: per-thread handle + seek/read (the realistic
    # threadpool pattern). Handles cached by thread, opened unbuffered.
    import threading

    local = threading.local()

    def pread(off: int) -> bytes:
        f = getattr(local, "f", None)
        if f is None:
            f = local.f = os.open(path, os.O_RDONLY | os.O_BINARY)
        os.lseek(f, off, os.SEEK_SET)
        return os.read(f, CHUNK)

    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=min(inflight, 64)) as pool:
        t0 = time.perf_counter()
        results = await asyncio.gather(
            *(loop.run_in_executor(pool, pread, o) for o in offsets)
        )
    assert all(len(r) == CHUNK for r in results)
    return time.perf_counter() - t0


async def seq_aya(path: str, chunk: int) -> float:
    size = os.path.getsize(path)
    async with ayafileio.open(path, "rb") as f:
        t0 = time.perf_counter()
        off = 0
        while off < size:
            data = await f.read_at(off, chunk)
            if not data:
                break
            off += len(data)
    return time.perf_counter() - t0


async def seq_aiofiles(path: str, chunk: int) -> float:
    async with aiofiles.open(path, "rb") as f:
        t0 = time.perf_counter()
        while await f.read(chunk):
            pass
    return time.perf_counter() - t0


async def main() -> None:
    print(f"Python {sys.version.split()[0]}, backend: {ayafileio.get_backend_info()['backend']}")
    print(f"CPUs: {os.cpu_count()}")

    tmpdir = tempfile.mkdtemp(prefix="aya_conc_")
    path = os.path.join(tmpdir, "data.bin")
    try:
        print(f"Preparing {FILE_MB}MB file...", end=" ", flush=True)
        t0 = time.perf_counter()
        with open(path, "wb") as f:
            block = os.urandom(1024 * 1024)
            for _ in range(FILE_MB):
                f.write(block)
        print(f"{time.perf_counter() - t0:.1f}s")

        max_off = os.path.getsize(path) - CHUNK
        offsets = make_offsets(TOTAL_OPS, max_off)

        print(f"\n=== A) Random 4KB pread, {TOTAL_OPS:,} ops/level ===")
        print(f"{'inflight':>8}  {'aya read_at':>12}  {'aya read_many':>14}  {'tp pread(<=64t)':>16}")
        for n in LEVELS:
            gc.collect()
            t1 = await aya_read_at(path, offsets, n)
            t2 = await aya_read_many(path, offsets, n)
            t3 = await tp_pread(path, offsets, n)
            print(
                f"{n:>8}  {TOTAL_OPS / t1:>10,.0f}/s  {TOTAL_OPS / t2:>12,.0f}/s  {TOTAL_OPS / t3:>14,.0f}/s"
            )

        print("\n=== B) Sequential read, whole file, chunk scaling (MB/s) ===")
        size_mb = os.path.getsize(path) / 1024 / 1024
        print(f"{'chunk':>8}  {'aya read_at':>12}  {'aiofiles':>12}  {'aya/aio':>8}")
        for chunk in (64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024):
            gc.collect()
            ta = await seq_aya(path, chunk)
            tb = await seq_aiofiles(path, chunk)
            print(
                f"{chunk // 1024:>6}KB  {size_mb / ta:>10,.0f}/s  {size_mb / tb:>10,.0f}/s  {tb / ta:>7.2f}x"
            )
    finally:
        try:
            os.remove(path)
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    asyncio.run(main())
