# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)]()
[![Version](https://img.shields.io/badge/version-1.6.0-red.svg)]()

[English](README.md) | **简体中文**

> **「幻想郷最速のファイルI/O、風神少女の如く」**
> *—— 射命丸文，今日も全力で翔ける*

跨平台异步文件 I/O，用的是真正的内核后端：Windows 上 **IOCP**，Linux 上 **io_uring**（内核 5.1+），macOS 上 **Dispatch I/O (GCD)**。没有拿线程池冒充异步的把戏——一个 `ayafileio.open()`，自动选对后端。

## 更新日志

见 [CHANGES_CN.md](CHANGES_CN.md)。

## 后端

| 平台 | 后端 | 真异步 | 说明 |
|------|------|:------:|------|
| Windows | IOCP | 是 | NT 内核原生 I/O 完成端口 |
| Linux | io_uring | 是 | 内核 5.1+ |
| macOS | Dispatch I/O | 是 | GCD 内核级异步 I/O |

## 特性

- 真异步平台零线程开销——没有后台线程，也不需要 `run_in_executor`
- 内核级完成通知：IOCP / io_uring / Dispatch I/O 直达内核
- 单文件句柄上轻松扛数千并发操作
- 与 aiofiles 兼容的 API，就是普通的 `async/await`
- 文本/二进制模式，自动编解码
- 所有后端共享一套运行时可调的配置
- 支持 Python 3.10–3.14，含 3.14t free-threading

## 安装

```bash
pip install ayafileio
```

要求 Python 3.10+，Windows 7+ / Linux（内核 5.1+ 可启用 io_uring）/ macOS 10.10+。
无其他依赖，预编译 wheel 开箱即用。

## 快速开始

```python
import asyncio
import ayafileio

async def main():
    # 写入文件——像风一样快
    async with ayafileio.open("example.txt", "w") as f:
        await f.write("Hello, async world!\n")

    # 读取并自动解码
    async with ayafileio.open("example.txt", "r", encoding="utf-8") as f:
        content = await f.read()
        print(content)

    # 二进制操作
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read(1024)
        await f.seek(0, 0)

asyncio.run(main())
```

## 打开一次，操作多次

开关文件的开销已经是微秒级，但在循环里反复打开同一个文件，每轮还是要白搭一次协程调度：

```python
# 慢：循环内反复 open/close
for i in range(10000):
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read()

# 快：打开一次，多次操作（约快 6 倍）
async with ayafileio.open("data.bin", "rb") as f:
    for i in range(10000):
        await f.seek(0)
        data = await f.read()
```

## 查看当前后端

```python
import ayafileio

print(ayafileio.get_backend_info())
# Windows: {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True}
# Linux:   {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True}
# macOS:   {'platform': 'macos', 'backend': 'dispatch_io', 'is_truly_async': True}
```

## 配置

所有后端共享一套运行时配置：

```python
import ayafileio

print(ayafileio.get_config())

ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,      # 128 KB
    "close_timeout_ms": 2000,
})

ayafileio.reset_config()  # 恢复默认
```

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `handle_pool_max_per_key` | 64 | 每个文件最大缓存句柄数 (Windows) |
| `handle_pool_max_total` | 2048 | 全局最大缓存句柄数 (Windows) |
| `io_worker_count` | 0 | I/O 工作线程数，0 = 自动 |
| `buffer_pool_max` | 512 | 最大缓存缓冲区数 |
| `buffer_size` | 65536 | 单个缓冲区大小（字节） |
| `close_timeout_ms` | 4000 | 关闭时等待 pending I/O 的超时 (ms) |
| `iocp_batch_size` | 64 | IOCP 批量收割完成事件数 (Windows, 1–256) |
| `io_uring_queue_depth` | 256 | io_uring 队列深度 (Linux) |
| `io_uring_sqpoll` | False | 是否启用 SQPOLL 模式 (Linux) |

## API 参考

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

    # 读取
    async def read(self, size: int = -1) -> T: ...
    async def readline() -> T: ...
    async def readlines(hint: int = -1) -> list[T]: ...
    async def readall() -> T: ...                        # read(-1) 别名
    async def readinto(buf: bytearray | memoryview) -> int: ...  # 零拷贝 [仅二进制]
    async def chunk(chunk_size: int, *, buf: bytearray | memoryview | None = None) -> AsyncGenerator[memoryview, None]: ...  # 流式分块 [仅二进制]
    async def read_at(offset: int, size: int = -1) -> bytes: ...  # 位置读（pread 语义），不动文件位置 [仅二进制]
    async def read_many(spans: Iterable[tuple[int, int]]) -> list[bytes]: ...  # 批量位置读，单事件循环周期提交 [仅二进制]
    async def write_at(offset: int, data: bytes | bytearray | memoryview) -> int: ...
    async def write_many(writes: Iterable[tuple[int, bytes | bytearray | memoryview]]) -> list[int]: ...

    # 写入
    async def write(self, data: str | bytes) -> int: ...
    async def writelines(lines) -> None: ...              # 批量写入

    # 位置
    async def seek(self, offset: int, whence: int = 0) -> int: ...
    async def tell() -> int: ...
    async def truncate(size: int) -> None: ...

    # 控制
    async def flush(self) -> None: ...
    async def close(self) -> None: ...

    # 属性
    @property
    def closed(self) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def mode(self) -> str: ...

    # 状态
    def readable() -> bool: ...
    def writable() -> bool: ...
    def seekable() -> bool: ...
    def fileno() -> int: ...
    def isatty() -> bool: ...

    # 迭代器
    def __aiter__(self) -> AsyncFile[T]: ...
    async def __anext__(self) -> T: ...
```

### 支持的模式

| 模式 | 说明 |
|------|------|
| `"r"`, `"rb"` | 读取（文本/二进制） |
| `"w"`, `"wb"` | 写入（文本/二进制） |
| `"a"`, `"ab"` | 追加（文本/二进制） |
| `"x"`, `"xb"` | 独占创建（文本/二进制） |
| 加上 `"+"` | 读写组合 |

### 位置写入与批量写入

```python
async with ayafileio.open("data.bin", "w+b") as f:
    n = await f.write_at(1024, b"payload")
    counts = await f.write_many([(0, b"header"), (4096, b"block")])
    assert await f.tell() == 0
```

`write_at(offset, data)` 返回实际写入字节数；`write_many(writes)` 接受可迭代的
`(offset, data)`，并按输入顺序返回字节数列表。二者仅支持可写的二进制文件，拒绝追加模式，
不改变逻辑文件位置。支持 `bytes`、`bytearray` 和连续 `memoryview`；底层提交时复制数据。
位置写入会丢弃并回退 `readline()` 的预读缓存，以保证后续读取能看到新内容。
空批次返回 `[]`，空数据返回 `0`；偏移必须非负，偏移加长度不能超过有符号 64 位范围，
每项数据长度最多为 4 GiB − 1 字节。与 `write()` 一样，调用者应检查短写返回值。

批量请求直接聚合原生 Future，省去每项创建 Python Task 的开销；`read_many()` 也采用此优化。
这些批量操作会在报告提交或 I/O 异常前等待已提交请求完成，但不是原子事务，失败时可能已部分写入。
重叠区间的并发写入顺序未定义；需要顺序保证时请逐项 `await write_at(...)`。
取消等待不会撤销已经提交的系统 I/O。

### 配置函数

```python
def configure(options: dict) -> None: ...      # 统一配置
def get_config() -> dict: ...                   # 获取当前配置
def reset_config() -> None: ...                 # 重置为默认值
def get_backend_info() -> dict: ...             # 获取后端信息
```

### 包装已有文件

```python
def wrap_file(fd: int, mode: str = "rb", *, owns_fd: bool = False) -> AsyncFile[bytes]: ...
```

将已有的 `int` 文件描述符或带 `fileno()` 方法的对象包装为 `AsyncFile`，底层自动选择最优平台后端。仅支持二进制模式。

### 池管理

```python
def drain_handle_pool() -> None: ...            # 清空句柄池中的所有缓存句柄
def drain_buffer_pool() -> None: ...            # 清空缓冲区池中的所有缓存缓冲区
```

适用于大量临时文件操作之后，或基准测试轮次之间的清理。

## 基准测试

### Crawlee 风格 Dataset 追加写入（每条记录 open → write → close）

5000 条记录，50 个并发写入者，每条写一行就关闭文件——模拟 Crawlee 的 Dataset 追加模式：

| 平台 | ayafileio | aiofiles | 提速 |
|------|-----------|----------|------|
| Windows (NVMe SSD) | **41,336 条/秒** | 9,658 条/秒 | **4.28x** |
| Linux (NVMe SSD) | **17,688 条/秒** | 11,455 条/秒 | **1.54x** |
| macOS (NVMe SSD) | **29,837 条/秒** | 25,522 条/秒 | **1.17x** |
| Windows (6年旧机械盘) | **20,251 条/秒** | 13,011 条/秒 | **1.56x** |

Windows NVMe 那轮，ayafileio 的 P99 延迟是 0.044ms，aiofiles 是 1.854ms——差 42 倍；
负载下抖动 16.2% 对 96.7%。就算换上快报废的机械盘，数字依然稳。

*测试环境：Windows 10/11, Ubuntu 22.04, macOS 14；GitHub Actions NVMe SSD。*

### 单文件高并发随机读

100,000 个并发任务共享同一个文件句柄，随机读 256 字节——没有开关文件开销，纯粹比拼 I/O 路径：

| 库 | 1K 并发 | 10K 并发 | 50K 并发 | 100K 并发 |
|---|---------|----------|----------|-----------|
| **ayafileio (IOCP)** | 7,487 ops/s | **46,616 ops/s** | **28,165 ops/s** | **19,290 ops/s** |
| aiofiles (线程池) | 7,706 ops/s | 7,320 ops/s | 2,131 ops/s | 2,130 ops/s |
| 同步线程池 | 9,492 ops/s | 9,469 ops/s | 8,840 ops/s | 8,660 ops/s |
| **ayafileio vs aiofiles** | 1.0x | **6.4x** | **13.2x** | **9.1x** |

1K 并发时三家差不多——IOCP 的初始化开销还没摊完。过了 10K，aiofiles 的线程池开始饱和，
吞吐量**随并发增加不升反降**（7,706 → 2,130 ops/s，掉了 72%）；IOCP 反而越跑越快，
靠的是 `GetQueuedCompletionStatusEx` 批量收割完成事件。同步线程池则无论并发多少都趴在
~8,800 ops/s——那就是线程竞争的物理天花板。

*测试环境：Windows 10, Python 3.14.5, 西数 1TB 7200RPM 机械硬盘, 20MB 文件, 256B 随机读。*

### 压测：50 万并发读

50 万个 asyncio 任务同时读同一个文件，走 IOCP：

| 指标 | 数值 |
|------|------|
| 并发任务数 | 500,000 |
| 总耗时 | 21.6 秒 |
| 吞吐量 | 23,116 ops/s |
| 峰值内存 (RSS) | ~583 MB |
| 错误 / 异常 | 0 |

双 IOCP worker 架构（总共 2 个线程）收割完全部 50 万个完成事件，零错误。
同样的负载 aiofiles 得开几千个线程——而且还会更慢。

### 关于调优

我们在机械盘上用 14 种配置组合（`iocp_batch_size`、`buffer_size`、`buffer_pool_max`、
`io_worker_count`）跑了 100K 并发。结果每一种都落在默认值的 ±3% 以内——自动调好的默认值
已经顶到了磁盘的物理 I/O 上限，没有留给手工调的空间。

NVMe 盘（>500K IOPS）上，把 `iocp_batch_size` 提到 128–256、`buffer_size` 提到 128 KB，
可能还能再榨一点：

```python
ayafileio.configure({
    "iocp_batch_size": 128,
    "buffer_size": 131072,
})
```

## 贡献

欢迎投稿「文文。新闻」：fork、开分支、加测试、确认基准测试仍然通过，然后发 PR。

## 许可证

MIT —— 最速最自由，详见 [LICENSE](LICENSE)。

---

**「遅いのは罪だぜ？」**
*—— 射命丸文，『文文。新闻』主编*
