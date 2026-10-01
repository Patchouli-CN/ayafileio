# ayafileio Usage Guide

[简体中文](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/USAGE_CN.md)

This guide covers day-to-day use of ayafileio: opening files, the read/write
APIs, positioned I/O, batch operations, configuration, and migration notes.
For benchmarks and the backend architecture, see the
[README](https://github.com/Patchouli-CN/ayafileio/blob/main/README.md).

## Contents

- [Installation](#installation)
- [Quick start](#quick-start)
- [Two ways to open a file: `open()` vs `aopen()`](#two-ways-to-open-a-file-open-vs-aopen)
- [Reading and writing](#reading-and-writing)
- [Positioned I/O: `read_at` / `write_at` / `read_many` / `write_many`](#positioned-io)
- [Whole-file helpers](#whole-file-helpers)
- [Batch reads: `read_bytes_many` / `read_text_many`](#batch-reads)
- [Streaming with `chunk()`](#streaming-with-chunk)
- [Zero-copy reads with `readinto()`](#zero-copy-reads-with-readinto)
- [Copying files: `acopy()`](#copying-files-acopy)
- [Wrapping an existing file descriptor](#wrapping-an-existing-file-descriptor)
- [Configuration](#configuration)
- [Checking the active backend](#checking-the-active-backend)
- [Pool management](#pool-management)
- [Performance best practices](#performance-best-practices)
- [Migrating from aiofiles](#migrating-from-aiofiles)
- [Type checking](#type-checking)

## Installation

```bash
pip install ayafileio
```

Python 3.10+ (including 3.14t/3.15t free-threading), Windows 7+ /
Linux kernel 5.1+ / macOS 10.10+. No dependencies; precompiled wheels
are provided for all three platforms.

## Quick start

```python
import asyncio
import ayafileio

async def main():
    async with ayafileio.open("notes.txt", "w", encoding="utf-8") as f:
        await f.write("hello\n")

    async with ayafileio.open("notes.txt", "r", encoding="utf-8") as f:
        print(await f.read())

asyncio.run(main())
```

Everything is a plain coroutine — if you know `async`/`await`, you know the
API. `AsyncFile` supports iteration (`async for line in f`), `readline()`,
`readlines()`, `seek()`/`tell()`, `truncate()`, `flush()`, and the usual
`closed` / `name` / `mode` properties.

Supported modes mirror the builtin `open()`: `r` / `w` / `a` / `x`, each with
a binary `b` variant and `+` read-write combinations.

## Two ways to open a file: `open()` vs `aopen()`

### `ayafileio.open()` — the default

```python
async with ayafileio.open("data.bin", "rb") as f:
    data = await f.read()
```

Synchronous construction: the OS open happens on the calling thread
(microseconds on a warm SSD). Errors such as `FileNotFoundError` are raised
immediately at the call site. Use it for the common "open one file, do I/O"
case — it has the lowest latency and the simplest failure semantics.

### `ayafileio.aopen()` — parallel opens

```python
async with ayafileio.aopen("data.bin", "rb") as f:
    data = await f.read()
```

Same arguments, same returned `AsyncFile` — but the OS open itself runs on the
C++ thread pool, off the event loop. The handle is lazy (aiofiles style), so
both of these work:

```python
async with ayafileio.aopen(path, "rb") as f:   # open happens in __aenter__
    ...

f = await ayafileio.aopen(path, "rb")          # explicit await also works
```

Reach for `aopen()` when the open syscall itself is the bottleneck:

- opening **many files at once** — opens genuinely run in parallel
- slow storage (spinning disks, network filesystems) where one open can take
  milliseconds and would stall the loop

For a single file on local storage, `open()` is cheaper: `aopen()` pays one
thread-pool dispatch and one event-loop wakeup per call. The lazy handle is
single-use — don't re-enter the same handle for a second `async with` block.

## Reading and writing

```python
async with ayafileio.open("data.bin", "rb") as f:
    head = await f.read(1024)        # first 1024 bytes
    await f.seek(0)
    everything = await f.read()      # -1 = to EOF

async with ayafileio.open("log.txt", "a", encoding="utf-8") as f:
    await f.write("one line\n")
    await f.writelines(["a\n", "b\n"])
```

Binary reads return `bytes`, text reads return `str` decoded with the given
`encoding` (or the platform locale default, like builtin `open()`). Text mode
supports `newline` translation with the same semantics as the builtin.

One deliberate difference from the builtin: in text mode, `write()` returns
the number of **bytes** written after encoding, not the number of characters.

`ayafileio` is unbuffered: every `read()`/`write()` is one I/O request. That
is what makes thousands of concurrent operations on one handle cheap, but it
also means byte-at-a-time loops are a bad idea — read in chunks.

## Positioned I/O

`read_at` / `write_at` are pread/pwrite semantics: they operate on an absolute
offset **without touching the logical file position**, so they never race with
concurrent `read()`/`seek()` on the same handle:

```python
async with ayafileio.open("model.safetensors", "rb") as f:
    header, layer0, layer1 = await asyncio.gather(
        f.read_at(0, 1024),
        f.read_at(1024, 1 << 20),
        f.read_at((1 << 20) + 1024, 1 << 20),
    )
```

`read_many` / `write_many` submit a whole batch in one event-loop turn,
gathering the native futures directly instead of creating a Python Task per
item:

```python
chunks = await f.read_many([(0, 4096), (8192, 4096), (16384, 4096)])
counts = await f.write_many([(0, b"header"), (4096, b"block")])
```

Rules of the road (binary mode only, `write_at`/`write_many` reject append
mode): batches are **not atomic** — an error is raised only after all
submitted requests drain, and earlier writes may already be on disk.
Overlapping ranges have unspecified ordering; await them separately when
order matters. Offsets must be nonnegative and fit signed 64-bit together
with the length. Check for short writes as you would with `write()`.

This is the API to reach for when your data layout is known up front: model
shards, database pages, index regions.

## Whole-file helpers

```python
data = await ayafileio.read_bytes("model.safetensors")
n    = await ayafileio.write_bytes("out.bin", payload)
text = await ayafileio.read_text("config.yaml", encoding="utf-8")
n    = await ayafileio.write_text("out.txt", "hello", encoding="utf-8")
```

One await does open + read/write + close. Semantics are identical to the
hand-written `async with ayafileio.open(...)` form — it just removes the
boilerplate. (`write_text` also returns the encoded byte count.)

## Batch reads

The single most common async-file mistake is the sequential loop:

```python
# Slow no matter how fast each read is — one file in flight at a time
for path in paths:
    async with ayafileio.open(path, "rb") as f:
        data = await f.read()
```

The one-line fix:

```python
shards  = await ayafileio.read_bytes_many(model_shard_paths)
configs = await ayafileio.read_text_many(config_paths, encoding="utf-8")
```

One call reads many files concurrently: opens go through `aopen()` (parallel
OS opens), reads go through the platform async backend, and results come back
**in input order**. `max_concurrency` (keyword-only, default 64) caps
in-flight files, so a hundred-thousand-path input can't exhaust file
descriptors. Any failure raises the whole call, same as `asyncio.gather`.

Measured on 300 × 4 KiB cache-hot files: 5.6x over the sequential aiofiles
loop, 2.6x over the sequential ayafileio loop.

## Streaming with `chunk()`

For large files you don't want in memory all at once:

```python
async with ayafileio.open("big.bin", "rb") as f:
    async for block in f.chunk(1 << 20):     # 1 MiB blocks
        process(block)                        # block is a memoryview
```

`chunk(chunk_size, *, buf=None)` yields memoryviews; pass your own
`bytearray`/`memoryview` as `buf` to reuse one buffer across iterations
(binary mode only).

## Zero-copy reads with `readinto()`

```python
buf = bytearray(65536)
async with ayafileio.open("data.bin", "rb") as f:
    n = await f.readinto(buf)      # data lands directly in buf
```

The backend reads straight into your buffer — no intermediate copy. Binary
mode only.

## Copying files: `acopy()`

```python
n = await ayafileio.acopy("model.gguf", "backup/model.gguf")
```

`acopy(src, dst, *, chunk_size=4 MiB, concurrency=8, copy_stat=False)` copies
a whole file and returns the byte count. It uses the fastest path the OS
offers — `copy_file_range` (in-kernel zero-copy) on Linux, `CopyFile2` on
Windows (also preserves metadata) — and falls back to a `read_at`/`write_at`
pipeline with up to `concurrency` chunks in flight elsewhere, so memory stays
below `concurrency × chunk_size`. The destination is truncated first; copying
a file onto itself raises `shutil.SameFileError`; `copy_stat=True` applies
`shutil.copystat` afterwards.

## Wrapping an existing file descriptor

```python
f = ayafileio.wrap_file(fd, "rb", owns_fd=False)
```

Wraps an `int` fd (or any object with `fileno()`) as an `AsyncFile`. Binary
mode only. With `owns_fd=True`, closing the `AsyncFile` closes the fd.

## Configuration

One runtime configuration is shared by all backends:

```python
ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,
    "close_timeout_ms": 2000,
})

print(ayafileio.get_config())
ayafileio.reset_config()
```

| Option | Default | Description |
|--------|---------|-------------|
| `handle_pool_max_per_key` | 64 | Max cached handles per file (Windows) |
| `handle_pool_max_total` | 2048 | Max total cached handles (Windows) |
| `io_worker_count` | 0 | I/O worker threads, 0 = auto |
| `buffer_pool_max` | 512 | Max cached I/O buffers |
| `buffer_size` | 65536 | Buffer size in bytes |
| `close_timeout_ms` | 4000 | Close timeout for pending I/O (ms) |
| `iocp_batch_size` | 64 | IOCP completion harvest batch (Windows, 1–256) |
| `io_uring_queue_depth` | 256 | io_uring queue depth (Linux) |
| `io_uring_sqpoll` | False | Enable SQPOLL mode (Linux) |

The defaults are where tuning ended up. On very fast NVMe drives,
`iocp_batch_size` 128–256 with `buffer_size` 128 KiB can squeeze a bit more
out of Windows.

## Checking the active backend

```python
print(ayafileio.get_backend_info())
# {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True, ...}
```

Backend selection is automatic: IOCP on Windows, io_uring on Linux 5.1+,
Dispatch I/O on macOS. Where the native backend is unavailable (old kernel,
restricted container), ayafileio falls back to a thread-pool backend and
`is_truly_async` reads `False` — the API is identical either way.

## Pool management

```python
ayafileio.drain_handle_pool()   # release all cached file handles
ayafileio.drain_buffer_pool()   # release all cached I/O buffers
```

Useful after bulk tempfile operations or between benchmark rounds.

## Performance best practices

1. **Open once, operate many times.** Reopening in a loop costs a coroutine
   round-trip per iteration even when the handle itself is pooled:

   ```python
   async with ayafileio.open("data.bin", "rb") as f:
       for _ in range(10000):
           await f.seek(0)
           data = await f.read()
   ```

2. **Many files → batch APIs, never a sequential `for` loop.**
   `read_bytes_many()` is the shortest *and* the fastest form.

3. **Known offsets → positioned I/O.** `read_at`/`read_many` skip the
   seek/read dance and never disturb other operations on the handle.

4. **Big files → `chunk()` or `readinto()`**, not one giant `read()`.

5. **Lots of tiny writes → batch them yourself** (`write_many` for positioned
   writes, or accumulate and write once) — each call is one I/O request.

## Migrating from aiofiles

The common surface is a drop-in replacement:

| aiofiles | ayafileio |
|----------|-----------|
| `aiofiles.open(...)` | `ayafileio.open(...)` |
| `async with aiofiles.open(...)` | `async with ayafileio.open(...)` |

Differences worth knowing:

- **True kernel async.** No thread pool behind the scenes on any of the three
  platforms (aiofiles runs everything through `asyncio.to_thread`-style
  executors).
- **Unbuffered.** aiofiles opens buffered by default; ayafileio does not
  buffer, which is why tiny sequential reads look different between the two.
  Read in chunks.
- **Text-mode `write()` returns encoded bytes**, not characters.
- **Extra APIs aiofiles doesn't have:** `read_at`/`write_at`/`read_many`/
  `write_many`, `aopen()`, `read_bytes_many()`/`read_text_many()`, `acopy()`,
  `readinto()`, `chunk()`.
- `aiofiles.os` (async `stat`, `listdir`, …) is **not** covered — ayafileio
  is file I/O only.

## Type checking

ayafileio ships a `py.typed` marker, so mypy/pyright pick up the annotations
out of the box. `open()` and `aopen()` are overloaded on the mode string:

```python
async with ayafileio.open("a.txt", "r") as f:
    reveal_type(f)   # AsyncFile[str]
async with ayafileio.open("b.bin", "rb") as f:
    reveal_type(f)   # AsyncFile[bytes]
```

`AsyncFile[str]` reads give `str`, `AsyncFile[bytes]` reads give `bytes` —
wrong-mode `write()` calls are caught by the type checker before they hit
the runtime `TypeError`.
