"""acopy vs shutil.copyfile vs robocopy —— 临时基准, 不进 git"""
import asyncio
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")
import ayafileio

SIZE = 512 * 1024 * 1024  # 512 MiB
TMP = Path(__file__).parent / "acopy_bench"
TMP.mkdir(exist_ok=True)
SRC = TMP / "src.bin"


def ensure_src():
    if SRC.exists() and SRC.stat().st_size == SIZE:
        return
    rng = random.Random(42)
    with open(SRC, "wb") as f:
        for _ in range(SIZE // (4 * 1024 * 1024)):
            f.write(rng.randbytes(4 * 1024 * 1024))


def clean(dst: Path):
    dst.unlink(missing_ok=True)


def bench_shutil():
    dst = TMP / "dst_shutil.bin"
    clean(dst)
    t = time.perf_counter()
    shutil.copyfile(SRC, dst)
    dt = time.perf_counter() - t
    ok = dst.stat().st_size == SIZE
    clean(dst)
    return dt, ok


def bench_acopy(chunk_mb=4, conc=8, pipeline_only=False):
    dst = TMP / "dst_acopy.bin"
    clean(dst)

    async def run():
        if pipeline_only:
            from ayafileio import _copy
            saved = _copy._os_copy
            _copy._os_copy = None
            try:
                return await ayafileio.acopy(
                    SRC, dst, chunk_size=chunk_mb * 1024 * 1024, concurrency=conc
                )
            finally:
                _copy._os_copy = saved
        return await ayafileio.acopy(
            SRC, dst, chunk_size=chunk_mb * 1024 * 1024, concurrency=conc
        )

    t = time.perf_counter()
    n = asyncio.run(run())
    dt = time.perf_counter() - t
    ok = n == SIZE and dst.stat().st_size == SIZE
    clean(dst)
    return dt, ok


def bench_robocopy():
    outdir = TMP / "robo_out"
    outdir.mkdir(exist_ok=True)
    dst = outdir / SRC.name
    clean(dst)
    t = time.perf_counter()
    # robocopy 单文件: 源目录 目标目录 文件名 /NFL /NDL /NJH /NJS /NC /NS /NP
    r = subprocess.run(
        ["robocopy", str(TMP), str(outdir), SRC.name, "/NFL", "/NDL", "/NJH", "/NJS", "/NC", "/NS", "/NP"],
        capture_output=True,
    )
    dt = time.perf_counter() - t
    ok = r.returncode < 8 and dst.stat().st_size == SIZE
    clean(dst)
    return dt, ok


def main():
    ensure_src()
    print(f"file: {SIZE / 1024 / 1024:.0f} MiB, backend: {ayafileio.get_backend_info()['backend']}")
    # 预热 page cache
    with open(SRC, "rb") as f:
        while f.read(64 * 1024 * 1024):
            pass

    for name, fn in [
        ("shutil.copyfile      ", bench_shutil),
        ("acopy (OS 快车道)    ", bench_acopy),
        ("acopy pipeline 1x16  ", lambda: bench_acopy(1, 16, True)),
        ("acopy pipeline 4x8   ", lambda: bench_acopy(4, 8, True)),
        ("acopy pipeline 16x4  ", lambda: bench_acopy(16, 4, True)),
        ("robocopy (单文件)     ", bench_robocopy),
    ]:
        dts = []
        for _ in range(3):
            dt, ok = fn()
            assert ok, name
            dts.append(dt)
        best = min(dts)
        print(f"{name} best {best:.3f}s  {SIZE / best / 1024 / 1024:7.0f} MiB/s  (runs: {', '.join(f'{d:.2f}' for d in dts)})")


if __name__ == "__main__":
    main()
