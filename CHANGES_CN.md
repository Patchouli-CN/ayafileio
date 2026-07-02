# 更新日志

本文件记录项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)，
本项目遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

## [1.4.8] - 2026-07-02

### 修复
- **`readline()` 与 `read()`/`readinto()`/`chunk()` 混用时丢数据**：`readline()` 会预读最多 64 KB 到行缓冲，但 `read()`/`readinto()`/`chunk()` 直接从 C++ 层读取，预读缓冲里尚未消费的数据被静默跳过。现在所有读取路径都会先消费预读缓冲。
- **`seek()` 不清空 readline 预读缓冲**：`readline()` 之后 `seek(0)` 再 `readline()` 返回的是旧缓冲里的陈旧数据而不是新位置的行。现在 `seek()` 会清空缓冲，且相对定位（`whence=1`）以用户实际消费到的逻辑位置为基准——与内置 `open()` 语义一致。
- **`tell()` 返回的是物理（预读）位置**：读一行短行之后 `tell()` 报告 64 KB 而不是行尾位置。现在会减去缓冲中未消费的字节数。`write()` 和 `truncate()` 同样会先把底层位置回退到逻辑位置再执行（对 `r+`/`+` 模式有意义）。
- **`async with wrap_file(...)` 退出时崩溃**：`AsyncFile._from_impl` 漏初始化 `_auto_flush` 和 `_mode` 两个 slot，退出上下文管理器时抛 `AttributeError`，包装文件上的 `.mode`/`readable()`/`writable()` 也同样损坏。现在所有 slot 均被初始化，`wrap_file` 会把 mode 透传下去。

### 性能
- **`readline()` 提速 4–5 倍**（13.4 MB / 20 万行文件，Windows：文本通用换行模式 1640 → 405 ms，二进制 1516 → 293 ms）：
  - 通用换行模式（默认模式）的行尾扫描原来是逐字节 Python 循环，现在改用 C 级 `find()`，并把 `\r` 的扫描范围限制在第一个 `\n` 之前，保证每次扫描 O(行长)。
  - 取行原来每行都把整个剩余缓冲切片复制两次（每个块内 O(n²)），现在改为游标偏移消费，每次 64 KB 补块时只压缩一次。
- **IOCP 完成热路径减负**（实测并发 4 KB 写吞吐 +8%；读取受磁盘限制无变化）：
  - 每个 `Session` 在打开时缓存自己的 `ResultBatcher*`——完成路径不再每次加锁查哈希表。
  - 自适应批处理中位数从每次完成都重算改为每 16 次完成重算一次（原来是每个 I/O 都在持批处理锁 + GIL 状态下做 O(128) 的 `nth_element`）。
  - `flush_batchers()` 改用新增的单次持锁 `ResultBatcher::flush_if_idle()`，替代原来每个 batcher 每轮三次独立加锁。
  - 所有后端不再在提交时预取 `future.set_exception`；错误路径按需获取，成功路径每个 I/O 省一次属性查找。
- **BufferPool 改为按 2 的幂尺寸档分配**（最小 4 KB）：原来按精确请求字节数分配，池被大量互不匹配的尺寸碎片化，变长负载下缓冲几乎无法复用。

## [1.4.7] - 2026-07-01

### 修复
- **`submit_*` 返回裸 `NULL` 却未设置异常（Windows/IOCP）**：当 session 已不存在时（例如操作与 `close()` 竞态），`submit_seek`、`submit_flush`、`submit_tell`、`submit_truncate`、`submit_readinto` 会在没有活跃 Python 异常的情况下返回 `nullptr`。nanobind 绑定层的 `throw py::python_error()` 随后把它暴露成令人困惑的 `SystemError: <built-in> returned NULL without setting an error`。现在这些路径统一返回一个携带 `ValueError("File not open.")` 的已失败 future，与 `submit_read`/`submit_write` 保持一致。其中 `seek()`/`tell()` 在 Python 层没有 `_closed` 前置检查，因此确实会走到这条 C++ 路径。
- **IOCP 初始化警告缺少格式占位符**：`init_iocp()` 失败时的 `printf` 用了 `"Warning: Failed to init IOCP:"` 却没有 `%s`，导致 `e.what()` 被静默丢弃。现在会正确打印失败原因。

## [1.4.6] - 2026-06-16

### 新增
- **文本模式完整 `newline` 语义**：`AsyncFile` 现在遵循 Python 内置 `open()` 的换行符行为：
  - `newline=None`（默认）：通用换行模式 — 读取时把 `\r\n`、`\r` 都转为 `\n`；写入时把 `\n` 转为 `os.linesep`。
  - `newline=""`：通用换行但不翻译 — 识别 `\r\n`、`\r`、`\n` 为行尾并原样返回。
  - `newline="\n"`、`"\r"`、`"\r\n"`：固定行尾；写入时将 `\n` 翻译为指定行尾。
  - 二进制模式现在会拒绝 `newline` 参数，与标准库一致。

### 修复
- **`AsyncFile.open` 类方法签名**：补齐缺失的 `auto_flush` 参数，使其与模块级 `open()` 函数一致。
- **构建配置清理**：
  - `CMakeLists.txt` 与 `pyproject.toml` 统一使用 C++20。
  - 删除 `pyproject.toml` 中冗余的 `[tool.setuptools]` 配置（由 scikit-build-core 处理打包）。
  - 将已弃用的 `cmake.minimum-version` 替换为 `cmake.version`，适配 scikit-build-core >= 0.8。

### 变更
- **README 措辞**：收敛了“唯一真异步”等过于绝对的营销表述，保留所有东方/幻想乡元素不变。

## [1.4.5] - 2026-06-09

### 新增
- **自适应 `ResultBatcher` — 智能批处理阈值**: 完成包批处理器现在通过 128 项环形缓冲区追踪 I/O 完成间隔，利用滚动中位数动态调节批处理阈值：

  ```
  dynamic_threshold = adaptive_target_latency_us / median_interval_us
  dynamic_idle_ms    = min(idle_max, median_us * 4 / 1000)
  ```

  - **快盘 (NVMe, ~10μs 间隔)**: 阈值自动升至 100+，每次 `call_soon_threadsafe` 批量处理更多完成包 — 降低 GIL 压力与调度开销
  - **慢盘 (HDD, ~10ms 间隔)**: 阈值降至 1，立即 flush — 不为尚未到达的完成包空等
  - **空闲超时同步自适应**: 按观测到的 I/O 速率等比缩放

  默认启用。新增配置项: `adaptive_batch` (bool, 默认 `True`) 和 `adaptive_target_latency_us` (unsigned, 默认 1000μs, 范围 1-10000)。

- **benchmark 脚本国际化 (i18n)**: `tests/i18n.py` 模块，自动检测系统语言（系统 locale + `AYAFILEIO_LANG` 环境变量），基于 YAML 的翻译文件存放于 `tests/lang/`。所有 benchmark 脚本 (`bench_compare.py`、`bench_tuned.py`、`stress_single_file.py`) 均支持中英文自动切换。

### 变更
- **README 性能章节大幅扩展**: 新增两个实测场景及详细数据：
  - **场景二：单文件高并发随机读** — 100K 并发任务共享一个文件句柄。ayafileio vs aiofiles 在 1K/10K/50K/100K 四个并发级别下实测对比。100K 时 ayafileio 比 aiofiles 快 **9.1x**（19,290 vs 2,130 ops/s，同为 HDD）。aiofiles 吞吐量**随并发增加反而下降**（7,706 → 2,130），而 ayafileio **先升后稳**（7,487 → 46,616 at 10K）。
  - **场景三：50 万并发极限压测** — 50 万个 asyncio 任务从同一文件 IOCP 并发读取。**零异常**，23,116 ops/s，仅 2 个 worker 线程，~583 MB RSS。
  - **配置调优结论**: 实测 14 种配置组合 — 默认配置最优（HDD 上全部在 ±3% 以内）。瓶颈是物理磁盘而非软件。

### 改进
- **CI: 扩展 `paths-ignore`**: 新增 `tests/bench_*.py`、`tests/stress_*.py`、`tests/t_compare.py`、`tests/test_loguru.py`、`tests/lang/**`、`tests/i18n.py`、`ignore/*.py` — benchmark 和纯配置变更不再触发完整 CI 构建。

## [1.4.4] - 2026-06-02

### 修复
- **macOS GCD fd 复用竞态条件**: `dispatch_io_create` 会接管文件描述符的所有权，并在 `dispatch_io_close` 时异步关闭它。之前使用的 `dispatch_barrier_sync` 只能保证 dispatch queue 排空，无法保证内核层 fd 关闭已完成。当后续 `open()` 在 GCD 延迟关闭完成前复用了同一个 fd 号时，GCD 会误关**新**文件的 fd，导致 macOS 上偶发 `OSError: [Errno 9] EBADF`（错误的文件描述符）。通过在传给 `dispatch_io_create` 前 `dup()` fd 修复——GCD 操作 dup 副本，原始 fd 完全由我们控制，从根本上消除竞态。

### 变更
- **CI：合并 `release.yml` 到 `build_wheel_ci.yml`**: 两个 workflow 触发器相同但构建矩阵不同。`build_wheel_ci.yml` 是严格超集（更多架构：riscv64、aarch64、x86、universal2；更多 runner：macos-15-intel；更多 Python 版本：含 cp313t）。删除 `release.yml`，在 `build_wheel_ci.yml` 中新增 PyPI 发布 job 和显式 `CIBW_BUILD` 版本指定。

## [1.4.3] - 2026-05-30

### 新增
- **`AsyncFile.chunk(chunk_size, *, buf=None)` — 流式读取迭代器**: 基于 `readinto` 的零拷贝流式分块读取。每次迭代返回一个 `memoryview` 指向填充了本次读取数据的缓冲区切片，避免每次分块分配新内存。适用于大文件流式处理、网络上传分片等场景。
  ```python
  # 内置缓冲区
  async for chunk in f.chunk(4096):
      process(chunk)

  # 预分配缓冲区（高频场景更高效）
  buf = bytearray(65536)
  async for chunk in f.chunk(4096, buf=buf):
      sock.send(chunk)
  ```
  仅支持二进制模式（文本模式抛出 `ValueError`，请使用 `readline`）。当用户提供的 `buf` 容量小于 `chunk_size` 时自动收窄为缓冲区容量。

### 变更
- **幽幽子 `yuyuko_memlife.hpp` 修复**:
  - `static` 全局变量 → `inline`（修复 header-only ODR 问题：多编译单元包含时全局状态不再分裂为独立副本）
  - `static` 自由函数 → `inline`（消除每编译单元的代码膨胀）
  - 修复 `yuyuko_logln` 中 `snprintf` 截断时越界读取 `buf` 的 bug
  - `find_soul` / `find_containing_soul` 返回 `const SoulRecord*`，消除 `const_cast` hack
  - 补 `<thread>` 头文件显式 include
- **幽幽子增量 CRC 更新**: 新增 `mem_snapshot_update(ptr, offset, len)` — 小幅修改大块内存时只重算受影响的 chunk。1GB 缓冲区修改 4B：全量重算 ~130ms → 增量更新 ~0.5μs（快 26 万倍）。配套宏 `MEM_SNAPSHOT_UPDATE`。
- **幽幽子升级 C++20**: 异步日志引擎 `std::thread` → `std::jthread`，RAII 自动 join 简化关闭逻辑。`__cpp_lib_jthread` 特性检测兼容 AppleClang 15。
- **`ENABLE_ASAN` → `ENABLE_YUYUKO`**: 编译宏重命名以准确反映功能（幽幽子内存追踪器，非 AddressSanitizer）。Windows MSVC 默认开启（ASAN 支持不完善），Linux/macOS 默认关闭（有原生 ASAN 可用），可手动 `-DENABLE_YUYUKO=ON` 开启。
- **`ignore/yuyuko` ↔ `src/utils` 版本同步**: 两处 `yuyuko_memlife.hpp` 功能代码完全一致，仅保留生产版的 `ENABLE_YUYUKO` 条件编译和零开销 stub。

## [1.4.2] - 2026-05-29

### 修复
- **IOCP 双重投递 use‑after‑free 崩溃修复**: 在极端并发（~50K 协程）下，Windows IOCP 可能将同一 `OVERLAPPED` 完成包多次投递给 `GetQueuedCompletionStatusEx`——包括跨批次调用，间隔约 10–15ms。当第一次投递释放了 `IORequest` 后、后续投递到达时，若堆已将该地址复用给新的 `IORequest`，双重投递 CAS 守卫（`IOState`）会看到**新**对象的 `PENDING` 状态并错误处理该过期完成包，破坏新 `IORequest` 的数据，最终在 `PyGILState_Release` 中导致访问违例。通过**隔离区延迟释放**机制修复：`process_one` 将释放的 `IORequest` 插入线程本地隔离列表而非立即删除，隔离对象持有 1 秒后才真正释放，防止在 IOCP 双重投递窗口内发生地址复用。同时修正了 CAS 守卫顺序——`compare_exchange_strong` 现在在 `Yuyuko::check_access` **之前**执行，使双重投递由 CAS 静默拒绝，无需触碰任何可能已失效的字段。
- **幽幽子内存追踪器增强**（用于诊断上述 bug）：新增 `TRACKED_NEW_ARGS` / `TRACKED_PLACEMENT_NEW` / `TRACKED_PLACEMENT_NEW_ARGS` 宏，支持带参构造和 placement‑new 的分配追踪。新增 `reserve_souls(alive_cap, released_cap)` 预分配魂簿哈希表容量。新增 `MEM_GUARD_BATCH_DEF` / `MEM_GUARD_BATCH_ADD` 批量守卫便捷宏。

### CI
- **`test_critical.py` 重写提升稳定性**: 写入测试现为每个 worker 使用独立的预创建文件，而非所有 worker 打开同一 `"wb"`（`CREATE_ALWAYS`）文件——消除了并发截断导致的数据损坏。添加了 `asyncio.gather(return_exceptions=True)` + 失败汇总报告、压测期间 `gc.disable()`、基于 `tempfile.mkdtemp` 的清理机制，并将读取测试文件从 1GB 缩减至 100MB。

## [1.4.1] - 2026-05-29

### 变更
- **IOCP 文件大小缓存**: `submit_read`、`submit_readinto`、`submit_write`（追加模式）和 `submit_seek`（SEEK_END）不再每次操作调用 `GetFileSizeEx`。文件大小现缓存于 `Session` 结构体中——在 `create_session` 时初始化一次，write/truncate 时刷新——消除了热路径上每次 I/O 的内核 syscall。`windows_io_backend.cpp` 构造函数也使用缓存值进行追加模式位置初始化。
- **`ResultBatcher` 双触发 flush**: 将 `flush_batchers()` 中无条件的 `|| true` flush 替换为 `process_one` 中的阈值触发。当 `push()` 达到批次阈值时立即 flush；否则由空闲超时（5ms）控制 flush 时机。低到中等负载下减少不必要的 `call_soon_threadsafe` 调用，高负载下保持响应。
- **线程本地 `BufferPool` 缓存**: `pool_acquire*` / `pool_release` 现使用 per-thread 缓存（最多 8 个缓冲区），miss 后才回退到全局 `BufferPool` mutex。常见场景——同一线程释放后迅速重新获取——完全避免了全局锁。

### 修复
- **`test_critical.py` 写入测试数据损坏**: 写入压力测试原先所有 worker 以 `"wb"`（`CREATE_ALWAYS`）模式打开同一文件路径，导致并发截断和数据损坏。现每个 worker 使用独立的预创建文件。添加了 `return_exceptions=True` + 失败汇总报告、压测期间 `gc.disable()`、基于 `tempfile.mkdtemp` 的清理机制，并将读取测试文件从 1GB 缩减至 100MB。

## [1.4.0] - 2026-05-27

### 变更
- **C++ 标准从 C++17 升级至 C++20**: 全面现代化代码库，利用 C++20 特性提升性能与安全性。最低编译器要求提升为 GCC 10+ / Clang 10+ / MSVC 19.28+（Visual Studio 2019 16.8+）。
- **`counting_semaphore` 优化 `close_impl()` 等待循环**: io_uring 后端的 `close_impl()` 原先使用指数退避的 `sleep_for()` 轮询（1ms 起步）等待 pending I/O 完成。替换为 `std::counting_semaphore::try_acquire_for()` — `complete_ok()`/`complete_error()` 在基类中自动释放信号量，最后一批 I/O 完成时 `close()` 立即唤醒，无需等待下一次 sleep 周期。最坏情况下 close 延迟从 ~1ms 降至 ~10μs。
- **`[[likely]]` / `[[unlikely]]` 分支预测提示**: 在关键热路径上添加 C++20 标准分支预测属性：`complete_ok()` 的 Read/Write 分支、`IOUringBackend::read()`/`write()` 的提前退出路径（已关闭、EOF、零大小）、`BufferPool::acquire()` 缓存命中、`HandlePool::acquire()` 缓存未命中。CPU 分支预测器在这些高频路径上不再预测错误，减少流水线冲刷。
- **`std::jthread` 线程生命周期管理**: `GlobalThreadPool` 与幽幽子异步日志引擎中的 `std::thread` 替换为 `std::jthread`。`jthread` 在析构时自动 RAII join，并内置 `std::stop_token` 支持，简化了关闭逻辑，消除了手动 `join()` 调用。
- **`std::span` 缓冲区传递**: io_uring 后端的 `submit_io()` 现接受 `std::span<const std::byte>` 替代分离的 `(const void*, size_t)` 参数。零开销抽象——编译器生成完全相同的机器码——但可在编译期消除缓冲区/长度不匹配的 bug。

### 修复
- **`global_thread_pool` 关闭竞态**: 将 `std::atomic<bool> m_stop` 替换为 `std::atomic<unsigned> m_running_workers`，消除了 `ensure_started()` 与 `shutdown()` 之间的 TOCTOU 竞态。
- **GCC 11 对 `[[unlikely]]` catch 子句编译失败**: GCC 11 不支持 `[[unlikely]]` 属性在 `catch` 子句上（GCC 12 起支持）。移除了 io_uring 后端 `catch (const std::runtime_error&)` 处的属性，恢复对 GCC 11 的兼容。
- **AppleClang 15 缺少 `std::jthread` / `<stop_token>`**: AppleClang 15（Xcode 15）附带的 libc++ 未实现 `<stop_token>` 和 `std::jthread`。在 `GlobalThreadPool` 和幽幽子异步日志引擎中添加 `__cpp_lib_jthread` 特性检测宏，特性不可用时自动回退到 `std::thread` + `std::atomic<bool>`。

### CI
- **升级 CI 运行镜像**: Linux `ubuntu-22.04` → `ubuntu-24.04`（GCC 11 → GCC 14），macOS `macos-14` → `macos-15`（AppleClang 15 → 16）。
- **纯文档变更跳过 CI**: 在 `test.yml` 的 push/PR 触发器中添加 `paths-ignore`，仅修改文档文件（`**/*.md`、LICENSE、.gitignore、MANIFEST.in）时跳过 Build and Test 流水线。`build_wheel_ci.yml` 和 `release.yml` 通过 `workflow_run` 链式触发 test 成功，因此会被自动一并跳过。

## [1.3.1] - 2026-05-22

### 重构
- **移除 `LoopHandle`，统一为 `ResultBatcher`**: 删除了 `LoopHandle` 类及其 `loop_handle.hpp`/`.cpp`（约 150 行）。`LoopHandle` 和 `ResultBatcher` 职责完全重叠——都是批量收集 future 结果、通过 `call_soon_threadsafe` 投递到 event loop。统一后三个非 Windows 后端（io_uring、macOS GCD、ThreadIO）均使用全局 batcher 注册表（`get_or_create_batcher`），`IORequest` 中的 `loop_handle` 字段改为 `batcher`。`ResultBatcher` 的 `push()` + `flush()` 模式替代了 `LoopHandle::push()` 的单次调度模式，天然继承阈值批量、空闲超时、失败重试和 `InvalidStateError` 静默抑制等所有优化。

### 修复
- **Windows Ctrl+C access violation 崩溃**: 控制台 handler (`ctrl_handler`) 从无 Python GIL 的 Windows 系统线程中调用了 `close_all_sessions()`。等待 IOCP pending 完成包的循环死锁——IOCP worker 需要 GIL 来处理 cancellation 包，但 GIL 被主线程（正在跑 event loop）持有。500ms 超时后在有 I/O 执行中的情况下调用 `CloseHandle` → 未定义行为 → access violation。修复为在 `CTRL_C_EVENT`/`CTRL_BREAK_EVENT` 时只设置 `trigger_ctrlc()` 标记并返回 `FALSE`，让 Python 自己的 console handler 正常抛出 `KeyboardInterrupt`，使清理流程走持有 GIL 的 `submit_close()` 路径（该路径自 v1.3.0 起已在 sleep 循环中正确释放 GIL）。
- **Windows Ctrl+C `InvalidStateError` 噪音**: asyncio 任务取消时，`Task.cancel()` 对内部 future 调用 `future.cancel()`，将其转为 `CANCELLED` 状态。IOCP worker 后续完成 I/O 尝试 `future.set_result()` 时抛出 `asyncio.exceptions.InvalidStateError`，被 `PyErr_Print()` 打印到 stderr。修复为在模块 globals 中缓存 `asyncio.exceptions.InvalidStateError`，并在 `ResultBatcher` 和 `LoopHandle` 的 drain 回调中静默吞掉——future 已取消意味着没有人在等结果，丢弃是正确的行为。
- **全平台 Ctrl+C `close_impl()` GIL 死锁**: io_uring、macOS GCD、ThreadIO 三个后端的 `close_impl()` 等待循环持 GIL 调用 `std::this_thread::sleep_for()`，导致 I/O 完成回调（io_uring reaper、GCD dispatch 回调、线程池 worker）无法通过 `PyGILState_Ensure()` 获取 GIL 来完成 `complete_ok/complete_error`。虽然 `pending` 计数器在 GIL 获取之前已原子递减，等待循环能正常退出，但完成回调一直阻塞在 GIL 上。最后一次 `close()` 时 io_uring 的 `UringInstance` 析构调用 `reaper_thread.join()` 时 reaper 仍阻塞在 `PyGILState_Ensure()` → 死锁。修复为三个后端均在 sleep 周围添加 `Py_BEGIN_ALLOW_THREADS`/`Py_END_ALLOW_THREADS`，与 v1.3.0 中 Windows `submit_close()` 的修复一致。
- **macOS GCD `close()` 后 fd 竞态导致 EBADF (errno 9)**: `dispatch_io_close(m_channel, DISPATCH_IO_STOP)` 是异步调用，系统的清理 handler 在稍后的 GCD 回调中才关闭底层 fd。如果在此期间 fd 号被后续的 `open()` 调用复用（POSIX fd 重用），GCD 清理回调会错误地关闭新文件的 fd，导致后续 I/O 返回 EBADF。修复为在 `dispatch_io_close` 之后通过 `dispatch_barrier_sync` 等待 GCD 队列排空，确保清理 handler 执行完毕后 `close_impl()` 才返回，之后 fd 号才可安全复用。barrier 等待期间通过 `Py_BEGIN_ALLOW_THREADS` 释放 GIL，避免 GCD 回调死锁。
- **Windows IOCP 混合读写偶发性 hang**: `ResultBatcher` 和 `LoopHandle` 的 drain callback 调度失败时（`call_soon_threadsafe` 返回 NULL），原逻辑会静默丢弃 batch 中所有待处理的 future 结果（`set_result`/`set_exception`），导致等待这些 future 的协程永久挂起。scene 4 同时激活 read/write 两个 IOCP worker，高并发下偶发 `call_soon_threadsafe` 失败 → 丢 batch → hang。修复为失败时不丢弃 batch，仅重置 `dispatch_pending` 标记，让下一次 `flush`/`push` 重试调度。

### CI
- **新增 Python 3.14t free-threading 构建**到 Linux、Windows、macOS 三个平台的测试矩阵中。

## [1.3.0] - 2026-05-21

### 修复
- **Windows IOCP 双重投递崩溃**: 极端并发下（约 50K 并发 readinto 操作）`GetQueuedCompletionStatusEx` 可能对同一个 `OVERLAPPED` 返回两次完成包，导致 use‑after‑free 和 `STATUS_HEAP_CORRUPTION`（0xC0000374）。在 `IORequest` 中增加 `IOState` 原子标记，`process_one` 入口处 CAS 检测并安全跳过重复完成包。（20 轮压测中 12 轮捕获到重复投递。）
- **同步 I/O 的 pending 计数器负溢出**: 同步完成的 pending 被双重递减（提交路径减一次 + IOCP worker 减一次），导致 `pending` 变为负数，`close()` 跳过 `CancelIoEx` + 等待。修复为移除提交路径的递减，统一由 worker 处理。
- **`submit_close` 中的 GIL 死锁**: Sleep 等待循环持有 GIL，阻止 IOCP worker 获取 GIL 处理已取消的完成包。在 `Sleep` 周围添加 `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS`。
- **Handle Pool 与 IOCP 关联冲突导致堆损坏**: `submit_close` 将已关联 IOCP 的 HANDLE 放回池中缓存，后续 `create_session` 重新获取该 HANDLE 并调用 `CreateIoCompletionPort` 更新 completion key 时失败（err=87 `ERROR_INVALID_PARAMETER`），错误路径中 `CloseHandle` 后内核复用同一个 HANDLE 值，导致连锁的 HANDLE 无效（err=6）和堆损坏 crash。修复为 IOCP 路径下 HANDLE 不再回池，直接 `CloseHandle`；新增 `handle_pool_evict()` 在 IOCP 关联失败时清空池中对应 key 的所有缓存 HANDLE。压测 500K 次 readinto（50K 并发）零 crash 通过。
- **yuyuko 内存追踪器集成与修复**: 项目内建的幽幽子（Yuyuko）内存生命周期追踪系统（`yuyuko_memlife.hpp`）集成到 IOCP 路径的 `IORequest` 分配/释放。修复了追踪器自身的多个问题：64 位地址空间下 `ShadowMemory` 数组越界（改为基于魂簿哈希表查询）、`TRACKED_NEW`/`TRACKED_DELETE` 宏未调用构造/析构函数（改为使用 `new`/`delete`）。新增 `ENABLE_ASAN` 编译选项的 MSVC 支持。yuyuko 在压测中捕获到 1 次 IOCP 双重投递导致的 use‑after‑free（被 `IOState` CAS guard 正确防御）。

### 变更
- **CMakeLists.txt 调试控制统一**: 用标准 `CMAKE_BUILD_TYPE`（Debug / Release / RelWithDebInfo）替代 `ENABLE_DEBUG` 选项。提取 MSVC 和 GCC/Clang 的公共编译选项，减少重复。
- **`LoopHandle` 全局缓存清理**: 增加 `clear_loop_handles()` 在模块 cleanup 时释放所有缓存的 `LoopHandle` 对象，修复重复 init/shutdown 周期下的内存泄漏。

## [1.2.0] - 2026-05-17

### 变更
- **IOCP 批量收割完成通知**: 将单次 `GetQueuedCompletionStatus` 替换为 `GetQueuedCompletionStatusEx`，采用双阶段策略：Phase 1 以单条目 + `INFINITE` 超时阻塞等待首个事件，Phase 2 非阻塞收割最多 `iocp_batch_size - 1` 个额外完成事件。高并发下显著减少 syscall 开销。
- **`iocp_batch_size` 配置**: 新增配置参数（默认 64，范围 1-256），控制每个 IOCP 工作线程循环收割的完成事件数量上限。可通过 `ayafileio.configure({"iocp_batch_size": N})` 配置，通过 `ayafileio.get_config()` 读取。Python 层 `AyafileioConfig` TypedDict 及 `configure()` 文档字符串同步更新。

## [1.1.6] - 2026-05-15

### 变更
- **`wrap_fd` → `wrap_file`**: 重命名以反映其现可接受文件描述符（`int`）和带 `fileno()` 方法的文件对象。新增 `is_real_file()` 校验，拒绝 socket、pipe、tty 等非普通文件。
- **`FileObj` 类型**: 在 `types.py` 中新增 `_HasFileno` Protocol，导出为 `FileObj`，表示任何具有 `fileno() -> int` 的对象。
- **`drain_handle_pool` / `drain_buffer_pool`**: 改为通过 `_compat.py` 的 Python 包装函数重新导出，而非直接引用 C++ 符号。
- **开发状态**: `pyproject.toml` 中从 "4 - Beta" 更新为 "5 - Production/Stable"。

### 修复
- **macOS EBADF write 错误**: `dispatch_data_create(DISPATCH_DATA_DESTRUCTOR_DEFAULT)` 和 `IORequest` 析构同时释放同一缓冲区，导致堆破坏和间歇性 EBADF。新增 `IORequest::release_buffer_ownership()` 将所有权正确转移给 `dispatch_data`。
- **`malloc`/`free` 不匹配**: `PoolBuf` 和 `make_req` 用 `new[]`/`delete[]` 分配缓冲区，但 `dispatch_data_create` 用 `free()` 释放。全部改为 `std::malloc`/`std::free`。

## [1.1.5] - 2026-05-11

### 新增
- **`auto_flush` 参数**: `open()` 和 `AsyncFile` 新增 `auto_flush` 关键字参数（默认 `False`）。设为 `True` 时，`__aexit__` 关闭文件前自动调用 `flush()`，确保缓冲数据在退出上下文管理器前写入磁盘。

## [1.1.4] - 2026-05-10

### 修复
- **macOS seek 后 read 返回空数据**：`seek()` 直接在原始 fd 上调用 `lseek()`，干扰了 Dispatch I/O 通道首次使用时的内部状态。移除 `lseek()` — macOS 后端使用 `DISPATCH_IO_RANDOM` 模式配显式 offset，fd 位置无关。
- **macOS `dispatch_io_read` 多回调数据丢失**：`dispatch_io_read` 可能分多次回调传递数据（首次带 `data` + `done=false`，再次 `NULL` + `done=true`）。现用 `__block` 变量跨回调累计字节数。
- **`malloc`/`free` 堆破坏**：`PoolBuf` 和 `make_req` 用 `new[]` 分配缓冲区，但 `dispatch_data_create(..., DISPATCH_DATA_DESTRUCTOR_DEFAULT)` 用 `free()` 释放，导致堆破坏。全部改为 `std::malloc`/`std::free`。
- **CI 测试步骤默认通过**：移除 `test.yml` 测试步骤中的 `continue-on-error: true`，测试失败现在正确导致 workflow 失败。Release workflow 通过 `workflow_run` 触发依赖 test workflow 成功。
- **CI 冗余调试日志**：测试构建从 Debug 改为 Release 模式，关闭 debug/verbose logging，减少日志噪音。

## [1.1.2.post1] - 2026-05-06

### 修复
- **POSIX `wb+` 模式回退**: `parse_mode()` 中 `if (mi.plus) flags = O_RDWR` 覆盖了 `O_CREAT` / `O_TRUNC` / `O_APPEND` 等标志，导致打开 `wb+`、`ab+` 等模式时抛出 `FileNotFoundError`。已在三个 POSIX 后端中使用 `(flags & ~O_ACCMODE) | O_RDWR` 修复。
- **异常消息中重复的 `[Errno N]`**: `throw_os_error()` / `set_os_error()` 手动拼接了 `[Errno %d]` 到消息中，但 Python 的 `OSError` 构造函数也会自动添加前缀 — 导致 `FileNotFoundError: [Errno 2] [Errno 2] Failed to open file: '/path'`。现改用正确的 2 参数/3 参数 `OSError` 构造函数形式。
- **Windows 上文件打开失败导致 atexit 段错误**: `WindowsIOBackend` 在构造函数中 `CreateFileW` / `CreateIoCompletionPort` 可能失败之前就将 `this` 插入了 `g_openFiles`。若构造抛出异常，析构函数不会运行，导致全局集合中留下悬空指针。atexit 清理 (`close_all_files`) 遍历该集合时崩溃。现已将插入推迟到所有可能失败的操作完成之后。

## [1.1.2] - 2026-05-06

### 变更
- **`ThreadIOBackend`: 全局共享线程池** — 工作线程现在全局创建一次，所有 `ThreadIOBackend` 实例共享，消除了每次 open 创建 ~16 个线程、每次 close 全部销毁的巨大开销。Linux/macOS fallback 路径上 open-close 循环性能提升数个数量级。
- **`WindowsIOBackend::close_impl()` 优化** — 移除非必要的 `SetFilePointerEx`（所有 I/O 均使用 `OVERLAPPED.Offset` 显式指定位置，文件指针无影响）。`m_pending == 0` 时跳过 `CancelIoEx`，常见场景每次 close 节省 1 个 syscall。
- **`handle_pool_release` 锁优化** — `CloseHandle` syscall 移到写锁之外，避免阻塞并发 `handle_pool_acquire`。用 `find()` 替代 `operator[]`，不再为不会被池化的句柄创建空 map 条目。
- **`handle_pool_release` 使用原子变量** — 池大小限制现从无锁原子变量（`g_hpMaxPerKey` / `g_hpMaxTotal`）读取，而非每次调用都走 `ConfigManager`（含 `shared_mutex` 锁）。
- **`BufferPool::release` 使用原子缓存** — `buffer_pool_max` 缓存到原子变量中，仅在 `configure()` 时刷新，避免每次释放缓冲区都读 `ConfigManager`。
- **`WindowsIOBackend` 非池化模式跳过 `make_pool_key`** — `CREATE_ALWAYS` / `CREATE_NEW` 模式（w/x 模式）不再计算句柄池 key，省去文件系统路径规范化开销。
- **`AsyncFile.readline()` 内部改用 `bytearray`** — 用 `bytearray.extend()` 替代 `bytes += bytes` 拼接，消除大文件逐行读取时的 O(n²) 内存分配。
- **`AsyncFile.writelines()` 批量写入** — 所有行先拼接再一次性 `write()`，不再逐行发起异步调用。

### 新增
- **`drain_handle_pool()` / `drain_buffer_pool()` 公开 API** — 新增运行时清空句柄池和缓冲区池的函数（适用于基准测试轮次间或大量临时文件操作后清理）。
- **`GlobalThreadPool` 单例** — 为 `ThreadIOBackend` 提供的跨平台共享线程池。新建 `src/global_thread_pool.hpp` / `.cpp`，全平台编译以确保一致的清理流程。

### 修复
- **`test_base.py`: 异步测试异常被静默吞掉** — `run_async()` 中原有的 `except Exception as e: ...` 不报告任何异常也不增加失败计数。现正确报告 `AssertionError` 和其他异常。
- **`_config.py`: `reset_config()` 触发 `NameError`** — `_CACHE_MAX_SIZE` 和 `_CACHE_ENABLED` 被声明为 `global` 但从未在模块级定义。现已补全。
- **`_compat.py`: 移除 `globals()` 反模式** — 将 `globals()["_io_worker_count"] = count` 替换为正常的模块级变量 + `global` 声明。
- **`util.py`: 裸 `except:` 替换为 `except Exception:`** — 三处裸 `except` 可能吞掉 `KeyboardInterrupt` 和 `SystemExit`。

### 测试与基准
- **`test_speed.py` 场景5（临时文件风暴）Windows 修复** — 场景5 之前在 Windows 上会卡死，根因有三：(a) 并发量过高（1000→200），(b) 每个文件打开两次（先写后读→现改为单次 `wb+`），(c) 僵尸句柄在池中跨轮积累（现每轮前清空句柄池）。场景5 现约 3s 完成。

## [1.1.1.post1] - 2026-05-02

### 移除
- **`handle_pool_posix.cpp`**: 删除了 POSIX 平台的句柄池空桩实现。POSIX 上 `open()`/`close()` 开销极低（微秒级），句柄缓存无实际收益。相关 API 保留为头文件内联空实现以维持跨平台兼容。

### 变更
- **`handle_pool.hpp`**: 新增 POSIX 平台的内联桩函数，替代已删除的 `handle_pool_posix.cpp`。
- **`CMakeLists.txt`**: 移除 macOS 和 Linux 构建中对 `handle_pool_posix.cpp` 的引用，减少编译文件数。

## [1.1.1] - 2026-05-01

### 新增
- **Python 3.14+ free-threading 支持**: 在 `nanobind_add_module()` 中添加 `FREE_THREADED` 标记，`ayafileio` 现可在 `python3.14t`（自由线程构建）中原生运行，无需 GIL。
- **CMake 构建增强**: `nanobind_add_module` 现在传入 `FREE_THREADED` 参数，自动处理 free-threaded 编译目标。
- **CI 矩阵扩展**: 新增 Python 3.14 free-threading 构建，覆盖 Windows / Linux / macOS 全平台。

### 测试验证
- **Windows (IOCP)**：45 个测试全部通过，场景 3 追加写入比 aiofiles 快 **3.06 倍**
- **Linux (io_uring)**：45 个测试全部通过，场景 5 临时文件风暴比 aiofiles 快 **1.92 倍**
- **macOS (Dispatch I/O)**：45 个测试全部通过，loguru 并发写入比线程池快 **2.1 倍**
- 所有测试在 free-threading 环境下稳定运行，无数据竞争，无 GIL 重入问题

### 变更
- `CMakeLists.txt`：为 `nanobind_add_module` 调用添加 `FREE_THREADED` 参数

### 打包说明
- 预编译 wheel 现已包含 Python 3.14t 版本
- 源码包保持不变，free-threading 支持在构建时启用

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