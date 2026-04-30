# 更新日志

本文件记录项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)，
本项目遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

## [1.1.0] - 2026-04-30

### 新增
- **`tell()`**: 返回当前文件位置。纯内存操作，四个后端全部实现。
- **`truncate(size)`**: 截断/扩展文件到指定大小。
- **`fileno()`**: 返回底层文件描述符（POSIX）或 CRT fd（Windows）。
- **`readinto(buf)`**: 零拷贝直接读到预分配的 `bytearray` 或 `memoryview`，
  返回读取字节数而非新 `bytes` 对象。仅二进制模式可用。
- **`readable()` / `writable()` / `seekable()`**: 查询文件访问模式。
- **`writelines(lines)`**: 批量写入多行。
- **`readall()`**: `read(-1)` 别名。
- **`isatty()`**: 检查文件是否为 TTY。
- **`mode` 属性**: 返回原始模式字符串（如 `"rb"`, `"w+"`）。

### 变更
- **重构 `IOBackendBase`**: 四个公共方法（`complete_ok`, `complete_error`, `make_req`,
  `complete_error_inline`）提取到基类统一实现，消除四个后端共 ~240 行重复代码。
- **`IORequest` 扩展**: 新增 `isReadinto`, `userBuf`, `userBufView` 字段，支持
  零拷贝 `readinto()`。`buf()` 和析构函数自动处理 readinto 路径。
- **`io_backend.cpp` 的 `complete_ok`**: 使用 `switch-case` 分派，readinto
  请求返回 `int` 而非 `bytes`。
- **后端 `.hpp` 文件清理**: 删除 `m_pending`, `m_loop_handle` 等冗余声明，
  全部从 `IOBackendBase`（protected）继承。

## [1.0.5.post1] - 2026-04-29

### 修复
- **MacOSGCDBackend**: 修复了 macOS 上调用 `seek()` 或 `flush()` 时出现的严重
  `ContextVar` 重入错误和偶发性 `Segmentation fault`。这两个方法之前使用了
  `dispatch_io_barrier`，其回调可能在任意 GCD 线程上执行，并通过
  `PyGILState_Ensure` 尝试获取 Python GIL，与 `asyncio` 的内部上下文管理器
  状态发生冲突。在并发场景下（如快速 `open()` / `close()` 同一文件），还导致
  了文件描述符的 use-after-free。`seek()` 和 `flush()` 现在改为在主线
  程同步执行，完全消除了跨线程 GIL 竞争和对象生命周期问题。

### 变更
- **MacOSGCDBackend**: `seek()` 和 `flush()` 不再使用 `dispatch_io_barrier`。
  现在直接在调用线程上执行 `lseek` 和 `fsync`。由于这两个操作都在微秒级完成，
  同步方式避免了 GCD 调度开销，实际上更快且完全安全。
- `MacOSGCDBackend` 不再需要未使用的 `m_barrierMtx` 和 `m_barrierCv` 同步原语
  （可从头文件中移除）。

## [1.0.5] - 2026-04-28

### 新增
- `AsyncFile` 现为泛型类：`AsyncFile[str]` 表示文本模式，`AsyncFile[bytes]`
  表示二进制模式。IDE 自动补全和 mypy 可在编译时确定 `read()` 的返回类型。
- `open()` 使用 `@overload` 根据 mode 参数自动返回 `AsyncFile[str]` 或
  `AsyncFile[bytes]`。

### 变更
- `wrap_fd()` 返回类型从 `AyaFileIO[bytes]` 改为 `AsyncFile[bytes]`，增强类型推断。
- `AyaFileIO` 协议现为泛型版本 (`AyaFileIO[T]`)，保留在 `ayafileio.types` 作为
  内部类型，不再从 `__init__.py` 导出。用户应直接使用 `AsyncFile[str]` 或
  `AsyncFile[bytes]` 进行类型标注。
- 精简公开 API：`ayafileio` 现仅导出 `AsyncFile` 作为主要类型，从 `__all__`
  移除冗余的 `AyaFileIO`。

### 修复
- `wrap_fd()` 现在在传入非二进制模式时会在运行时抛出 `ValueError`，与文档说明
  保持一致。

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