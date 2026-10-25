"""Python overhead only, no disk I/O: python benchmarks/bench_wrapper.py."""
import asyncio
import statistics
import time
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests" / "python"))

from test_async_wrapper import MemoryBackend, wrapper


async def main():
    payload = b"x" * (16 * 1024 * 1024) + b"\n"
    find_line_end = wrapper._find_line_end

    async def lines(baseline):
        f = wrapper.AsyncFile._from_impl(MemoryBackend(payload), "rb")
        if baseline:
            # Emulate the old rescan-from-start behavior.
            wrapper._find_line_end = lambda buf, start, newline, eof: find_line_end(
                buf, f._line_pos, newline, eof)
        start = time.perf_counter()
        try:
            assert await f.readline() == payload
            return time.perf_counter() - start
        finally:
            wrapper._find_line_end = find_line_end

    async def batch(baseline):
        f = wrapper.AsyncFile._from_impl(MemoryBackend(b"x" * 4096), "rb")
        start = time.perf_counter()
        if baseline:
            values = await asyncio.gather(*(f.read_at(i, 1) for i in range(4096)))
        else:
            values = await f.read_many((i, 1) for i in range(4096))
        assert values == [b"x"] * 4096
        return time.perf_counter() - start

    for name, run in [("16 MiB readline", lines), ("4096 completed-Future reads", batch)]:
        before, after = [], []
        for _ in range(5):
            before.append(await run(True))
            after.append(await run(False))
        old, new = statistics.median(before), statistics.median(after)
        print(f"{name}: before={old * 1000:.2f}ms after={new * 1000:.2f}ms "
              f"ratio={old / new:.2f}x (memory backend, not disk throughput)")


if __name__ == "__main__":
    asyncio.run(main())
