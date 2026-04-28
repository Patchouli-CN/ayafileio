
---

# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Cross--platform-blue.svg)](https://en.wikipedia.org/wiki/Cross-platform)
[![Version](https://img.shields.io/badge/version-1.0.5-red.svg)]()

**当前是英文** | [**chinese version**](README_CN.md)

> **"The fastest file I/O in Gensokyo, swift as the Wind God Maiden."**  
> *— Aya Shameimaru, always flying at full speed*

**Cross-platform asynchronous file I/O library using native async I/O where available.**  
Windows leverages **IOCP** (I/O Completion Ports), Linux uses **io_uring** (kernel 5.1+), and macOS uses **Dispatch I/O (GCD)** for truly non-blocking file operations.

## changes

**see** -> [**CHANGES**](CHANGES.md)

## 🏆 The Only True Async on All Three Major Platforms

| Platform | Backend | True Async | Description |
|----------|---------|------------|-------------|
| **Windows** | IOCP | ✅ | NT kernel native I/O Completion Ports |
| **Linux** | io_uring | ✅ | Next-gen async I/O (kernel 5.1+) |
| **macOS** | Dispatch I/O | ✅ | GCD kernel-level async I/O |

**ayafileio is the only Python library providing true async file I/O on Windows, Linux, and macOS.**

## 📸 Key Features

| Feature | Description |
|---------|-------------|
| 🍃 **Zero thread overhead** | No background threads on true async platforms |
| 📰 **Kernel-level completion** | IOCP / io_uring / Dispatch I/O direct to kernel |
| ⚡ **High concurrency** | Handles thousands of concurrent file operations |
| 🎴 **Familiar API** | aiofiles-compatible, supports `async/await` |
| 📖 **Text & binary support** | Automatic encoding/decoding in text modes |
| 🔧 **Unified configuration** | Runtime tunable parameters for all backends |
| 🌍 **Cross-platform** | Windows, Linux, and macOS |
| 🐍 **Latest Python** | Supports 3.10, 3.11, 3.12, 3.13, 3.14 |

## 🛠️ Installation

```bash
pip install ayafileio
```

**System requirements:**
- Python 3.10+
- Windows 7+ / Linux (kernel 5.1+ for io_uring) / macOS 10.10+
- No external dependencies, precompiled wheels available

## 🚀 Quick Start

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

## ⚡ Performance Best Practice

ayafileio's file open/close overhead is already in the microsecond range, but for maximum performance, **avoid reopening the same file in a loop**.

```python
# ❌ DO NOT DO THIS: repeated open/close in a loop
for i in range(10000):
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read()

# ✅ DO THIS: open once, operate many times
async with ayafileio.open("data.bin", "rb") as f:
    for i in range(10000):
        await f.seek(0)
        data = await f.read()
```

The latter is ~6x faster — it eliminates 9999 unnecessary coroutine scheduling round-trips.

## 🔍 Backend Information

Check which backend is currently in use:

```python
import ayafileio

info = ayafileio.get_backend_info()
print(info)
# Windows: {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True}
# Linux:   {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True}
# macOS:   {'platform': 'macos', 'backend': 'dispatch_io', 'is_truly_async': True}
```

## ⚙️ Unified Configuration

`ayafileio` provides a unified configuration system that allows runtime tuning:

```python
import ayafileio

# View current configuration
config = ayafileio.get_config()
print(config)

# Update configuration
ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,      # 128KB buffer
    "close_timeout_ms": 2000,
})

# Reset to defaults
ayafileio.reset_config()
```

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `handle_pool_max_per_key` | 64 | Max cached handles per file (Windows) |
| `handle_pool_max_total` | 2048 | Max total cached handles (Windows) |
| `io_worker_count` | 0 | IO worker threads, 0=auto |
| `buffer_pool_max` | 512 | Max cached buffers |
| `buffer_size` | 65536 | Buffer size in bytes |
| `close_timeout_ms` | 4000 | Close timeout for pending I/O (ms) |
| `io_uring_queue_depth` | 256 | io_uring queue depth (Linux) |
| `io_uring_sqpoll` | False | Enable SQPOLL mode (Linux) |
| `enable_debug_log` | False | Enable debug logging |

## 📚 API Reference

### AsyncFile class

```python
class AsyncFile:
    def __init__(self, path: str | Path, mode: str = "rb", encoding: str | None = None): ...
    async def read(self, size: int = -1) -> str | bytes: ...
    async def write(self, data: str | bytes) -> int: ...
    async def seek(self, offset: int, whence: int = 0) -> int: ...
    async def flush(self) -> None: ...
    async def close(self) -> None: ...
    async def readline(self) -> str | bytes: ...
    def __aiter__(self) -> AsyncFile: ...
    async def __anext__(self) -> str | bytes: ...
```

### Supported Modes

| Mode | Description |
|------|-------------|
| `"r"`, `"rb"` | Read (text/binary) |
| `"w"`, `"wb"` | Write (text/binary) |
| `"a"`, `"ab"` | Append (text/binary) |
| `"x"`, `"xb"` | Exclusive create (text/binary) |
| `+` added | Read/write combinations |

### Configuration Functions

```python
def configure(options: dict) -> None: ...      # Unified configuration
def get_config() -> dict: ...                   # Get current configuration
def reset_config() -> None: ...                 # Reset to defaults
def get_backend_info() -> dict: ...             # Get backend information
```

## 🧪 Performance Comparison

Simulating Crawlee's Dataset append pattern (5,000 records, 50 concurrent):

| Platform | ayafileio | aiofiles | Speedup |
|----------|-----------|----------|---------|
| **Windows (NVMe SSD)** | **41,336 items/s** | 9,658 items/s | **4.28x** |
| **Linux (NVMe SSD)** | **17,688 items/s** | 11,455 items/s | **1.54x** |
| **macOS (NVMe SSD)** | **29,837 items/s** | 25,522 items/s | **1.17x** |
| **Windows (6yr old HDD)** | **20,251 items/s** | 13,011 items/s | **1.56x** |

**Key findings:**
- On Windows enterprise SSD, ayafileio achieves **42x lower P99 latency** (0.044ms vs 1.854ms)
- aiofiles shows **96.7% jitter** under load; ayafileio only **16.2%**
- Even on degraded hardware, ayafileio maintains predictable performance

> *Test environment: Windows 10/11, Ubuntu 22.04, macOS 14; GitHub Actions enterprise NVMe SSD*

## 🤝 Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Add tests
4. Ensure benchmarks pass
5. Open a pull request

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

---

**"Slow is a crime, right?"**  
*— Aya Shameimaru, editor-in-chief of Bunbunmaru News*

---
