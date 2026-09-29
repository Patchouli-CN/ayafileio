<p align="center">
  <img src="assets/icon.png" alt="ayafileio icon" width="200">
</p>

# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)]()
[![PyPI](https://img.shields.io/pypi/v/ayafileio.svg)](https://pypi.org/project/ayafileio/)

**English** | [简体中文](https://github.com/Patchouli-CN/ayafileio/blob/main/README_CN.md)

> "The fastest file I/O in Gensokyo, swift as the Wind God Maiden."
> — Aya Shameimaru

Async file I/O on real kernel backends: IOCP on Windows, io_uring on Linux
(5.1+), Dispatch I/O on macOS. No thread pools pretending to be async;
`ayafileio.open()` picks the backend for you.

## Changelog

See [CHANGES.md](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/CHANGES.md).
Using ayafileio in an interesting way? See [STORY.md](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/STORY.md).

## Backends

| Platform | Backend | True async | Notes |
|----------|---------|:----------:|-------|
| Windows | IOCP | Yes | NT kernel I/O Completion Ports |
| Linux | io_uring | Yes | Kernel 5.1+ |
| macOS | Dispatch I/O | Yes | GCD kernel-level async I/O |

## Features

- Zero thread overhead on true-async platforms — no background threads
- Kernel-level completion: IOCP / io_uring / Dispatch I/O
- Cache-hit inline fast paths: hot small reads complete on the calling thread (IOCP sync-completion / io_uring COOP_TASKRUN / macOS mincore + pread)
- Thousands of concurrent operations on a single file handle
- aiofiles-compatible API, plain `async/await`
- Text and binary modes with automatic encoding/decoding
- Async whole-file copy: `acopy()` with OS-level fast paths (zero-copy on Linux)
- Runtime-tunable configuration shared by all backends
- Python 3.10–3.15, including 3.14t/3.15t free-threading

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

### Copying files

```python
n = await ayafileio.acopy("model.gguf", "backup/model.gguf")
```

`acopy(src, dst, *, chunk_size=4 MiB, concurrency=8, copy_stat=False)` copies a
whole file and returns the number of bytes copied. On Linux it uses the
in-kernel zero-copy `copy_file_range`; on Windows `CopyFile2`, which also
preserves metadata. Both run in a worker thread. Where no fast call exists (or
the filesystem rejects it), it falls back to a `read_at`/`write_at` pipeline
with up to `concurrency` chunks in flight, so memory stays below
`concurrency × chunk_size`. The destination is truncated first; copying a file
onto itself raises `shutil.SameFileError`; `copy_stat=True` applies
`shutil.copystat` afterwards.

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
an `AsyncFile`. Binary mode only.

### Pool management

```python
def drain_handle_pool() -> None: ...           # release all cached file handles
def drain_buffer_pool() -> None: ...           # release all cached I/O buffers
```

Useful after bulk tempfile operations or between benchmark rounds.

## Benchmarks

Every push runs `tests/t_compare.py` on all three platforms on CI and attaches
the JSON results as artifacts. Numbers below are from a recent run
(GitHub Actions, Python 3.14) unless marked *local*. Reproduce with
`python tests/t_compare.py`.

### Random 4 KiB read (Linux, io_uring)

| Concurrency | ayafileio | aiofiles | aya/aio |
|------------:|----------:|---------:|--------:|
| 1 | 191.5K ops/s | 9.8K ops/s | 19.6x |
| 16 | 361.6K ops/s | 10.2K ops/s | 35.4x |
| 64 | 361.2K ops/s | 12.5K ops/s | 28.9x |
| 256 | 349.9K ops/s | 12.3K ops/s | 28.5x |

Positioned `read_at` on a single shared handle holds ~345K ops/s from x16 up;
aiofiles has no positioned-read API to compare against. Small 4 KiB sequential
reads: 201.6K vs 21.9K ops/s (9.2x). Sequential read throughput: 7,360 MB/s at
64 KiB blocks (6.6x), 20,178 MB/s at 4 MiB (1.3x).

Sequential writes are closer: 1.2–1.3x up to 256 KiB blocks, and aiofiles
pulls ahead at 4 MiB (0.4x) on the CI runner — large buffered writes ride the
kernel's writeback path and swing between runs. If that workload matters to
you, check a recent run's artifact rather than any single number.

### Windows and macOS

GitHub's Windows runners throttle small I/O too hard for the CI numbers to be
representative, so the Windows figures are *local* NVMe measurements:

- Windows (IOCP), sequential write: 1,363 → 2,858 MB/s at 1 MiB blocks,
  929 → 3,046 MB/s at 4 MiB (1.47x over aiofiles). 4 KiB random `read_at`
  with 16 in flight: 21.4K ops/s.
- macOS (Dispatch I/O): cache-hit small reads now complete inline on the
  calling thread (mincore + pread) — 4 KiB sequential read 6.8K → 416K ops/s
  (24.3x over aiofiles), 4 KiB random read at x1 4.6K → 289K ops/s (37.7x),
  64 KiB sequential read 384 → 7,474 MB/s (7.3x); the write path is untouched
  (1 MiB sequential write 5.4 GB/s). Measured on the macos-15 CI runner
  (Python 3.14, `tests/t_compare.py` methodology).

### Whole-file copy (`acopy`, *local*, 512 MiB cache-hot file)

| Tool | Time | Throughput |
|------|-----:|-----------:|
| `robocopy` | 0.150 s | 3,413 MiB/s |
| `acopy()` (CopyFile2 fast path) | 0.155 s | 3,300 MiB/s |
| `shutil.copyfile` | 0.217 s | 2,358 MiB/s |
| `acopy()` forced pipeline | 0.330 s | 1,554 MiB/s |

### Concurrent logging writes (`tests/test_loguru.py`)

10K records × 128 B written concurrently to 5 files: 3.5–5.2x over
sync-writes-in-a-threadpool across runs.

### Tuning

The defaults are where the tuning ended up. On very fast NVMe drives,
`iocp_batch_size` 128–256 with `buffer_size` 128 KiB can squeeze out a bit
more on Windows.

## Contributing

Contributions welcome: fork, branch, add tests, make sure the benchmarks still
pass, open a PR.

## License

MIT — see [LICENSE](LICENSE).

---

**"Slow is a crime, right?"**
*— Aya Shameimaru, editor-in-chief of Bunbunmaru News*
