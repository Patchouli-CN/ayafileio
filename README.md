# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)]()
[![Version](https://img.shields.io/badge/version-1.6.0-red.svg)]()

**English** | [简体中文](README_CN.md)

> "The fastest file I/O in Gensokyo, swift as the Wind God Maiden."
> — Aya Shameimaru, always flying at full speed

Async file I/O with real kernel backends: **IOCP** on Windows, **io_uring** on Linux (kernel 5.1+), **Dispatch I/O (GCD)** on macOS. No thread pools pretending to be async — one `ayafileio.open()`, and the right backend is picked for you.

## Changelog

See [CHANGES.md](CHANGES.md).

## Backends

| Platform | Backend | True async | Notes |
|----------|---------|:----------:|-------|
| Windows | IOCP | Yes | NT kernel I/O Completion Ports |
| Linux | io_uring | Yes | Kernel 5.1+ |
| macOS | Dispatch I/O | Yes | GCD kernel-level async I/O |

## Features

- Zero thread overhead on true-async platforms — no background threads
- Kernel-level completion: IOCP / io_uring / Dispatch I/O, direct to the kernel
- Thousands of concurrent operations on a single file handle
- aiofiles-compatible API, plain `async/await`
- Text and binary modes with automatic encoding/decoding
- Runtime-tunable configuration shared by all backends
- Python 3.10–3.14, including 3.14t free-threading

## Installation

```bash
pip install ayafileio
```

Requires Python 3.10+, Windows 7+ / Linux (kernel 5.1+ for io_uring) / macOS 10.10+.
No external dependencies; precompiled wheels are available.

## Quick start

```python
import asyncio
import ayafileio

async def main():
    # Write to a file — fast as the wind
    async with ayafileio.open("example.txt", "w") as f:
        await f.write("Hello, async world!\n")

    # Read with automatic decoding
    async with ayafileio.open("example.txt", "r", encoding="utf-8") as f:
        content = await f.read()
        print(content)

    # Binary operations
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read(1024)
        await f.seek(0, 0)

asyncio.run(main())
```

## Open once, read many times

Open/close overhead is already in the microsecond range, but reopening the same
file in a loop still costs you a coroutine round-trip per iteration:

```python
# Slow: repeated open/close
for i in range(10000):
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read()

# Fast: one handle, many operations (~6x faster)
async with ayafileio.open("data.bin", "rb") as f:
    for i in range(10000):
        await f.seek(0)
        data = await f.read()
```

## Checking the active backend

```python
import ayafileio

print(ayafileio.get_backend_info())
# Windows: {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True}
# Linux:   {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True}
# macOS:   {'platform': 'macos', 'backend': 'dispatch_io', 'is_truly_async': True}
```

## Configuration

All backends share one runtime configuration:

```python
import ayafileio

print(ayafileio.get_config())

ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,      # 128 KB
    "close_timeout_ms": 2000,
})

ayafileio.reset_config()  # back to defaults
```

| Option | Default | Description |
|--------|---------|-------------|
| `handle_pool_max_per_key` | 64 | Max cached handles per file (Windows) |
| `handle_pool_max_total` | 2048 | Max total cached handles (Windows) |
| `io_worker_count` | 0 | IO worker threads, 0 = auto |
| `buffer_pool_max` | 512 | Max cached buffers |
| `buffer_size` | 65536 | Buffer size in bytes |
| `close_timeout_ms` | 4000 | Close timeout for pending I/O (ms) |
| `iocp_batch_size` | 64 | IOCP batch completion harvest size (Windows, 1–256) |
| `io_uring_queue_depth` | 256 | io_uring queue depth (Linux) |
| `io_uring_sqpoll` | False | Enable SQPOLL mode (Linux) |

## API reference

### AsyncFile

```python
class AsyncFile(Generic[T]):
    def __init__(
        self, path: str | Path, mode: str = "rb",
        encoding: str | None = None,
        newline: str | None = None,
        errors: str | None = None,
        auto_flush: bool = False
    ): ...

    # Reading
    async def read(self, size: int = -1) -> T: ...
    async def readline() -> T: ...
    async def readlines(hint: int = -1) -> list[T]: ...
    async def readall() -> T: ...                        # alias for read(-1)
    async def readinto(buf: bytearray | memoryview) -> int: ...  # zero-copy [binary only]
    async def chunk(chunk_size: int, *, buf: bytearray | memoryview | None = None) -> AsyncGenerator[memoryview, None]: ...  # streaming chunks [binary only]
    async def read_at(offset: int, size: int = -1) -> bytes: ...  # positioned read (pread), leaves file position untouched [binary only]
    async def read_many(spans: Iterable[tuple[int, int]]) -> list[bytes]: ...  # batched positioned reads, submitted in one event-loop turn [binary only]
    async def write_at(offset: int, data: bytes | bytearray | memoryview) -> int: ...
    async def write_many(writes: Iterable[tuple[int, bytes | bytearray | memoryview]]) -> list[int]: ...

    # Writing
    async def write(self, data: str | bytes) -> int: ...
    async def writelines(lines) -> None: ...

    # Position
    async def seek(self, offset: int, whence: int = 0) -> int: ...
    async def tell() -> int: ...
    async def truncate(size: int) -> None: ...

    # Control
    async def flush(self) -> None: ...
    async def close(self) -> None: ...

    # Properties
    @property
    def closed(self) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def mode(self) -> str: ...

    # State
    def readable() -> bool: ...
    def writable() -> bool: ...
    def seekable() -> bool: ...
    def fileno() -> int: ...
    def isatty() -> bool: ...

    # Iteration
    def __aiter__(self) -> AsyncFile[T]: ...
    async def __anext__(self) -> T: ...
```

### Supported modes

| Mode | Description |
|------|-------------|
| `"r"`, `"rb"` | Read (text/binary) |
| `"w"`, `"wb"` | Write (text/binary) |
| `"a"`, `"ab"` | Append (text/binary) |
| `"x"`, `"xb"` | Exclusive create (text/binary) |
| `+` added | Read/write combinations |

### Positioned and batched writes

```python
async with ayafileio.open("data.bin", "w+b") as f:
    n = await f.write_at(1024, b"payload")
    counts = await f.write_many([(0, b"header"), (4096, b"block")])
    assert await f.tell() == 0
```

`write_at(offset, data)` returns the number of bytes written. `write_many(writes)`
accepts an iterable of `(offset, data)` pairs and returns counts in input order.
Both require a writable binary file, reject append mode, and preserve the logical
file position. They accept `bytes`, `bytearray`, and contiguous `memoryview`
buffers, which the backend copies on submission. Positioned writes rewind and
discard any `readline()` read-ahead so subsequent reads see the updated content.
An empty batch returns `[]`; an empty buffer returns `0`. Offsets must be
nonnegative, offset plus length must fit a signed 64-bit integer, and each buffer
is limited to 4 GiB minus 1 byte. As with `write()`, callers must check for short
writes.

Batch operations gather native Futures directly, avoiding a Python Task per item;
`read_many()` uses the same optimization. Submitted requests are drained before
reporting submission or I/O errors, but batches are not atomic: some writes may
have succeeded when an error is raised. Concurrent overlapping writes have
unspecified ordering; await each `write_at(...)` separately when ordering matters.
Cancelling the wait does not undo already submitted system I/O.

### Configuration functions

```python
def configure(options: dict) -> None: ...      # apply unified configuration
def get_config() -> dict: ...                  # current configuration
def reset_config() -> None: ...                # reset to defaults
def get_backend_info() -> dict: ...            # active backend information
```

### Wrapping an existing file

```python
def wrap_file(fd: int, mode: str = "rb", *, owns_fd: bool = False) -> AsyncFile[bytes]: ...
```

Wraps an existing file descriptor (int) or a file-like object with `fileno()` as
an `AsyncFile`, backed by the optimal platform backend. Binary mode only.

### Pool management

```python
def drain_handle_pool() -> None: ...           # release all cached file handles
def drain_buffer_pool() -> None: ...           # release all cached I/O buffers
```

Useful after bulk tempfile operations or between benchmark rounds.

## Benchmarks

### Crawlee-style dataset append (open/write/close per record)

5,000 records, 50 concurrent writers, each writing a single line and closing the
file — simulating Crawlee's Dataset append pattern:

| Platform | ayafileio | aiofiles | Speedup |
|----------|-----------|----------|---------|
| Windows (NVMe SSD) | **41,336 items/s** | 9,658 items/s | **4.28x** |
| Linux (NVMe SSD) | **17,688 items/s** | 11,455 items/s | **1.54x** |
| macOS (NVMe SSD) | **29,837 items/s** | 25,522 items/s | **1.17x** |
| Windows (6yr old HDD) | **20,251 items/s** | 13,011 items/s | **1.56x** |

On the Windows NVMe run, ayafileio's P99 latency is 42x lower (0.044 ms vs
1.854 ms), and jitter under load is 16.2% against aiofiles' 96.7%. Even on the
old HDD the numbers stay predictable.

*Test environment: Windows 10/11, Ubuntu 22.04, macOS 14; GitHub Actions NVMe SSD.*

### Single-file random read at high concurrency

100,000 concurrent tasks doing random 256-byte reads on one shared file handle —
no open/close overhead, pure I/O path:

| Library | 1K concur | 10K concur | 50K concur | 100K concur |
|---------|-----------|------------|------------|-------------|
| **ayafileio (IOCP)** | 7,487 ops/s | **46,616 ops/s** | **28,165 ops/s** | **19,290 ops/s** |
| aiofiles (threadpool) | 7,706 ops/s | 7,320 ops/s | 2,131 ops/s | 2,130 ops/s |
| sync threadpool | 9,492 ops/s | 9,469 ops/s | 8,840 ops/s | 8,660 ops/s |
| **ayafileio vs aiofiles** | 1.0x | **6.4x** | **13.2x** | **9.1x** |

At 1K concurrency everything is about equal — the IOCP setup cost is still being
amortized. Past 10K, aiofiles' thread pool saturates and throughput *drops* with
more concurrency (7,706 → 2,130 ops/s, a 72% loss), while IOCP *gains* throughput
thanks to batched completion harvesting via `GetQueuedCompletionStatusEx`. The
sync threadpool flatlines around 8,800 ops/s no matter what — that's the thread
contention ceiling.

*Test environment: Windows 10, Python 3.14.5, WDC WD10EZEX 7200RPM HDD, 20 MB file, 256 B random reads.*

### Stress: 500,000 concurrent reads

Half a million asyncio tasks all reading one file through IOCP:

| Metric | Value |
|--------|-------|
| Concurrent tasks | 500,000 |
| Completion time | 21.6 s |
| Throughput | 23,116 ops/s |
| Peak memory (RSS) | ~583 MB |
| Errors / exceptions | 0 |

The dual-IOCP worker architecture (2 threads total) drains all 500K completions.
aiofiles would need thousands of threads for the same workload — and would still
be slower.

### A note on tuning

We benchmarked 14 configuration combinations (`iocp_batch_size`, `buffer_size`,
`buffer_pool_max`, `io_worker_count`) on the HDD at 100K concurrency. Every single
one landed within ±3% of the defaults — the auto-tuned defaults already saturate
the disk's physical I/O limit, so there is nothing left to tune by hand.

On NVMe drives with >500K IOPS, raising `iocp_batch_size` to 128–256 and
`buffer_size` to 128 KB may squeeze out a bit more:

```python
ayafileio.configure({
    "iocp_batch_size": 128,
    "buffer_size": 131072,
})
```

## Contributing

Contributions welcome: fork, branch, add tests, make sure the benchmarks still
pass, open a PR.

## License

MIT — see [LICENSE](LICENSE).

---

**"Slow is a crime, right?"**
*— Aya Shameimaru, editor-in-chief of Bunbunmaru News*
