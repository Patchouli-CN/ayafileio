# ayafileio 使用指南

[English](https://github.com/Patchouli-CN/ayafileio/blob/main/docs/USAGE.md)

本文档覆盖 ayafileio 的日常用法：打开文件、读写 API、定位 I/O、批量
操作、配置与迁移说明。性能数据与后端架构见
[README](https://github.com/Patchouli-CN/ayafileio/blob/main/README_CN.md)。

## 目录

- [安装](#安装)
- [快速开始](#快速开始)
- [两条打开路径：`open()` vs `aopen()`](#两条打开路径open-vs-aopen)
- [读与写](#读与写)
- [定位 I/O：`read_at` / `write_at` / `read_many` / `write_many`](#定位-io)
- [整文件操作](#整文件操作)
- [批量读取：`read_bytes_many` / `read_text_many`](#批量读取)
- [流式分块：`chunk()`](#流式分块chunk)
- [零拷贝读取：`readinto()`](#零拷贝读取readinto)
- [复制文件：`acopy()`](#复制文件acopy)
- [包装已有文件描述符](#包装已有文件描述符)
- [配置](#配置)
- [查看当前后端](#查看当前后端)
- [池管理](#池管理)
- [性能最佳实践](#性能最佳实践)
- [从 aiofiles 迁移](#从-aiofiles-迁移)
- [类型检查](#类型检查)

## 安装

```bash
pip install ayafileio
```

Python 3.10+（含 3.14t/3.15t 自由线程），Windows 7+ / Linux 内核 5.1+ /
macOS 10.10+。零依赖，三平台均提供预编译 wheel。

## 快速开始

```python
import asyncio
import ayafileio

async def main():
    async with ayafileio.open("notes.txt", "w", encoding="utf-8") as f:
        await f.write("你好\n")

    async with ayafileio.open("notes.txt", "r", encoding="utf-8") as f:
        print(await f.read())

asyncio.run(main())
```

所有操作都是普通协程——会用 `async`/`await` 就会用这个 API。
`AsyncFile` 支持迭代（`async for line in f`）、`readline()`、
`readlines()`、`seek()`/`tell()`、`truncate()`、`flush()`，以及常见的
`closed` / `name` / `mode` 属性。

模式与内置 `open()` 对齐：`r` / `w` / `a` / `x`，各有二进制 `b` 变体
和 `+` 读写组合。

## 两条打开路径：`open()` vs `aopen()`

### `ayafileio.open()` —— 默认入口

```python
async with ayafileio.open("data.bin", "rb") as f:
    data = await f.read()
```

同步构造：OS open 在调用线程完成（温缓存 SSD 上是微秒级）。
`FileNotFoundError` 这类错误在调用点立刻抛出。单个文件的"打开→I/O"
场景用它——延迟最低、失败语义最简单。

### `ayafileio.aopen()` —— 并行打开

```python
async with ayafileio.aopen("data.bin", "rb") as f:
    data = await f.read()
```

参数相同、返回同样的 `AsyncFile`——但 OS open 本身跑在 C++ 线程池，
不占事件循环。返回的是惰性句柄（aiofiles 同款），两种写法等价：

```python
async with ayafileio.aopen(path, "rb") as f:   # __aenter__ 里触发打开
    ...

f = await ayafileio.aopen(path, "rb")          # 显式 await 也可以
```

适合 `aopen()` 的场景是 open 系统调用本身成为瓶颈：

- **同时打开大量文件**——多个 open 真正并行
- 慢存储（机械盘、网络文件系统），单次 open 可能毫秒级，会卡住事件循环

本地存储开单个文件，`open()` 更划算：`aopen()` 每次要多付一次线程池
调度和一次事件循环唤醒。惰性句柄是一次性的——不要把同一个句柄复用进
第二个 `async with`。

## 读与写

```python
async with ayafileio.open("data.bin", "rb") as f:
    head = await f.read(1024)        # 前 1024 字节
    await f.seek(0)
    everything = await f.read()      # -1 = 读到 EOF

async with ayafileio.open("log.txt", "a", encoding="utf-8") as f:
    await f.write("一行\n")
    await f.writelines(["a\n", "b\n"])
```

二进制读返回 `bytes`；文本读按 `encoding` 解码返回 `str`（未指定时
沿用平台 locale 偏好编码，同内置 `open()`）。文本模式的 `newline`
换行翻译语义与内置一致。

一处与内置有意为之的差异：文本模式 `write()` 返回编码后写入的
**字节数**，不是字符数。

ayafileio 是无缓冲的：每次 `read()`/`write()` 都是一次真实 I/O 请求。
这正是单句柄上千并发操作便宜的原因，但也意味着逐字节循环读写很糟糕
——按块来。

## 定位 I/O

`read_at` / `write_at` 是 pread/pwrite 语义：在绝对偏移上操作，**不碰
逻辑文件位置**，因此与同句柄上并发的 `read()`/`seek()` 无竞争：

```python
async with ayafileio.open("model.safetensors", "rb") as f:
    header, layer0, layer1 = await asyncio.gather(
        f.read_at(0, 1024),
        f.read_at(1024, 1 << 20),
        f.read_at((1 << 20) + 1024, 1 << 20),
    )
```

`read_many` / `write_many` 在一个事件循环回合内提交整批请求，直接
gather 原生 future，不为每项创建 Python Task：

```python
chunks = await f.read_many([(0, 4096), (8192, 4096), (16384, 4096)])
counts = await f.write_many([(0, b"header"), (4096, b"block")])
```

使用约定（仅二进制模式；`write_at`/`write_many` 拒绝 append 模式）：
批量**不是事务**——报错前会先排空已提交的请求，此前的写入可能已落盘。
重叠区间顺序未定义，在意顺序请逐个 await。偏移必须非负，偏移加长度
须落在有符号 64 位内。与 `write()` 一样，调用方需检查短写。

数据布局已知时这是首选 API：模型分片、数据库页、索引区。

## 整文件操作

```python
data = await ayafileio.read_bytes("model.safetensors")
n    = await ayafileio.write_bytes("out.bin", payload)
text = await ayafileio.read_text("config.yaml", encoding="utf-8")
n    = await ayafileio.write_text("out.txt", "hello", encoding="utf-8")
```

一次 await 完成 open + 读写 + close。语义与手写
`async with ayafileio.open(...)` 完全一致，只是省掉样板。
（`write_text` 同样返回编码后的字节数。）

## 批量读取

异步文件操作最常见的错误写法是串行循环：

```python
# 无论单次读多快都慢——同一时刻只有一个文件在飞
for path in paths:
    async with ayafileio.open(path, "rb") as f:
        data = await f.read()
```

一行修复：

```python
shards  = await ayafileio.read_bytes_many(model_shard_paths)
configs = await ayafileio.read_text_many(config_paths, encoding="utf-8")
```

一次调用并发读取多个文件：打开走 `aopen()`（OS open 并行），读取走
平台异步后端，返回顺序**与输入一致**。`max_concurrency`（仅关键字，
默认 64）限制在飞文件数，十万级路径输入也不会打爆 fd。任一失败整体
抛出，与 `asyncio.gather` 默认语义相同。

实测 300 个 4 KiB 缓存热文件：对串行 aiofiles 循环 5.6x，对串行
ayafileio 循环 2.6x。

## 流式分块：`chunk()`

不想一次性读进内存的大文件：

```python
async with ayafileio.open("big.bin", "rb") as f:
    async for block in f.chunk(1 << 20):     # 1 MiB 一块
        process(block)                        # block 是 memoryview
```

`chunk(chunk_size, *, buf=None)` 产出 memoryview；传入自己的
`bytearray`/`memoryview` 作为 `buf` 可在迭代间复用同一缓冲区
（仅二进制模式）。

## 零拷贝读取：`readinto()`

```python
buf = bytearray(65536)
async with ayafileio.open("data.bin", "rb") as f:
    n = await f.readinto(buf)      # 数据直接落进 buf
```

后端直接读进你的缓冲区，没有中间拷贝。仅二进制模式。

## 复制文件：`acopy()`

```python
n = await ayafileio.acopy("model.gguf", "backup/model.gguf")
```

`acopy(src, dst, *, chunk_size=4 MiB, concurrency=8, copy_stat=False)`
复制整个文件并返回字节数。优先使用 OS 提供的最快路径——Linux 上是
`copy_file_range`（内核零拷贝），Windows 上是 `CopyFile2`（同时保留
元数据）；没有快路径时回退到 `read_at`/`write_at` 流水线，最多
`concurrency` 块在飞，内存占用不超过 `concurrency × chunk_size`。
目标文件先截断；源与目标相同抛 `shutil.SameFileError`；
`copy_stat=True` 复制后追加 `shutil.copystat`。

## 包装已有文件描述符

```python
f = ayafileio.wrap_file(fd, "rb", owns_fd=False)
```

把一个 `int` fd（或任何带 `fileno()` 的对象）包装成 `AsyncFile`。
仅二进制模式。`owns_fd=True` 时，关闭 `AsyncFile` 会连带关闭 fd。

## 配置

所有后端共享一份运行时配置：

```python
ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,
    "close_timeout_ms": 2000,
})

print(ayafileio.get_config())
ayafileio.reset_config()
```

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `handle_pool_max_per_key` | 64 | 每文件缓存句柄上限（Windows） |
| `handle_pool_max_total` | 2048 | 缓存句柄总数上限（Windows） |
| `io_worker_count` | 0 | I/O worker 线程数，0 = 自动 |
| `buffer_pool_max` | 512 | 缓存 I/O 缓冲区上限 |
| `buffer_size` | 65536 | 缓冲区大小（字节） |
| `close_timeout_ms` | 4000 | 有待完成 I/O 时的关闭超时（毫秒） |
| `iocp_batch_size` | 64 | IOCP 完成收割批量（Windows，1–256） |
| `io_uring_queue_depth` | 256 | io_uring 队列深度（Linux） |
| `io_uring_sqpoll` | False | 启用 SQPOLL 模式（Linux） |

默认值就是调优后的结果。在很快的 NVMe 上，Windows 侧
`iocp_batch_size` 128–256 配 `buffer_size` 128 KiB 还能再挤一点。

## 查看当前后端

```python
print(ayafileio.get_backend_info())
# {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True, ...}
```

后端自动选择：Windows 用 IOCP，Linux 5.1+ 用 io_uring，macOS 用
Dispatch I/O。原生后端不可用时（旧内核、受限容器），自动降级到线程池
后端，`is_truly_async` 为 `False`——两种情况 API 完全一致。

## 池管理

```python
ayafileio.drain_handle_pool()   # 释放所有缓存句柄
ayafileio.drain_buffer_pool()   # 释放所有缓存 I/O 缓冲区
```

批量临时文件操作之后、跑基准测试各轮之间很有用。

## 性能最佳实践

1. **一次打开，多次操作。** 即使句柄有池化，循环里反复开关依然每次
   多付一个协程往返：

   ```python
   async with ayafileio.open("data.bin", "rb") as f:
       for _ in range(10000):
           await f.seek(0)
           data = await f.read()
   ```

2. **多文件 → 批量 API，绝不写串行 `for` 循环。**
   `read_bytes_many()` 既是最短写法也是最快写法。

3. **偏移已知 → 定位 I/O。** `read_at`/`read_many` 省掉 seek/read
   往返，也不干扰句柄上的其他操作。

4. **大文件 → `chunk()` 或 `readinto()`**，而不是一次巨型 `read()`。

5. **大量小写 → 自己攒批**（定位写用 `write_many`，或累积后一次写）
   ——每次调用都是一次 I/O 请求。

## 从 aiofiles 迁移

常用面是即插即用：

| aiofiles | ayafileio |
|----------|-----------|
| `aiofiles.open(...)` | `ayafileio.open(...)` |
| `async with aiofiles.open(...)` | `async with ayafileio.open(...)` |

值得知道的差异：

- **真内核异步。** 三平台都不是线程池冒充（aiofiles 全部走
  `asyncio.to_thread` 式执行器）。
- **无缓冲。** aiofiles 默认带缓冲；ayafileio 不缓冲，这也是两者小
  顺序读曲线不同的原因。按块读取。
- **文本模式 `write()` 返回编码后字节数**，不是字符数。
- **aiofiles 没有的 API：** `read_at`/`write_at`/`read_many`/
  `write_many`、`aopen()`、`read_bytes_many()`/`read_text_many()`、
  `acopy()`、`readinto()`、`chunk()`。
- `aiofiles.os`（异步 `stat`、`listdir` 等）**不在覆盖范围**——
  ayafileio 只做文件 I/O。

## 类型检查

ayafileio 自带 `py.typed` 标记，mypy/pyright 开箱即用。`open()` 和
`aopen()` 都按模式字符串做了重载：

```python
async with ayafileio.open("a.txt", "r") as f:
    reveal_type(f)   # AsyncFile[str]
async with ayafileio.open("b.bin", "rb") as f:
    reveal_type(f)   # AsyncFile[bytes]
```

`AsyncFile[str]` 读出 `str`，`AsyncFile[bytes]` 读出 `bytes`——
模式写错的 `write()` 调用在类型检查阶段就会被拦下，不用等运行时的
`TypeError`。
