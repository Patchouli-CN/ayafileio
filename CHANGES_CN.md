# 更新日志

本文件记录项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)，
本项目遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

## [1.0.4] - 2026-04-26

### 新增
- **`wrap_fd(fd, mode, *, owns_fd)`**: 将现有**文件**描述符包装为异步 I/O 对象，
  底层自动使用最优平台后端（io_uring / IOCP / Dispatch I/O）。
  Windows 上会透明地将 fd 升级为支持 OVERLAPPED 的句柄。
  仅支持文件描述符；socket 和 pipe 请交由事件循环管理。
- **`AyaIO` 协议类型**（`ayafileio.types`）: 统一的异步 I/O 接口
  （`read()`, `write()`, `seek()`, `flush()`, `close()`, `closed`），
  `AsyncFile` 和 `wrap_fd()` 的返回值均符合此协议。

### 变更
- 所有后端（`IOUringBackend`、`MacOSGCDBackend`、`ThreadIOBackend`、
  `WindowsIOBackend`）现支持通过 `FileHandle(int fd, mode, owns_fd)` 从原始
  文件描述符构造。
- `close_impl()` 遵循 `owns_fd` 标志——外部传入的文件描述符在 `owns_fd=False`
  时不会被 ayafileio 关闭。
- Windows 上 `wrap_fd()` 通过 `GetFinalPathNameByHandleW` 从 CRT fd 获取文件路径，
  若 `owns_fd=True` 则在重开前关闭原始 fd，并用 `FILE_FLAG_OVERLAPPED` 重新打开
  以实现真正的 IOCP 异步 I/O。

### 修复
- 修复 Windows 上 `wrap_fd()` 仅写模式调用 `read()` 时出现 `PermissionError`
  的问题——重开的句柄现在始终请求 `GENERIC_READ | GENERIC_WRITE`。

## [1.0.3] - 2026-04-26

### 新增
- `open()` 新增 `newline` 参数，支持自定义行尾符转换
- `open()` 新增 `errors` 参数，支持非严格编码错误处理

### 变更
- `AsyncFile.__slots__` 中包含 `_newline` 和 `_errors` 属性

## [1.0.2 & 1.0.2.post1] - 2026-04-26

### 新增
- Linux 后端现支持通过 `io_uring` 的 `IORING_OP_OPENAT` 异步打开文件，使用专用本地 ring 避免与 reaper 线程竞争。
- 新增基准测试场景："临时文件风暴"（无句柄复用的 open-read-close，2000 个文件 × 4KB）。

### 变更
- **完全重写 `IOUringBackend` 架构**：
  - 文件打开（`open`）现使用独立的 `io_uring` 实例（`local_ring`），与 reaper 线程的共享 ring 完全隔离。
  - 共享 ring（由 `UringManager` 管理）现延迟到首次 `read()` 或 `write()` 调用时才初始化，而非在构造期间。
  - 此"双 ring"设计消除了构造函数与 reaper 之间的所有 CQE 竞争，解决了 Linux 上持续出现的 segfault。
- `ensure_loop_initialized()` 简化：现在仅负责获取用于读写操作的共享 ring。
- Reaper 循环精简：不再需要处理 `char*` 类型的 user_data（因为 OPENAT 使用自己的 ring），减少热路径分支。

### 修复
- 修复 Linux 上因构造函数的 `IORING_OP_OPENAT` 与 reaper 线程在同一 ring 上竞争 `io_uring_wait_cqe` 而导致的 segfault。

## [1.0.1.post2] - 2026-04-26

### 修复
- **ThreadIOBackend**: 修复 `close_impl()` 中的死锁问题。现在在停止 worker 线程前会先调用 `m_cv.notify_all()` 唤醒它们，而不是直接等待 `join()`。
- **MANIFEST.in**: 修复文件名拼写错误 (原为 `MAIFEST.in`)。

### 新增
- **CHANGES.md**: 开始维护更新日志。

### 变更
- **config.hpp**: 移除未使用的 `enable_debug_log` 和 `enable_perf_stats` 配置项。移除空的 `from_env()` 方法和未实现的回调系统 (`register_callback`, `on_config_changed`)。精简代码约 130 行。
- **CMakeLists.txt**: 当未找到 liburing 时，将 CMake 消息级别从 `STATUS` 提升为 `WARNING`，提示用户安装相应的开发包。

## [1.0.1] — 2026-04-25

### 新增
- 新增 `CHANGES.md` 和 `CHANGES_CN.md`，提供中英双语更新日志。
- 基于 `TypedDict` 的 `AyafileioConfig`，为 `configure()` 提供 IDE 友好的自动补全。
- 在 `test_speed.py` 基准测试中新增 `asyncio.sleep(0)` 延迟校准基准。

### 变更
- 重构 `__init__.py`：将职责分离到 `_async_file.py`、`_open.py`、`_config.py`、`_compat.py`、`_cleanup.py`。
- 改进 `configure()`，接受 `TypedDict` 以获得更好的类型检查和自动补全。
- CMake 在找不到 `liburing` 时现在发出 `WARNING` 而非 `STATUS`，用户可知自己处于线程池回退状态。
- 改进 `warn_fake_async()` 消息，添加 `liburing-dev` / `liburing-devel` 的安装指引。

### 修复
- 修复 `MANIFEST.in` 文件名拼写错误。
- 修复 Windows 上 `test_loguru.py` 缺少 `import io` 的问题。
- 修复 Linux 上 `ThreadIOBackend` 在并发读取负载下可能死锁的问题。
- 修复未安装 Rich 时 `print_stats`/`print_latency_detail` 的 Rich 标记错误。

---

## [1.0.0] — 2026-04-24

### 新增
- 首次公开发布。
- Windows (IOCP)、Linux (io_uring)、macOS (Dispatch I/O / GCD) 全平台真异步文件 I/O。
- `AsyncFile` 类，提供与 aiofiles 兼容的熟悉 API。
- 统一的 `configure()` 运行时配置系统，支持热加载。
- `get_backend_info()` 用于运行时后端检测。
- 按大小分桶的 `BufferPool`，提升内存效率。
- `LoopHandle` 批量调度机制，减少 GIL 争用。
- 跨平台基准测试套件 (`test_speed.py`)，与 aiofiles 性能对比。
- Loguru 异步 sink 示例及基准测试 (`test_loguru.py`)。
- 通过 GitHub Actions 预编译 Python 3.10–3.14 的 wheel，覆盖 Windows、Linux、macOS。