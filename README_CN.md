<p align="center">
  <img src="assets/icon.png" alt="ayafileio 图标" width="200">
</p>

# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)]()
[![PyPI](https://img.shields.io/pypi/v/ayafileio.svg)](https://pypi.org/project/ayafileio/)

[English](https://github.com/Patchouli-CN/ayafileio/blob/main/README.md) | **简体中文**

> **「幻想郷最速のファイルI/O、風神少女の如く」**
> *—— 射命丸文*

跨平台异步文件 I/O，用的是真正的内核后端：Windows 上 IOCP，Linux 上 io_uring（内核 5.1+），macOS 上 Dispatch I/O (GCD)。没有拿线程池冒充异步的把戏——`ayafileio.open()` 会自动选对后端。

## 更新日志

见 [CHANGES_CN.md](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/CHANGES_CN.md)。
完整使用指南见 [USAGE_CN.md](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/USAGE_CN.md)。
在用 ayafileio 玩有趣的东西？见 [STORY_CN.md](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/STORY_CN.md)。

## 后端

| 平台 | 后端 | 真异步 | 说明 |
|------|------|:------:|------|
| Windows | IOCP | 是 | NT 内核原生 I/O 完成端口 |
| Linux | io_uring | 是 | 内核 5.1+ |
| macOS | Dispatch I/O | 是 | GCD 内核级异步 I/O |

## 特性

- 真异步平台零线程开销——没有后台线程，也不需要 `run_in_executor`
- 内核级完成通知：IOCP / io_uring / Dispatch I/O
- 缓存命中内联快车道：热路径小读在调用线程就地完成，await 不让出事件循环（IOCP 同步完成 / io_uring COOP_TASKRUN / macOS mincore + pread）
- 单文件句柄抗住数千并发操作
- 与 aiofiles 兼容的 API，与标准 `async/await` 用法一致
- 文本/二进制模式，自动编解码
- 异步整文件复制：`acopy()`，Linux/Windows 走 OS 级快车道
- 所有后端共享一套运行时可调的配置
- 支持 Python 3.10–3.15，含 3.14t / 3.15t free-threading

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

开关文件的开销已经是微秒级，但在循环里反复打开同一个文件，每轮仍要多付一次协程调度开销：

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
# Windows: {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True, 'os_version': '10.0.19045', ...}
# Linux:   {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True, 'os_version': 'Linux 6.5.0-...', ...}
# macOS:   {'platform': 'macos', 'backend': 'dispatch_io', 'is_truly_async': True, 'os_version': 'Darwin 24.5.0', ...}
```

## 能力发现

`get_capabilities()` 返回 `get_backend_info()` 的全部内容，外加两个字典：
`features`（库级能力，键跨平台统一）和 `backend_detail`（当前后端在此机器
上探测到的原生能力）。探测直接问内核——io_uring 的 opcode 支持情况走
`IORING_REGISTER_PROBE` 实测，不查版本号表，backport 内核报的也是真实
能力。结果进程内缓存，重复调用零开销。

```python
caps = ayafileio.get_capabilities()

caps["features"]
# {'positional_io': True, 'batch_positional_io': True, 'zero_copy_readinto': True,
#  'chunked_streaming': True, 'async_open': True, 'adaptive_batching': True,
#  'fast_file_copy': True}   # fast_file_copy：copy_file_range / CopyFile2 可用

caps["backend_detail"]   # 键随后端不同，以 Linux io_uring 为例：
# {'available': True, 'completion_model': True, 'sqpoll': True,
#  'features': {'single_mmap': True, 'fast_poll': True, ...},
#  'opcodes': {'read': True, 'write': True, 'statx': True, ...}}  # 内核 < 5.6 时为 None

# 典型用法：按内核实际能力决定是否启用可选特性
if caps["backend_detail"].get("sqpoll"):
    ayafileio.configure({"io_uring_sqpoll": True})
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
| `io_uring_sqpoll` | False | 是否启用 SQPOLL 模式 (Linux)。开启时按探测到的内核能力校验，不支持则抛 `ValueError`——建议先查 `get_capabilities()` |

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

### 复制文件

```python
n = await ayafileio.acopy("model.gguf", "backup/model.gguf")
```

`acopy(src, dst, *, chunk_size=4 MiB, concurrency=8, copy_stat=False)` 复制整个
文件，返回复制的字节数。Linux 上走内核态零拷贝 `copy_file_range`，Windows 上走
`CopyFile2`（顺带保留元数据），都在 worker 线程里执行。没有快速路径可用（或文件
系统拒绝）时，回退到 `read_at`/`write_at` 流水线：最多 `concurrency` 个块在飞，
内存占用不超过 `concurrency × chunk_size`。目标文件先截断；复制到自身抛
`shutil.SameFileError`；`copy_stat=True` 时完成后附加 `shutil.copystat`。

### 整文件操作

```python
data = await ayafileio.read_bytes("model.safetensors")
n    = await ayafileio.write_bytes("out.bin", payload)
text = await ayafileio.read_text("config.yaml", encoding="utf-8")
```

一次 await 完成 open + 读写 + close——面向 `asyncio.gather` 批量处理多文件
（按层加载权重、批量读配置）。语义与手写 `async with ayafileio.open(...)`
完全一致，只是省掉样板。

### 异步打开

```python
async with ayafileio.aopen("data.bin", "rb") as f:
    data = await f.read()
```

`aopen()` 与 `open()` 参数一致、返回同样的 `AsyncFile`——区别是 OS open
本身跑在 C++ 线程池，而不是阻塞事件循环线程。返回的是惰性句柄
（与 aiofiles 相同）：`async with` 在 `__aenter__` 里触发打开，写
`await ayafileio.aopen(...)` 显式等待也可以。批量打开大量文件时，open
系统调用本身也能并行；循环里同步 `open()` 则每个文件仍要付出一次短暂的
阻塞系统调用。

### 批量整文件操作

```python
shards  = await ayafileio.read_bytes_many(model_shard_paths)
configs = await ayafileio.read_text_many(config_paths, encoding="utf-8")
counts  = await ayafileio.write_bytes_many([(p, payload) for p, payload in items])
counts  = await ayafileio.write_text_many(items, encoding="utf-8")
```

一次调用并发读取（或写入）多个文件：打开经 `aopen()` 并行、I/O 走
平台异步后端、返回顺序与输入一致（写系列接收 `(path, data)` 对，
返回字节数）。`max_concurrency`（默认 64）限制在飞文件数，海量输入
不会打爆 fd；任一失败整体抛出——批量不是事务，写批量失败时部分文件
可能已经落盘。这是发挥异步后端性能的建议写法——串行 `for` 循环配
`open()` 无论单次操作多快，同一时刻都只有一个文件在飞。

### 配置函数

```python
def configure(options: dict) -> None: ...      # 统一配置
def get_config() -> dict: ...                   # 获取当前配置
def reset_config() -> None: ...                 # 重置为默认值
def get_backend_info() -> dict: ...             # 获取后端信息
def get_capabilities() -> dict: ...             # 能力矩阵（features + backend_detail）
```

### 包装已有文件

```python
def wrap_file(fd: int, mode: str = "rb", *, owns_fd: bool = False) -> AsyncFile[bytes]: ...
```

将已有的 `int` 文件描述符或带 `fileno()` 方法的对象包装为 `AsyncFile`。仅二进制模式。

### 池管理

```python
def drain_handle_pool() -> None: ...            # 清空句柄池中的所有缓存句柄
def drain_buffer_pool() -> None: ...            # 清空缓冲区池中的所有缓存缓冲区
```

适用于大量临时文件操作之后，或基准测试轮次之间的清理。

## 基准测试

每次推送都会在 CI 上三平台跑完整基准（`tests/t_compare.py`），JSON 结果作为
artifact 挂在每次运行上。除非标注*本地*，以下数字来自最近一次 CI 运行
（GitHub Actions, Python 3.14）。本地复现：`python tests/t_compare.py`。

### 随机 4 KiB 读（Linux, io_uring）

| 并发 | ayafileio | aiofiles | 倍数 |
|---:|----------:|---------:|---:|
| 1 | 191.5K ops/s | 9.8K ops/s | 19.6x |
| 16 | 361.6K ops/s | 10.2K ops/s | 35.4x |
| 64 | 361.2K ops/s | 12.5K ops/s | 28.9x |
| 256 | 349.9K ops/s | 12.3K ops/s | 28.5x |

单共享句柄上的位置读 `read_at` 从 x16 起稳定在 ~345K ops/s——aiofiles 没有
位置读 API，无从对比。4 KiB 小块顺序读 201.6K vs 21.9K ops/s（9.2x）。
顺序读吞吐：64 KiB 块 7,360 MB/s（6.6x），4 MiB 块 20,178 MB/s（1.3x）。

顺序写差距不大：256 KiB 块以内 1.2–1.3x，4 MiB 块在 CI 机器上反而落后
（0.4x）——大块缓冲写走内核 writeback 路径，不同轮次波动很大。关心这个
负载的话，直接看最近一次运行的 artifact 比看任何单个数字都靠谱。

### Windows 和 macOS

GitHub 的 Windows runner 对原始顺序吞吐限流明显，这几个 CI 格子没有代表性——
最近一次 windows-2022 上，64 KiB 顺序读只有对 aiofiles 的 0.79x、4 KiB 小读
0.62x——所以性能测试数字用的是*本地* NVMe 实测得出。但 1.11.0 记账修复之后，同一 runner
上的并发格子已经健康（随机 4 KiB 读、各自句柄）：x1 12.0K vs 7.4K ops/s
（1.6x）、x16 28.3K vs 5.4K（5.3x）、x64 31.3K vs 8.7K（3.6x）；顺序写则全面
领先（64 KiB：60.9K vs 8.2K ops/s，7.4x）。

- Windows (IOCP) 顺序写：1 MiB 块 1,363 → 2,858 MB/s，4 MiB 块 929 → 3,046 MB/s
  （对 aiofiles 1.47x）；4 KiB 随机 `read_at`、16 在飞：21.4K ops/s。
- macOS (Dispatch I/O)：缓存命中的小读在调用线程内联完成（mincore + pread）——
  4 KiB 顺序读 6.8K → 499K ops/s（对 aiofiles 27.2x）、4 KiB 随机读 x1
  4.6K → 432K ops/s（54.2x；x64 416K，25.7x）、64 KiB 顺序读 384 → 6,441
  MB/s（6.7x）；写路径不走快速路径（4 MiB 顺序写 8.2 GB/s）。macos-15 CI
  runner 实测（Python 3.14，`tests/t_compare.py` 口径）；小读格子在共享
  runner 上轮间抖动约 2x——此处引用的是最新一轮的数字，在 macOS fd 泄漏
  修复（修复前每次 open/close 都有额外同步开销——open 是同步版本，
  返回底层同步创建的异步文件句柄）之后测得。

### 整文件复制（`acopy`，*本地*，512 MiB 缓存热文件）

| 工具 | 耗时 | 吞吐 |
|------|-----:|-----:|
| `robocopy` | 0.150 s | 3,413 MiB/s |
| `acopy()`（CopyFile2 快车道） | 0.155 s | 3,300 MiB/s |
| `shutil.copyfile` | 0.217 s | 2,358 MiB/s |
| `acopy()` 强制流水线 | 0.330 s | 1,554 MiB/s |

### 并发日志写入（`tests/test_loguru.py`）

10K 条 × 128 B 并发写 5 个文件：各轮次对"同步写 + 线程池"快 3.5–5.2x。

### 调优

默认值就是调优的终点。特别快的 NVMe 上，Windows 可以试试 `iocp_batch_size`
128–256 配 `buffer_size` 128 KiB，按自己需求自己调整。

## 贡献

欢迎投稿「文文。新闻」：fork、开分支、加测试、确认基准测试仍然通过，然后发 PR。

## 许可证

MIT —— 最速最自由，详见 [LICENSE](LICENSE)。

---

**「遅いのは罪だぜ？」**
*—— 射命丸文，『文文。新闻』主编*
