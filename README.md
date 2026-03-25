
# ayafileio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python Version](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/platform-Cross--platform-blue.svg)](https://en.wikipedia.org/wiki/Cross-platform)

**跨平台异步文件 I/O 库，使用原生异步 I/O 机制。**

在 Windows 上使用 IOCP（I/O 完成端口），在 Linux 上使用线程池模拟异步 I/O，实现真正的异步文件操作。

## 🚀 核心特性

- ✅ **零线程开销**：无需 `run_in_executor`，无后台线程 (Windows)
- ✅ **内核级完成**：无用户态调度延迟 (Windows)
- ✅ **高并发友好**：轻松处理数千并发文件操作
- ✅ **标准 API**：熟悉的文件式接口，支持 async/await
- ✅ **文本二进制支持**：文本模式自动编解码
- ✅ **可配置句柄池**：根据工作负载调整性能 (Windows)
- ✅ **跨平台**：Windows 和 Linux 支持

## 🛠️ 安装

```bash
pip install ayafileio
```

**系统要求：**
- Python 3.10+
- Windows 7 / Server 2008 R2 或更高版本，或 Linux
- 无其他依赖

## 🚀 快速开始

```python
import asyncio
import ayafileio

async def main():
    # 基础异步文件操作
    async with ayafileio.open("example.txt", "w") as f:
        await f.write("Hello, async world!\n")
        await f.flush()

    # 读取并自动文本解码
    async with ayafileio.open("example.txt", "r", encoding="utf-8") as f:
        content = await f.read()
        print(content)  # "Hello, async world!\n"

    # 二进制操作
    async with ayafileio.open("data.bin", "rb") as f:
        data = await f.read(1024)
        await f.seek(0, 0)  # 定位到开头

asyncio.run(main())
```

## ⚙️ 高级配置

### 句柄池调优

对于高并发工作负载，调整句柄池大小：

```python
import ayafileio

# 查看当前限制
max_per_key, max_total = ayafileio.get_handle_pool_limits()
print(f"当前: 每键 {max_per_key}，总计 {max_total}")

# 增加以提升多文件性能
ayafileio.set_handle_pool_limits(128, 4096)
```

这会在打开/关闭周期中重用文件句柄，减少昂贵的 `CreateFile` 调用。

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

- `"r"`, `"rb"`: 读取（文本/二进制）
- `"w"`, `"wb"`: 写入（文本/二进制）
- `"a"`, `"ab"`: 追加（文本/二进制）
- `"x"`, `"xb"`: 独占创建（文本/二进制）
- 加上 `"+"` 用于读写组合

### 函数

```python
def set_handle_pool_limits(max_per_key: int, max_total: int) -> None: ...
def get_handle_pool_limits() -> tuple[int, int]: ...
```

## 🧪 运行基准测试

克隆仓库并运行基准测试套件：

```bash
git clone https://github.com/your-repo/ayafileio.git
cd ayafileio
python run_benchmark.py
```

这会在不同并发级别比较 `ayafileio` 与 `aiofiles`。

## 🤝 贡献

欢迎贡献！请：

1. Fork 本仓库
2. 创建功能分支
3. 为新功能添加测试
4. 确保基准测试仍通过
5. 提交拉取请求

## 📄 许可证

MIT 许可证 - 详见 [LICENSE](LICENSE)。

## ⚠️ 限制

- **Python 3.10+**：需要现代 Python 特性

## 🙏 致谢

基于 Windows IOCP 构建真正的异步 I/O。受 Python 高性能异步文件操作需求启发。
