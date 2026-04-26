
---

# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Cross--platform-blue.svg)](https://en.wikipedia.org/wiki/Cross-platform)
[![Version](https://img.shields.io/badge/version-1.0.2-red.svg)]()

**当前是中文** | [**english version**](README.md)

> **「幻想郷最速のファイルI/O、風神少女の如く」**  
> *—— 射命丸文，今日も全力で翔ける*

**跨平台异步文件 I/O 库，使用原生异步 I/O 机制。**  
Windows 上借 **IOCP**（I/O 完成端口）之力，Linux 上用 **io_uring**（内核 5.1+），macOS 上用 **Dispatch I/O (GCD)**，实现真正无阻塞的文件操作。

## 更改变动

**请看此处** -> [**变动**](CHANGES_CN.md)

## 🏆 独一无二的三平台真异步

| 平台 | 后端 | 真异步 | 说明 |
|------|------|--------|------|
| **Windows** | IOCP | ✅ | NT 内核原生 I/O 完成端口 |
| **Linux** | io_uring | ✅ | Linux 5.1+ 下一代异步 I/O |
| **macOS** | Dispatch I/O | ✅ | GCD 内核级异步 I/O |

**ayafileio 是 Python 生态中唯一同时支持三大平台真异步的文件 I/O 库。**

## 📸 核心特性

| 特性 | 说明 |
|------|------|
| 🍃 **零线程开销** | 真异步平台无后台线程，无需 `run_in_executor` |
| 📰 **内核级完成** | IOCP / io_uring / Dispatch I/O 直达内核 |
| ⚡ **高并发友好** | 数千并发文件操作，文文团扇一挥间 |
| 🎴 **标准 API** | 与 aiofiles 兼容，支持 `async/await` |
| 📖 **文本二进制支持** | 文本模式自动编解码 |
| 🔧 **统一配置系统** | 运行时动态调整所有参数 |
| 🌍 **跨平台** | Windows / Linux / macOS 皆可翱翔 |
| 🐍 **最新 Python** | 支持 3.10, 3.11, 3.12, 3.13, 3.14 |

## 🛠️ 安装

```bash
pip install ayafileio
```

**系统要求：**
- Python 3.10+
- Windows 7+ / Linux (内核 5.1+ 可启用 io_uring) / macOS 10.10+
- 无其他依赖，预编译 wheel 开箱即用

## 🚀 快速开始

```python
import asyncio
import ayafileio

async def main():
    # 写入文件——像风一样快
    async with ayafileio.open("example.txt", "w") as f:
        await f.write("Hello, async world!\n")

    # 读取并自动解码——文文新闻，一触即达
    async with ayafileio.open("example.txt", "r", encoding="utf-8") as f:
        content = await f.read()
        print(content)

    # 二进制操作——数据如风，来去无痕
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read(1024)
        await f.seek(0, 0)

asyncio.run(main())
```

## ⚡ 性能最佳实践

ayafileio 的文件打开/关闭开销已优化至极低（微秒级），但为发挥最高性能，**请勿在循环中反复打开同一文件**。

```python
# ❌ 不推荐：循环内反复 open/close（每次都有协程调度开销）
for i in range(10000):
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read()

# ✅ 推荐：打开一次，多次操作
async with ayafileio.open("data.bin", "rb") as f:
    for i in range(10000):
        await f.seek(0)
        data = await f.read()
```

## 🔍 后端信息

查看当前使用的后端：

```python
import ayafileio

info = ayafileio.get_backend_info()
print(info)
# Windows: {'platform': 'windows', 'backend': 'iocp', 'is_truly_async': True}
# Linux:   {'platform': 'linux', 'backend': 'io_uring', 'is_truly_async': True}
# macOS:   {'platform': 'macos', 'backend': 'dispatch_io', 'is_truly_async': True}
```

## ⚙️ 统一配置

`ayafileio` 提供统一的配置系统，所有参数可在运行时动态调整：

```python
import ayafileio

# 查看当前配置
config = ayafileio.get_config()
print(config)

# 修改配置——风势加强！
ayafileio.configure({
    "io_worker_count": 8,
    "buffer_size": 131072,      # 128KB 缓冲区
    "close_timeout_ms": 2000,
})

# 重置为默认值
ayafileio.reset_config()
```

### 配置项说明

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `handle_pool_max_per_key` | 64 | 每个文件最大缓存句柄数 (Windows) |
| `handle_pool_max_total` | 2048 | 全局最大缓存句柄数 (Windows) |
| `io_worker_count` | 0 | I/O 工作线程数，0=自动 |
| `buffer_pool_max` | 512 | 最大缓存缓冲区数 |
| `buffer_size` | 65536 | 单个缓冲区大小 (字节) |
| `close_timeout_ms` | 4000 | 关闭时等待 pending I/O 的超时 (ms) |
| `io_uring_queue_depth` | 256 | io_uring 队列深度 (Linux) |
| `io_uring_sqpoll` | False | 是否启用 SQPOLL 模式 (Linux) |
| `enable_debug_log` | False | 是否启用调试日志 |

## 📚 API 参考

### AsyncFile 类

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

### 支持的模式

| 模式 | 说明 |
|------|------|
| `"r"`, `"rb"` | 读取（文本/二进制） |
| `"w"`, `"wb"` | 写入（文本/二进制） |
| `"a"`, `"ab"` | 追加（文本/二进制） |
| `"x"`, `"xb"` | 独占创建（文本/二进制） |
| 加上 `"+"` | 读写组合 |

### 配置函数

```python
def configure(options: dict) -> None: ...      # 统一配置
def get_config() -> dict: ...                   # 获取当前配置
def reset_config() -> None: ...                 # 重置为默认值
def get_backend_info() -> dict: ...             # 获取后端信息
```

## 🧪 性能对比

模拟 Crawlee 爬虫框架的 Dataset 追加写入场景（5000 条记录，50 并发）：

| 平台 | ayafileio | aiofiles | 提速 |
|------|-----------|----------|------|
| **Windows (NVMe SSD)** | **41,336 条/秒** | 9,658 条/秒 | **4.28x** |
| **Linux (NVMe SSD)** | **17,688 条/秒** | 11,455 条/秒 | **1.54x** |
| **macOS (NVMe SSD)** | **29,837 条/秒** | 25,522 条/秒 | **1.17x** |
| **Windows (6年旧机械盘)** | **20,251 条/秒** | 13,011 条/秒 | **1.56x** |

**关键发现：**
- Windows 企业级 SSD 上，ayafileio 的 P99 写入延迟仅 **0.044ms**，aiofiles 为 **1.854ms**（低 42 倍）
- aiofiles 在负载下抖动高达 **96.7%**，ayafileio 仅 **16.2%**
- 即使在即将报废的机械硬盘上，ayafileio 依然保持稳定性能

> *测试环境：Windows 10/11, Ubuntu 22.04, macOS 14；GitHub Actions 企业级 NVMe SSD*

## 🤝 贡献

欢迎投稿「文文。新闻」！请：

1. Fork 本仓库
2. 创建功能分支（`git checkout -b feature/amazing-feature`）
3. 添加测试
4. 确保基准测试仍通过
5. 提交拉取请求

## 📄 许可证

MIT 许可证 —— **最速最自由**，详见 [LICENSE](LICENSE)。

---

**「遅いのは罪だぜ？」**  
*—— 射命丸文，『文文。新闻』主编*

---
