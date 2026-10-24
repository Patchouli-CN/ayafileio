# 更新日志

本文件记录项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)，
本项目遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

## [1.16.0] - 2026-10-24

> **版本策略**：`2.0.0` 保留给即将进行的**架构分层**，不用于发布功能。从 `2.0.0` 起严格执行
> semver（major = 不向后兼容的变更）。`1.x` 期间 minor 版本可能包含**已记录的行为变更**，
> 一律列在下面「升级须知」里。

### 升级须知（1.16.0）
- `wrap_file(fd, mode)` / `wrap_fd(fd, mode)` 现在严格按**声明的** mode 判定读写权限，与
  `io.FileIO(fd, "wb").read()` 的行为一致。用 `"wb"` 包装一个可读 fd 再 `read()` 会抛
  `io.UnsupportedOperation`——需要读就声明 `"w+b"`。（`io.UnsupportedOperation` 同时是
  `OSError` 与 `ValueError` 的子类，既有 `except OSError:` 写法继续有效。）
- 非法模式串现在抛 `ValueError`（`"rw"` / `"rbb"` / `"rr"` / `"rtb"` / `"r++"` 等）。注意 `"rw"`
  以前是被**静默按只读打开**的，完全看不出来。
- `seek()` 不再接受负位置，抛 `ValueError`（以前返回 `2^64-1` 并把它写进文件位置，之后每次读都
  报与原因无关的错）。这里刻意沿用 macOS 后端一贯的 `ValueError`，而不是内置的 `OSError(EINVAL)`。
- Windows 的 `fileno()` 现在返回**复制句柄**包装的 fd：关掉它不再影响文件对象，也不会再崩进程。
  若你以前靠 `os.close(f.fileno())` 关文件，请改用 `await f.close()`。


### 新增
- **`configure({"backend": ...})`：显式选择后端。** 取值 `auto`（默认，平台探测）/ `iocp` / `io_uring` / `dispatch_io` / `thread_pool`。只影响**之后**打开的句柄，已经打开的文件保持各自的后端；强制一个本构建/本机床上没有的后端会在 `configure()` 时抛 `ValueError`，而不是留到第一次 `open()` 才炸。`get_backend_info()` 与 `get_capabilities()` 现在都按**有效后端**回答，不再永远报平台默认值——「我现在到底在用哪个后端、是不是真异步」终于可回答了。顺带修正两处：Linux 上没有 liburing 的构建以前把 `platform` 报成 `"posix"`，现在是 `"linux"`（`platform` 管「在哪跑」，`backend` 管「用哪个」，两者不该混在一起）；`configure({"io_uring_sqpoll": True})` 的校验也从「内核支持吗」改成「**有效后端是 io_uring 吗**」——强制 thread_pool 之后这个选项不会生效，即使本机内核支持 SQPOLL，现在会直接 `ValueError` 而不是静默接受。
- **CI 覆盖线程池回退后端。** `ThreadIOBackend` 是 io_uring / GCD / IOCP 都不可用时的唯一路径，但正常 CI 里一行都跑不到（Linux 装了 liburing 就走 io_uring、macOS 走 GCD、Windows 走 IOCP）。新增两个 job：`test-fallback-backend`（Linux + macOS，通过 `AYAFILEIO_TEST_BACKEND=thread_pool` 让整套用例切到回退后端；`tests/conftest.py` 会在切换后核对生效，防止「配置静默无效」把整轮 CI 变成假绿）和 `test-no-io-uring-build`（Linux 刻意不装 liburing，`HAVE_IO_URING` 不成立，线程池是唯一后端——很多 manylinux 用户拿到的正是这样一份构建）。回归测试 `tests/test_backend_select.py`。

### 内部
- **`src/bindings.cpp` 回归纯绑定层。** 它原先除了 `m.def`/`py::class_` 还内联了 543 行里的约
  350 行业务代码：`py_configure` 的策略校验、`get_backend_info`/`get_capabilities` 的探测拼装、
  `open_async` 的线程池编排与引用计数契约、`cleanup` 的关停顺序、以及若干诊断入口。这些已按职责
  搬回**各自既有的模块**（没有新建目录/层）：`globals.cpp`（配置 API + 模块启动/关停）、
  `discover.cpp`（后端身份与能力）、`file_handle.cpp`（异步打开）、`result_batcher.cpp` 与
  `iocp_context.cpp`（诊断）。`bindings.cpp` 现在 168 行、4 个 include、零平台头文件。

### 修复
- **删掉 POSIX 三后端进程级的 `create_future` 缓存，并修掉一处每个文件都在泄漏的 loop 引用。**
  io_uring / macOS GCD / thread_io 曾各自缓存一个全局 `loop.create_future` **绑定方法**
  （`g_cachedFutureFn`），每文件 open 省一次属性查找（约 0.2 µs）。代价三条：绑定方法强引用 loop
  →「最后用过的那个 loop」永不可回收；`g_cachedLoop` 用**裸指针**做身份比较 → 新 loop 复用同一地址时
  会拿到死 loop 的方法；写侧加锁、读侧不加锁 → 多线程多 loop 下的引用计数竞争。都远贵过那 0.2 µs，
  缓存已删除，改为在 `ensure_loop_initialized` 里直接 `PyObject_GetAttr`（每文件一次，非热路径）。
  同处还修掉一个引用泄漏：`m_loop = loop; Py_INCREF(m_loop);` 之后 `get_running_loop()` 的那份引用
  从未归还——**每个 `FileHandle` 初始化都多钉一个 loop 引用**；现在直接接管那份所有权。
  功效：`test_event_loop_is_released_after_open` 从 Windows-only **解禁为全平台**。
- **`configure({"io_worker_count": N})` 在全局线程池启动之后是静默无效的。**
  `GlobalThreadPool::ensure_started()` 是 start-once——第一个调用者传的规模永久生效，
  之后调什么都没用，也没有 resize 接口。于是配置写进去了、`get_config()` 也报新值，
  但真正干活的还是老数量。实测（进程线程数采样）：配置从 2 改成 16 后，线程数全程
  钉在 9 不动。现在：worker 数公式收敛成一份 `effective_io_worker_count()`（原先在
  5 处各写一遍，谁先启动池谁说了算），池新增 `target_workers()`——多就补线程、少就让
  多余 worker 在取下一个任务前自行退出——`configure()` / `reset_config()` 末尾都会
  调用它。回归测试 `tests/test_pool_config.py` 断言「起 2 → 改 8 → 改 3 → reset 回
  auto」每一步的实时 worker 数。
  顺带修掉同一处的一个更严重的既有缺陷：jthread 变体的 worker 在**空闲 50 ms 后就 `break` 退出**，
  而 `m_running_workers` 不减——池在空闲之后名存实亡（计数还在、线程没了），这也正是缩容逻辑
  永远不执行的原因。现在空闲只是回到循环顶部重新评估退役条件，且任何退出路径都会配平计数。
- **Windows 上并发写 access violation：`OVERLAPPED` 用了 `hEvent = NULL`。（移植线程池后端时发现）**
  只用 ctypes 直调 Win32 就能复现：同一个 `FILE_FLAG_OVERLAPPED` 句柄上 16 线程各 300 次 `WriteFile`，
  `OVERLAPPED` 每次新建且 `hEvent = NULL` 时 **8 次跑崩 7 次**；给每个 `OVERLAPPED` 挂一个线程私有
  manual-reset 事件（每次提交前 `ResetEvent`）后 **0/8**；每线程复用一个 `OVERLAPPED` 也 0/8。
  原因：`hEvent == NULL` 时 `GetOverlappedResult(..., TRUE)` 是拿**文件句柄**当等待对象的，而对
  overlapped 句柄来说那个信号并不按 op 一对一，等待会提前返回；此时内核仍在往那块 OVERLAPPED
  （栈上、每次新建）里写完成状态，下一次调用覆盖同一地址 → 访问冲突。`native_file.hpp` 现在给
  `file_pread` / `file_pwrite` 都挂线程私有事件。
- **更正上一版的一处错误结论：`ThreadIOBackend` 的 `m_pending` 并没有失衡。**
  `IOBackendBase::complete_ok()` / `complete_error()` 开头就有 `m_pending.fetch_sub(1)` 与
  `m_close_wake.release()`，本来就是配平的。我上一版据此加的 `complete_pending()` 属于**双重递减**，
  会把计数打成负数、让 `close()` 误判「没有在飞的 I/O」而提前关句柄——反而制造了新的崩溃条件。该调用已撤回。
- **文件原语抽到 `src/utils/native_file.hpp`。** `ThreadIOBackend` 以前直接调 POSIX 的
  `open/pread/pwrite/fstat/fsync/ftruncate/fcntl`，因此只能在 POSIX 编译。现在这层统一成
  `file_open/file_pread/file_pwrite/file_size/file_fsync/file_truncate/file_close/file_is_append`：
  POSIX 侧直调系统调用（行为不变，错误码仍是 errno），Windows 侧用 `CreateFileW(FILE_FLAG_OVERLAPPED)`
  + 带 OVERLAPPED 偏移的 `ReadFile/WriteFile` + `GetOverlappedResult` 实现**定位** I/O（非 overlapped
  句柄会静默忽略偏移，多 worker 并发会互相踩）。
  **Windows 上也可以选它了**：`hEvent` 修好之后，库内并发写从约 1/8 崩溃降到 **0/30**
  （20 协程 × 20 次 append、16 worker），Windows + `thread_pool` 跑全套用例连续 3 次
  **226 passed / 4 skipped**。CI 的 `test-fallback-backend` 现在把 `windows-2022` 也纳入矩阵
  （共 31 个 job），三个平台的两个后端都被覆盖。
- **用错模式读写不再是一句与原因无关的 `OSError`，而是标准的 `io.UnsupportedOperation`。** 此前对只写文件（`"wb"`/`"ab"`）调 `read()`，或对只读文件（`"rb"`）调 `write()`，请求会真的发到 OS，然后拿回 `OSError: Errno 5`（Windows）或 `EBADF`（POSIX）——错误类型和消息都和真正的原因对不上。内置 `io` 在这种情况抛 `io.UnsupportedOperation("read"/"write"/"truncate")`，现在这里也一样。11 个入口加了守卫：`read`/`readline`/`readlines`/`read_at`/`read_many`/`readinto`/`chunk`（读）与 `write`/`write_at`/`write_many`/`truncate`（写）。`io.UnsupportedOperation` 的 MRO 同时含 `OSError` 和 `ValueError`，所以既有 `except OSError:` / `except ValueError:` 的写法全部继续有效。守卫直接判 `self._mode` 首字符（模式串不可变），热路径上没有引入方法调用，也没有新增实例属性。
  **注意一处行为变化**：`wrap_file(fd, mode)` 现在严格按**声明的** mode 判定，与 `io.FileIO(fd, "wb").read()` 抛 `UnsupportedOperation` 一致。用 `"wb"` 包装一个 O_RDWR 的 fd 再读，以前能过、现在会抛——`tests/test_wrap_file.py` 里就有两个这样的用例（包装后要读回来），已改成 `"w+b"`。
- **macOS 与 POSIX 线程池后端：`read()`/`read_at()` 看不到别的句柄追加的数据。** 这套 `m_cachedFileSize`（open 时快照，之后只由本会话的 write/truncate 更新）此前只有 Windows IOCP 后端做了「到 EOF 判决点就问内核」的刷新，另两个后端仍把旧长度当真相：外部追加后 `read()` 读到旧 EOF 就停、`read_at` 越界的一侧永远返回 `b""`、`seek(0, 2)` 报旧长度。现在两边都补齐（端点与 Windows 完全对应）：`read`/`read_at`/`readinto` 在 **EOF 判决点**和**发生短读**时 `fstat` 刷新，`seek(SEEK_END)` 前也刷一次；正常满载读一次 syscall 都不多加。macOS 的 mincore 内联快车道不受影响——刷新只让「超出原 EOF」的范围落到 dispatch_io/线程快速路，映射窗口本身不动。
  随之**解禁 5 条测试**：`tests/test_native_resources.py` 里的 4 条大小缓存用例（含 `readinto`）与 `fileno()` 稳定性用例不再 `WIN_ONLY`。现在的 gating 只剩**引擎语义**确实不同的三处：fileno 的复制句柄（2 条，Windows 专属语义）、以及 IOCP 专属的 quarantine（1 条）。
- **负偏移的 `seek()` 会把逻辑位置静默污染成 2^64-1。** `seek(-1, 0)` 返回 `18446744073709551615` 并把位置设成这个值；此后每次 `read()` 只报 `ReadFile failed, errno 87`（Windows）或直接读到 EOF（POSIX）——错误信息和真正原因毫无关系，位置也再回不来。Windows IOCP、io_uring、线程池三个后端都是裸 `static_cast<uint64_t>`，只有 macOS 的 GCD 后端早就有 `new_pos < 0` 校验。现在三个后端统一按 macOS 的语义拒绝（抛 `ValueError("negative seek position")`，且**不改动当前位置**）。刻意没有采用 stdlib 的 `OSError(EINVAL)`：macOS 后端一直是 `ValueError`，跨后端一致比逐字对齐内置更重要；这一处与内置的差异记在测试注释里。回归测试 `tests/test_io_parity.py`（5 个负偏移组合 +「报错后位置不变」+「报错后流仍可用」+「内部回退路径没被误杀」）。
- **非法模式串被静默接受。** `_normalize_mode` 只校验字符是否落在合法集合内，于是 `"rbb"` / `"rr"` / `"rw"` / `"rtb"` / `"r++"` 全部通过。其中 `"rw"` 最危险：挑起始模式字符时取第一个命中项，于是一个写着读写的模式实际按**只读**打开，调用方毫无察觉。现在补上与内置 `open()` 对齐的结构校验（恰好一个 `rwax`、至多一个 `+`/`b`/`t`、`b` 与 `t` 互斥），错误消息里带上模式串本身。
  另外记一笔**未修**的偏差：对只写模式（如 `"wb"`）调用 `read()` 时，内置抛 `io.UnsupportedOperation("read")`，这里会把请求发到 OS 拿到 `OSError`（Windows 上是 `errno 5`）。都是错误、只是类型不同；要对齐得在 Python 层给 8 个读写入口都加可访问性前置检查，留作独立改动。
- **Linux：跑过 io_uring I/O 的事件循环对象永远回收不了。** `UringInstance` 对 loop 做 `Py_INCREF`，而那个字段**从头到尾只写不读**——纯粹用来「给 loop 续命」，DECREF 只在 `cleanup_all()`（进程退出）里执行。于是任何一次 `aopen()`（Linux 原生 `OPENAT` 路径）或 io_uring I/O 都会把当前 loop 钉到进程退出。现在改成弱引用，并用它做缓存键的失效检测：`UringManager::acquire()` 的键是裸 `PyObject*`，旧 loop 已死、新 loop 复用同一地址时必须换新实例，否则新 loop 的完成结果会投给已经失效的 reaper/batcher（这是去掉强引用后才暴露出来的第二个问题）。`acquire()` 的失效路径、`remove()`、`cleanup_all()`、建实例失败路径统一走 `release_loop_ref()`。`weakref_target()` 抽到 `globals.hpp`，与 `ResultBatcher` 共用同一套 3.13 前后的版本分支。CI 由新增的 `test_loops_do_not_accumulate_across_loops` 守住：10 个不同的 loop 各做一次 open+aopen，存活 loop 增量必须 ≤2。
  （原先记在这里的 `g_cachedFutureFn` 有界滞留已在上面的修复条目里解决。）
- **做过一次 I/O 的事件循环对象永远回收不了。** `ResultBatcher` 按 loop 缓存（Windows 在 `IOCPContext::m_batchers`，其它后端在全局注册表），构造时缓存了 `loop.call_soon_threadsafe` 这个**绑定方法**——强引用，于是任何跑过一次 I/O 的 loop 都被钉到进程退出。`asyncio.run()` 包在每个请求里的代码（gunicorn 同步栈里很常见）等于每请求留下一个 loop 对象。现在 batcher 只持 loop 的**弱引用**，flush 时按弱引用取回（取不到就把这批结果丢掉并复位派发位——没有谁能再消费它们了，留着只会把 future 和它引用的 loop 一起压住）。缓存键仍是裸 `PyObject*`，所以 `get_batcher()` 会比对弱引用当前指向的对象：新 loop 复用了旧 loop 的地址时不匹配，旧 batcher **退役**（只退役不销毁——`Session::batcher` 与 `snapshot_batchers()` 可能仍握着这个裸指针，销毁即 UAF）并新建一个；不比对的话新 loop 的所有完成结果会被静默送进失效的旧 batcher。实测：200 次 `asyncio.run` + open + read 之后存活的 loop 对象从 +200 回到 0（跑完立刻统计时的 +200 是 quarantine 那 1 秒扣留，空闲 1.5 s 后归零）；随机 4 KiB 读 x16–x256 仍为 9.6–10.5x aiofiles，没有可测的性能回归（每次 flush 多一次属性查找，flush 是批量投递，不在每个 op 上）。
- **Windows：`fileno()` 每次调用泄漏一个 CRT 描述符，调用方 `os.close()` 会连累文件对象。** 原实现对会话自己的 HANDLE 做 `_open_osfhandle`：连续调三次返回 2003、2004、2005，前两个永远不释放；而那个描述符拥有的是**会话的**句柄，`os.close(f.fileno())` 之后 `await f.read()` 立刻报 `OSError: ReadFile failed`（errno 6）。现在复制一份句柄再包装，并且只创建一次（`io.FileIO.fileno()` 语义：同一个对象返回稳定的 fd）。关闭时两个坑都要防：一是调用方已经自己关掉了这个 fd，此时任何 CRT 校验函数（`_get_osfhandle`/`_close`）都会走 invalid-parameter handler 直接 `__fastfail`，进程 0xC0000409 —— 整段改在非致命的 thread-local handler 下跑；二是 fd 号可能已被后开的描述符复用，光判断「有效」会把别人的描述符当成自己的（实测会关掉 asyncio 的 self-pipe socketpair），因此比对的是**句柄身份**。回归测试 `tests/test_native_resources.py` 覆盖：fd 稳定、调用方 close 后文件仍可读、号码被复用时不误伤（子进程跑，真回归是整进程崩溃而非异常）。
- **每次 `open()` / `aopen()` 泄漏一个事件循环引用。** `WindowsIOBackend` 构造函数拿到 `get_running_loop()` 的 `+1` 却从不归还，注释把它说成 Session 的「续命来源」，而 `Session::loop` 同时被标注为 borrowed、`~Session` 明确不释放——那个引用就凭空消失了。所有权现在单向：`create_session()` 自行 `Py_INCREF`，`~Session` 释放；构造函数与 `open_async` 的回投 lambda 各自归还自己那一份（后者在 POSIX 上还多漏一个，因为注入参数本来就被忽略）。实测 200 次 open/close 后 loop 引用计数增长从 +200 降到 0。注意这**没有**解决事件循环对象的另一种滞留：`ResultBatcher` 按 loop 缓存并持有 `loop.call_soon_threadsafe` 绑定方法，任何做过一次 I/O 的 loop 都会被 `IOCPContext::m_batchers` 持有到进程退出（要做成弱引用 + 失效检测才能修，属独立改动）。
- **Windows：`read()`/`read_at()` 看不到别的句柄追加的数据。** 文件大小在 open 时快照，之后只由本会话的 write/truncate 更新，于是外部追加后 `seek(0)` + `read()` 只返回旧长度、`read_at` 越界的一侧永远返回 `b""`（对着正在被写的文件读就是静默少读；`read_at` 同样吃这个缓存值做 EOF 截断）。现在只在**已到 EOF 判决点**或**发生短读**时问一次内核，热路径 syscall 数不变，而长度变化在下一次操作就可见：外部追加的 7 字节，`read_at(10, -1)` 当场读得到、`seek(0, 2)` 报新长度。`readinto`、短写与写失败（提交前的乐观长度更新会高估）同样校正。macOS 的 dispatch_io 后端与 POSIX 线程池后端有同一套缓存，**尚未跟进**（需在对应平台上验证后再改）。
- **Windows：quarantine 只在有后续流量时才排空，最后一批完成的请求永久滞留。** 此前只在本线程 push 到 256 条、或 worker 收到一批完成包之后 drain，因此一批 I/O 跑完就空闲时，队列里那批 `IORequest`（连同它的 `future`，以及 `future` 里对事件循环的引用）会一直留着，直到下次有流量才释放——内存停在上次峰值上不回落。现在 worker 的等待超时把队首到期时间算进去，空闲时也会醒来 drain。实测 300 次读后空闲 1.6 s，`debug_quarantine_stats()['live']` 从 300 回到 0。

- **Windows IOCP 的 `flush()` 阻塞事件循环。** `submit_flush()` 在调用方线程（即事件循环）上直接执行 `FlushFileBuffers`：慢盘实测单次 40–50 ms，期间整个 loop 停摆，用 `asyncio.sleep(0)` 心跳测得最长 79.6 ms（空转基线 0.9 ms）。同一个调用在 io_uring 后端走 `IORING_OP_FSYNC`，本来就是真异步的，三个后端在这里行为不一致。现在 `FlushFileBuffers` 交给 `GlobalThreadPool`，结果经 `ResultBatcher` 回投——与 `aopen` 走同一条链路。记账沿用 `submit_write` 的规矩：提交时 `Session::pending` +1、`batcher->op_submitted()`，完成后 `complete_request()` 与 `op_completed()` 配平，所以 `close()` 仍会等这次 fsync 走完再关句柄（否则句柄被复用后 fsync 会刷到别的文件上）；任务捕获 `shared_ptr<Session>` 保证 session 存活，并在持 GIL 的状态下释放，避免「最后一个引用恰好在此析构」时 `~Session` 里的 `Py_XDECREF` 在裸线程上执行。顺序语义不变：`FlushFileBuffers` 覆盖此前已经交给驱动的写入，因此不额外等 `pending` 归零。同一块盘实测：flush 期间循环最长停摆 **79.6 ms → 0.8 ms**，单次 flush 耗时不变（23.9 → 25.0 ms，fsync 照做，只是不再占用循环）；`tests/` 153 passed / 1 skipped（Linux io_uring 用例），另做了并发与竞态验证（同句柄 40 并发、30 个文件并发、`flush` 与 `close` 竞态），每项之后 `debug_batcher_outstanding()` 均回到 0。本次只改 Windows IOCP：`thread_io_backend`（`src/backends/thread_io_backend.cpp`）与 `macos_gcd_backend`（`src/backends/macos_gcd_backend.cpp`）的 `flush()` 仍是内联 `fsync`，同样会阻塞调用方。

### 性能
- **open 不再每次取一次文件大小。** `create_session`（IOCP）与 `ThreadIOBackend` 构造原先无条件跑一次
  `GetFileSizeEx` / `fstat`，只为给读取路径的 EOF 截断提供长度；而长度可以「需要时再补」——EOF 判决点、
  短读、`seek(SEEK_END)` 都会刷新。改成惰性后（append 模式仍当场取，它必须知道起始位置），
  实测 `open()` p50 **41 µs → 38 µs**（i7-6700K / NTFS / 600 次）。适用面要说清：批量 open
  （`batch open`、`aopen only`）净省一次 syscall；「open + 一次 read」相当于把它挪到首读，中性。
  另外测明：`aopen()` 的 155 µs 与 `open()` 的 38 µs 之差不是代码浪费——`io_worker_count` 取 1/2/16
  实测都是 152–159 µs，瓶颈是池唤醒 + 2~3 次 GIL 交接 + 事件循环唤醒这几个**固定往返**。

## [1.15.0] - 2026-10-20

### 变更
- **路径参数改为遵循 PEP 519（`os.PathLike`），不再只认 `pathlib.Path`。** `open()`、`aopen()`、`AsyncFile()` / `AsyncFile.open()` 以及建立在它们之上的全部辅助函数（`read_bytes`、`read_text`、`read_bytes_many`、`write_text_many` 等）此前标注为 `str | Path`，并用 `str(path)` 归一化。这对 `pathlib.Path` 恰好可行（它的 `__str__` 就是路径），但对自定义 `os.PathLike` 实现会静默得到一串 repr 模样的"路径"，甚至 `open(12345)` 这种调用也会被字符串化后悄悄接受。现在所有入口统一标注 `str | os.PathLike[str]`，并通过 `os.fspath()` 归一化——自定义 path-like 对象正常工作，非路径类型则立即收到 `os.fspath` 抛出的清晰 `TypeError`，而不是之后一个莫名其妙的文件找不到错误。顺带清掉了 `_types.py` 里不再使用的 `pathlib.Path` 导入。

### 移除
- **Windows 句柄池整体拆除。** 它在原理上不可行：HANDLE 一旦关联 IOCP 就无法解除关联，回收的句柄再次 `CreateIoCompletionPort` 会失败（err=87），旧错误路径还级联成堆损坏（见下方 1.3.0 的修复记录）。实际上池早已是死代码——没有任何地方调用 `handle_pool_release`，每次 acquire 必然 miss。`src/handle_pool.{cpp,hpp}`、`poolKey` 相关管线、`drain_handle_pool()` 与 `set/get_handle_pool_limits` API 一并删除。兼容保留：`configure({"handle_pool_max_per_key"/"handle_pool_max_total": ...})` 被接受但静默忽略；`get_capabilities()['backend_detail']` 在 Windows 上不再上报 `handle_pool`。

## [1.14.0] - 2026-10-19

### 变更
- **文本模式 `write()` 改为返回字符数，不再返回字节数。** 以前返回的是编码（并换行翻译）之后的字节数——那是后端唯一知道的量——并且被文档写成与内置、与 `aiofiles` 的一处「有意差异」。现在对齐 `io.TextIOWrapper`：文本模式计**消耗的字符数**，二进制模式仍返回字节数，文本类整文件辅助函数同口径（`write_text()`、`write_text_many()` 数字符数，`*_bytes` 系列数字节数）。落盘结果不变，只是文本模式返回值的含义变了。迁移：依赖旧字节数的代码改用 `len(text.encode(encoding))`，或以二进制模式打开。短写路径会把真实写入的字节数映射回**完整的字符**（同时考虑 `newline` 翻译与编解码器），因此短写回报的是真正落到 OS 的字符数。`tests/test_base.py::test_write_read_text` 与 `tests/test_async_open.py::TestAsyncOpen::test_write_text_many` 已改为断言新契约（前者额外校验落盘字节数）；`tests/stress_single_file.py` 的追加写阶段也不再把所有已完成的写都记成部分写（`成功: 0 / 部分写入: 50,000` → `成功: 50,000 / 部分写入: 0`），也就是说它那个阶段原先恒为 0 的「成功」计数其实一直失效。

### 修复
- **Windows IOCP：延迟释放队列让持续小写吞吐塌陷。** 1.10.0 引入的 quarantine（每个完成的 `IORequest` 扣留 `QUARANTINE_MS` = 1 秒再释放，防止被重投递的 `OVERLAPPED` 命中复用地址）用 `std::vector` 实现，drain 时扫全表并对每个过期项 `erase(it)`。两半都是 O(n)：扫描每次都走完整个列表，`erase` 每次还要 memmove 整个尾部。稳态下列表深度 = 1 秒的流量，30k ops/s 就是约 3 万项，于是**每个完成的 op 都要检查几万个条目**（实测 `scanned/push` = 15,000–75,000），而且成本随吞吐上升——跑得越快付得越多。净效果：130 字节追加循环持续只跑 ~16k msg/s，每 op 多烧 2.3 倍 CPU（32.8 µs → 74.4 µs），而只要进程空闲一秒、列表排空就能跑回 ~58k msg/s。「空闲即复活」很容易被误读成写回压力，但 `GetProcessIoCounters` 显示写放大精确 1.0x、每轮工作量相同。列表按构造是 FIFO（`steady_clock` 时间戳单调不减），过期项必为前缀：改为 `std::deque` 从头弹出，摊销 O(1) 每项，保留 1 秒保留期与原有 CAS 双发保护不变。同一负载（10k×129 B / 5 文件 / 50 worker）实测：持续 16k → 84k msg/s（`scanned/push` 15k–75k → 1.0，500k 消息连续跑全程平坦），4 KiB 随机写 18.8k → 60.6k writes/s，写类负载从输给同门库约 2 倍变成反超。
- **`configure()` 在 loop 跑过 I/O 之后对批处理键静默无效。** `ResultBatcher` 在构造时缓存 `iocp_batch_size`、`adaptive_batch`、`adaptive_target_latency_us`，而提交/AOpen 路径每个 loop 只查一次 batcher，所以第一个异步操作之后再 `configure({...})` 只对之后新建的 event loop 生效——偏偏「基准测试起手配置」和「发现自适应批处理不对劲后关掉它」都发生在那一刻。现在 `configure()` 与 `reset_config()` 都会通过 `refresh_batchers_config()` 把当前值推进每一个存活的 batcher（上界夹紧，`current_threshold` / `current_idle_ms` 立即夹到新上界，调小立刻可见）。setter 只写 relaxed 原子量、不碰任何 Python 对象，因此「持 `m_batchersMtx` 调 `call_soon_threadsafe` 与 GIL 互等」那条既有死锁路径完全没被引入：指针在锁内拷贝、在锁外更新。

### 新增
- **`debug_quarantine_stats()`**（测试/调试辅助）暴露 quarantine 深度、push 次数、drain 检查过的条目数、drain 次数与深度峰值，让「摊销 O(1)」这条不变量可以被回归测试守住，而不是再被发现一次。
- **`debug_batcher_state()`**（测试/调试辅助）报告某个 loop 实际生效的批处理参数（`threshold`、`threshold_max`、`idle_ms`、`idle_max_ms`、`adaptive`、`target_latency_us`、`median_interval_us`、`outstanding`）。有了它，`configure()` 热更新才可验证——在此之前「这个开关到底生效没有」在 Python 侧无从回答。

### CI / 测试
- **测试框架统一到 `python -m pytest` 一条命令。** 仓库里此前并存三套：`unittest` 的 `TestCase`/`IsolatedAsyncioTestCase` 类（靠 `unittest discover` 跑）、自己写 `TestRunner` 打印对勾的脚本（`test_base.py`、`test_read_at.py`、`test_wrap_file.py`、`test_capabilities.py`）、以及 pytest——CI 为这三套各挂一步，而单独跑 `pytest tests` 连收集都活不下来。`pyproject.toml` 现在带 `[tool.pytest.ini_options]`：`testpaths`、`python_classes = ["Test*", "*Tests", "*TestCase"]`（历史上的 `AcopyTests` / `WrapperTests` / `FileLifecycleTests` 从来不匹配 pytest 默认的 `Test*`，这就是 `unittest discover` 一直在兜底的原因）、`asyncio_mode = "auto"` 配 session 级事件循环，外加 `dev` extra（pytest、pytest-asyncio、pyyaml、aiofiles、rich），CI 直接用 `pip install -e ".[dev]"` 安装，不再维护一份手写的 pip 列表。`tests/conftest.py` 把 `tests/` 放进 `sys.path`，让 `from i18n import i18n` 在 pytest 的 import 模式下也能解析；`tests/_console.py` 收拢了原先散在五个文件里的模块级 `sys.stdout = io.TextIOWrapper(...)`——那个替换会把 pytest 的 capture 对象从原 stream 上摘掉，整个会话死在 `ValueError: I/O operation on closed file`，这正是以前 `pytest tests` 报「no tests ran」的原因。现在它只在 `__main__` 下执行且就地 `reconfigure`，脚本模式照旧打印报告，pytest 侧 capture 正常。`tests/test_loguru.py` 不再是两个带位置参数的 `test_*(LOG_COUNT, CONCURRENT, NUM_FILES)`（pytest 把它们当成缺失的 fixture，旧矩阵里那两个 `EE` 就是它），改成两个零参数用例断言落盘字节数，吞吐对比留给脚本模式，Linux 的 `sqpoll` 调优也不再在 import 时改全局配置（否则会传染其它用例）。Windows 上整套 **148 项通过、1 项跳过**（Linux 专属 io_uring 用例），约 5 秒——对比旧 CI 矩阵里十一条独立调用。另外 pytest 顺带捡回了 12 个只存在于 `test_base.py`、却从未被它脚本 runner 挂上的文本模式 read/seek 回归用例（`test_text_read_*`、`test_*_seek_*`），全部通过——没有统一 harness 的话，它们会一直静默腐烂。新增的 `tests/test_quarantine.py` 把本次两个「静默劣化」bug 钉成回归：对着旧的扫描式 drain 会失败（2 万次完成扫了 2.08 亿条），并通过 `debug_batcher_state()` 断言 `configure()` 确实推进到已存在的 batcher。它还顺带记下一条 CI 教给我们的事实：当 `SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)` 生效时，缓存命中的小写就在 `submit_write` 里内联完成、当场释放请求，内核不投完成包，于是 quarantine 根本不会填满——`windows-2022` 上 2 万次小追加只有 59 次（约 0.3%）进队，本机则是 100%。这在构造上是安全的（没投过的完成包不可能双发），但摊销性质在那种 runner 上无从观测，所以那两个用例改为跳过并在原因里带上比例，而不是假装通过。`tests/test_loguru.py` 的线程池对照臂也改用二进制追加：`TextIOWrapper.write` 在 3.11 之前没有自己的锁，32 个线程同时追加到同一个文本句柄会丢数据（3.10 的 CI 上 1,290,000 字节只落了 1,162,161）。
- **CI 全流程切到 UTF-8（`PYTHONUTF8=1`、`PYTHONIOENCODING=utf-8`）。** 测试文件里那段换 `sys.stdout` 的 hack 之所以存在，是因为 CI 的 stdout 是**管道不是控制台**：此时 Python 按 locale 代码页编码——英文 `windows-2022` runner 上是 `cp1252`，部分 Linux 镜像是 `C`/ASCII——于是第一句中文 `print()` 就 `UnicodeEncodeError`；而那个绕过手段（`sys.stdout = io.TextIOWrapper(...)`）恰好就是废掉 pytest capture 的元凶。PEP 540 的 UTF-8 模式在上一层解决：workflow 级设置这两个变量后整个进程都是 UTF-8，中文断言消息在**失败时**（这才是最要紧的场景）也不会把 runner 打挂，测试文件不再需要碰 `sys.stdout`。`tests/_console.py` 保留脚本模式的控制台切换，供开发机在 `cp936` 控制台下直接跑，而且用的是保留原对象的 `sys.stdout.reconfigure()`，不替换 stream。

## [1.13.0] - 2026-10-18

### 新增
- **Linux 原生 `aopen`（io_uring `OPENAT`）。** Linux 的 io_uring 可用时，`aopen` 现在直接向每个事件循环共享的 ring 提交 `IORING_OP_OPENAT`，CQE 返回 fd 后构造 `AsyncFile`，不再为每次打开创建独立本地 ring 并同步 `io_uring_wait_cqe`，也不再把打开任务交给用户态 C++ 线程池。共享 SQ 溢出补提交、reaper、inline harvest 和 ResultBatcher 均复用现有 I/O 基础设施；ring 或请求初始化失败时保留原 C++ 线程池回退，真实打开错误（如 `ENOENT`/`EACCES`）直接投递。Windows 与 macOS 行为不变。
- **后端解耦的原生打开架构。** `PyAsyncFile` 移至 `file_handle.hpp`，原生提交与完成逻辑归入 `io_uring_backend`，`file_handle` 负责平台分发，`bindings.cpp` 仅保留通用注册与调用。完成路径明确处理 path/mode/Future 生命周期、fd 接管与异常所有权，避免静默回退、重复 batcher 配平和异常双关。

### CI / 测试
- `tests/test_async_open.py` 改由 pytest 接入三平台测试流程，并计入 CI 退出码。新增 `test_linux_native_aopen_submission`：通过 `debug_native_aopen_count` 断言 `aopen` 确实走了 io_uring 原生提交，而不是静默回退；Linux CI 实测 20 项全部通过，Windows/macOS 的对应平台专属用例按预期跳过。

## [1.12.1] - 2026-10-17

### 变更
- **Windows IOCP 的 `submit_close()` 改为完成信号唤醒，不再 Sleep 轮询。** `pending == 0` 的快速关闭、`CancelIoEx`、GIL 释放、关闭超时和 Session 生命周期语义保持不变；在飞请求改由每个 Session 的计数信号量唤醒 close，同步完成、异步完成、inline 完成和提交失败路径都会发出通知。每次唤醒都会重新检查 pending，早到或冗余信号不会导致提前关闭句柄，同时消除了指数退避轮询带来的等待延迟。

### CI / 测试
- `tests/test_base.py` 新增 128 个在飞 `read_at` 请求存在时关闭文件的回归场景，验证取消与排空、成功读取内容、任务收尾和 closed 状态。

## [1.12.0] - 2026-10-10

### 新增
- **运行时能力发现：`ayafileio.get_capabilities()`。** 返回 `get_backend_info()` 的全部内容，外加两个字典：`features`（库级能力，键跨平台统一：`positional_io` / `batch_positional_io` / `zero_copy_readinto` / `chunked_streaming` / `async_open` / `adaptive_batching` / `fast_file_copy`）和 `backend_detail`（当前后端在此机器上探测到的原生能力）。探测原则是**问内核，不猜版本号**：io_uring 的 opcode 支持走 `IORING_REGISTER_PROBE` 逐个实测（read/write/statx/renameat/mkdirat 等 21 项），SQPOLL 直接试建一个 ring 验证（5.11 之前需要特权，版本号说了不算），特性位读 `ring.features`——backport 内核报的也是真实能力。Windows 上报 `completion_model` / `batch_harvest` / `handle_pool`；macOS 诚实标注 `completion_model: false`（GCD 文件通道由内核托管 workqueue 线程执行阻塞 pread/pwrite，非提交-完成模型）并保留 `mincore_fastpath` 标记。探测逻辑独立为 `src/discover.{h,cpp}`，结果进程内缓存，重复调用零开销。
- **`get_backend_info()` 新增 `os_version` 键。** Windows 走 `RtlGetVersion`（不受 manifest 兼容模式欺骗），POSIX 走 `uname(2)`。纯新增，向后兼容。
- **能力校验的特性开关。** `configure({"io_uring_sqpoll": True})` 现在按探测到的内核能力校验：内核不支持 SQPOLL、io_uring 后端不可用、或非 Linux 平台时当场抛 `ValueError` 并指引先查 `get_capabilities()`——不再静默接受一个建 ring 时才失败的配置。设为 `False` 无条件允许，跨平台代码可以放心写。SQPOLL 探测抽为缓存的 `io_uring_sqpoll_supported()`，能力矩阵与 configure 校验共用同一次试建，不重复建 ring。
- **字典返回值的 TypedDict 注解。** 新增 `BackendInfo`（`platform`/`backend` 为 Literal）、`Capabilities`（继承 BackendInfo，含 `LibraryFeatures` 与各后端的 detail TypedDict 联合）、`AyafileioConfig` 顶层导出——`py.typed` 包的用户现在在 IDE 里对能力矩阵有完整补全。

### 变更
- **`warn_fake_async()` 改为读取扩展的权威探测结论。** 原实现跑在 import 时、扩展加载之前，只能靠 ctypes 加载 liburing、解析内核版本字符串、检查头文件路径三重猜测——且可能与真实后端不一致（容器 seccomp 禁掉 io_uring syscall 时，ctypes 说"有"、建 ring 却失败）。现在调用点挪到扩展加载之后，直接读 `get_backend_info()` 的 `is_truly_async`（Linux 上是真的试建过 ring 的结论），警告消息附带平台、内核版本与 `get_capabilities()` 自查指引。删除约 100 行猜测代码。

### CI / 测试
- `tests/test_capabilities.py`（26 项检查，含平台分支断言与缓存一致性）接入三平台测试流程；"Check backend info" 步骤现在输出完整能力矩阵，CI 日志兼职内核能力探测器。
- 修复 CI 抓到的两处问题：io_uring opcode 探测改用 uapi ABI 编号（`IORING_OP_*` 是枚举常量而非宏，`#ifdef` 全部静默为假导致矩阵为空——编号已对照内核 `include/uapi/linux/io_uring.h` 核实，`io_uring_opcode_supported` 自带 `op > last_op` 边界检查，编译不再依赖 liburing 头文件年代）；Windows 上 `_winapi.CopyFile2` 为 Python 3.12+ 才有，3.10/3.11 的 `fast_file_copy` 现在与 `acopy` 的实际行为一致地报 `False`。

## [1.11.3] - 2026-10-08

### 修复
- **Debian 12 / GCC 12 下源码编译失败：`error: 'atomic' in namespace 'ayafileio::std' does not name a template type`。** `src/globals.hpp` 的 POSIX 分支把 `#include <cstring>` 写在了 `namespace ayafileio` **里面**——在命名空间内引入标准库头文件会把其中的声明一并注入该命名空间，GCC 12 的 libstdc++ 因此凭空多出 `ayafileio::std`，其后所有 `std::` 引用全部解析错误、编译中止。更新的工具链（GCC 13+、clang、MSVC）恰好容忍这种写法，所以 CI 与预编译 wheel 从未察觉；而在 Debian 12 自带工具链上从源码构建的用户会立刻踩中。`<cstring>` 现移至命名空间外的全局 include 区。预编译 wheel 用户不受影响。本项由 [@N0zoM1z0](https://github.com/N0zoM1z0) **报告并修复**——issue（[#5](https://github.com/Patchouli-CN/ayafileio/issues/5)）环境、复现步骤、根因分析俱全，随后直接提交补丁（[#6](https://github.com/Patchouli-CN/ayafileio/pull/6)），并在 Debian 12 / GCC 12.2.0 上实测：Release 构建通过，io_uring 开/关两种配置下 139 项检查全绿。项目收到的首个外部代码贡献——感谢！

## [1.11.2] - 2026-10-06

### 变更
- **`ayafileio.types` 更名为 `ayafileio._types`。** 原名遮蔽了标准库 `types` 模块——包内绝对引用无碍，但源码目录一旦进入 `sys.path` 就是隐患。公共导入路径不变：`from ayafileio import AyaFileIO` 照旧。直接导入过内部模块（`from ayafileio.types import ...`）的代码请切到公共路径。（社区反馈）

### 修复
- **Python 3.10 运行时不再需要 `typing_extensions`。** `types.py` 的 `Self` 与 `_config.py` 的 `TypedDict`/`NotRequired` 在 3.10 上依赖 `typing_extensions`，但包从未声明该依赖——全新安装的 3.10 环境 import 即失败。`Self` 移入 `TYPE_CHECKING` 并配合惰性注解（运行时永不求值），`TypedDict` 改用标准库（3.8+ 自带），`NotRequired` 包装移除（该 TypedDict 本已 `total=False`）。（社区反馈）
- **macOS：wrap/close 周期可能误杀无关文件的描述符——新 `open()` 的文件首次 write 即 `OSError: [Errno 9]`（EBADF）。** 1.11.1 的泄漏修复在 `close_impl` 里于 channel 队列的 `dispatch_barrier_sync` 之后、先用 `fcntl(F_GETFD)` 探测再关闭那个 `dup()` 出来的 fd。问题在于 barrier 只能对*已入队*的块建立顺序，而 channel 的 cleanup handler 要经过多级异步链路才落到该队列（`dispatch_io_close` → channel 内部队列 → barrier queue → `close_queue` 恢复 → trampoline → 我们的队列）：`close_impl` 可能在 libdispatch teardown 仍在飞时就探测并关闭了 dup。过早关闭两个方向都错：fd 还带着 libdispatch 的 `GUARD_CLOSE` 守卫时，裸 `close()` 直接 `EPERM` 失败——本要回收的 fd 被静默重新泄漏；若关闭成功，fd 号在 teardown 仍引用它时被还给内核，teardown 迟到的操作就落在复用该号的新文件头上。探测对此无能为力——`fcntl` 回答的是"号还有效吗"，不是"所有权还归我们吗"（教科书 ABA）——CI 红绿纯由 teardown 时序决定，两次纯 Python 改动在同一 wrap-then-open 序列上翻红的原因即在此。dup 现在由 channel 的 cleanup handler 独占关闭、恰好一次：那是 `dispatch_io_create(3)` 契约明确交还 fd 控制权的时点（"safe for the application to `close(2)`"），且被调度在 libdispatch 自己的 fd_entry teardown（解除守卫、全局 fd 表摘除）完成之后——无需探测，也不会踩守卫。`close_impl` 改为 release channel 后等待 handler 信号量，保持"`close()` 返回时 fd 号真正回收"的旧保证；`close_timeout_ms` 超时兜底时 handler 仍会迟到关闭（block 捕获的 `shared_ptr` 让状态活得过后端析构）——迟到但仍恰好一次：无双关、无泄漏。回归覆盖：`tests/test_wrap_file.py` 新增 100 次 wrap → close → `os.close` → 立即重开写入的循环，每轮都复用刚释放的 fd 号。

### 文档
- README 双语措辞收紧：技术叙述的口语化表达改为精确措辞，射命丸文签名元素保留。补充英文仓库简介。

## [1.11.1] - 2026-10-03

### 修复
- **macOS：每次 open/close 泄漏一个文件描述符。** Dispatch I/O channel 由一个 `dup()` 出来的 fd 创建，但这个号码只存在于构造函数局部变量里，`close_impl` 也只是 `dispatch_io_close()` 后把指针置空——channel 的 +1 引用从未 `dispatch_release`，dup 出来的 fd 也无人关闭。按 `dispatch_io` 的契约，系统只是把 fd 的**控制权交还**应用，关闭它永远是应用的职责。于是每个 open/close 周期永久泄漏一个 fd。CI 新增的批量打开场景把它抖了出来：约 2,600 次开关后 `EMFILE`；对用户则是长跑进程缓慢走向 "too many open files"。现在后端会记住这个 dup、在 cleanup barrier 跑完后 release channel，并**自己关闭**该 fd（先探测再关，避免对已关过的实现双关误杀复用号）。macos-15 CI 实测连带收益：此前批处理场景跑到一半就 `EMFILE`，现在 1024 全量预算飞完；要开文件的格子也整体变好——4 KiB 小读修复后 499K ops/s（对 aiofiles 27.2x）、随机 4 KiB x1 432K（54.2x），而修复前同格子历史区间为 187K–416K；这段跨度里有一部分是共享 runner 的轮间抖动，泄漏本身的税只是其中可复现的部分。
- **`aopen` 错误路径：并发失败时返回 future 的 use-after-free。** `open_async` 用一份自有引用创建 future，随后把*同一份*引用交给了两个主人：线程池 worker 收工時 `Py_DECREF` 它，绑定层又用 `py::steal` 把它返回给 Python。通常情况下 batcher 里绑定的 `set_result`/`set_exception` 方法临时持有的那份引用恰好盖住窗口，相安无事；但当大量 open 同时失败时（最后一个在飞 op 触发 batcher 立即 flush），future 可能在 await 方仍持有时就被释放——macOS 批量打开 CI 场景实测为 `OSError: EMFILE` 之后接 `RuntimeError: Future object is not initialized.` 再 SIGSEGV（await 协程读到已释放的 Future 对象）。现 worker 持有自己的那一份引用，恢复"worker 里每个 `Py_DECREF` 都配平它自己拥有的引用"这一不变量。（同批复查了兄弟捕获——`set_result`、`create_future`——所有权均正确。）

### CI / 测试
- `tests/t_compare.py` 新增两个批量打开场景：**F. Batch open**（每批 1024 个 64B 小文件、全部并发打开——`read_bytes_many` vs `gather(read_bytes)` vs aiofiles 默认的 `gather(open+read+close)`）与 **G. Batch open, opens only**（只开不读：`aopen` vs aiofiles 开+关，每个文件两次 executor 往返）。Windows 句柄池每轮 drain 一次保证冷启；轮内重复的批次复用缓存句柄，即两个库的开箱行为。POSIX 上先把 `RLIMIT_NOFILE` 软限制抬到硬限制（macOS 默认 256——这个批量的文件数直接爆；首次 CI 跑 macOS 时第 571 个文件就 `EMFILE`，因为 macOS 每个 open 要占*两个* fd——后端给 dispatch channel dup 了一份——且关闭的 fd 异步回收，真实用量以数量级超过信号量给出的并发上限）——何况任何"数 fd"的模型在 macOS 上都是错的：XNU 把 `RLIMIT_NOFILE` 设多少就存多少、开文件时才按 `MIN(RLIMIT_NOFILE, kern.maxfilesperproc)` 裁剪（GitHub 的 macOS runner 就在 launchd 默认且不可抬升的 256 上），`getrlimit` 必然虚报。批量大小现在用*并发*批次翻倍探测，直到库自己拒绝——顺序探针看不见这个预算，因为信号量加回收 lag 让峰值用量成为在飞数的数倍；有效批量数记入场景元数据。
- 首批实测（files/s 中位数，对 aiofiles）：`read_bytes_many` 全平台胜——作者 Windows/NVMe 机器 **2.1x**、windows-2022 CI **3.0x**、ubuntu-24.04 CI **2.4x**、macos-15 CI **2.9x**。`gather(read_bytes)` 归因行是微妙的那行：open 贵的地方它塌到 ≈1.0x（本地 NVMe）——同步 open 串行在 loop 线程上时，光靠异步读打不过线程池，那份领先当归 `aopen`；open 便宜的地方（CI VM、缓存存储）它照样赢（windows-2022 **2.7x**、ubuntu-24.04 **3.1x**、macos-15 **3.6x**）。批量的胜势是稳健的，`aopen` 的*边际*价值则跟随单 open 成本起伏。只开不读（G）：`aopen` 本地 **2.5x**、windows-2022 **3.3x**、ubuntu-24.04 CI **2.1x**、macos-15 CI **4.3x**。

## [1.11.0] - 2026-10-01

### 新增
- **异步打开：`ayafileio.aopen()`。** 参数、模式语义、返回的 `AsyncFile` 与 `ayafileio.open()` 完全一致，但 OS open 本身跑在 C++ 全局线程池：工作线程持 GIL 构造后端，构造函数内部的阻塞系统调用（`CreateFileW` / `open` / `openat` 等待 / `mmap`）经新增的 `GilRelease` 守卫释放 GIL，多个 worker 因此真正并行打开文件。结果（可直接使用的 `AsyncFile` 或异常对象）经 `ResultBatcher` 投递，与 I/O 完成共用同一条批量唤醒链路。返回惰性句柄（aiofiles 同款），`async with ayafileio.aopen(...) as f` 与 `await ayafileio.aopen(...)` 两种写法等价。Windows 构造所需的 `loop`/`create_future` 在调用方的事件循环线程预取注入（工作线程上没有 running loop）；POSIX 后端首次 I/O 时才惰性绑定事件循环，忽略注入。同步 `ayafileio.open()` 路径同样受益：open 系统调用期间不再冻结整个解释器。
- **批量整文件操作：`read_bytes_many()` / `read_text_many()` / `write_bytes_many()` / `write_text_many()`。** 一次调用并发读取或写入多个文件——打开走 `aopen`（OS open 并行），I/O 走各平台异步后端，信号量（`max_concurrency`，默认 64）限制在飞文件数，十万级输入也不会打爆 fd。写系列接收 `(path, data)` 对，返回逐文件字节数。返回顺序与输入一致；任一失败整体抛出（`asyncio.gather` 默认语义——批量不是事务，写批量失败时部分文件可能已落盘）。把"正确写法"（对文件 gather）变成最短的写法。
- **类型检查支持：`py.typed` 标记 + `open()` / `aopen()` 的 `@overload` 重载对。** 包内随附 `ayafileio/py.typed`（wheel 与 sdist 都有），mypy/pyright 才会真正加载注解——没有这个标记，类型检查器会整个跳过已安装的包，所有重载对用户都不可见。`aopen()` 与 `open()` 保持一致的重载对（文本模式 → `AsyncFile[str]`，二进制 → `AsyncFile[bytes]`）。
- **双语使用指南：`docs/USAGE.md` / `docs/USAGE_CN.md`。** 日常参考：`open()` vs `aopen()`、定位与批量 I/O、整文件与批量操作、`chunk()` / `readinto()`、`acopy()`、配置表、最佳实践、aiofiles 迁移说明、类型检查。两个 README 均已链接。

### 修复
- **Windows：同步完成快路径的在飞 op 记账泄漏。** IOCP 提交路径上每个 `op_submitted()` 都必须由 `op_completed()` 配平，但当 `ReadFile`/`WriteFile` 在 `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` 启用下同步完成时，提交线程就地 resolve 却从未配平——每个同步完成的 op 泄漏一次计数，永不愈合。`m_outstanding` 持续虚高后，"最后一个在飞 op 完成立即 flush" 优化对该 loop 永久失效，所有异步完成的 op 退化为等 ~5ms 空闲清扫。同步完成是否发生取决于存储栈：裸机 Win10 几乎不走此路径（本地不可见），CI 的 windows-2022 VM 会走——实测后果：x1 并发随机 4 KiB 读只有 **144 ops/s**（≈7ms/op），而同 runner 上 aiofiles 为 4,122。四个 submit 点现已在 skipCP 分支经 `complete_tracked_op()` 配平（计数归零且有滞留结果时顺手 flush）。回归覆盖：`tests/test_batcher_accounting.py` 经新增的 `_ayafileio.debug_batcher_outstanding()` 探针断言每 loop 在飞记账归零（仅在同步完成路径会触发的机器上有判别力）。修复后同一 runner 上该格子实测 **12,012 ops/s（对 aiofiles 1.63x）**。

### CI / 测试
- 新增 `tests/test_async_open.py`（aopen 二进制/文本/写模式 round-trip、与 `open()` 一致的模式校验、异常经 future 投递、并发打开、批量读顺序、限流、错误传播）。

## [1.10.0] - 2026-09-29

### 性能
- **macOS：大读并行分块填充。** ≥ 1 MiB 的读不再由单个 worker 线程顺序 `pread`，而是拆成 512 KiB 的块一次性全部提交全局线程池，各 worker 并发 `pread` 进同一预建缓冲（位置写、零协调），最后一块完成时聚合收尾——整请求仍是一次完成，批量器记账不变。单线程顺序读的页缓存拷贝是瓶颈（turbofile 在自有 Apple Silicon 上实测 64 MiB：并行 2.7ms vs 单线程 8.9ms），摊到多个 worker 后逼近内存带宽。任一块失败即整请求失败（部分数据不可交付，与流水线路径语义一致）；分块数封顶 256，巨型文件自动放大块大小。Linux io_uring（20 GB/s 已带宽饱和）与 Windows IOCP（单完成结构）路径不变。
- **实测说明（诚实记账）**：macOS-15 CI runner（共享机型）上前后对比**未测出稳定收益**——1 MiB / 4 MiB 顺序读落在 ±30% 的 runner 抖动带内（低于并行阈值的 64 KiB 格同代码也抖 0.71x，可作噪声标尺）。机制正确性经 `tests/test_parallel_read.py` 逐字节验证；预期收益场景是内存带宽吃紧的机器。在自有硬件上测出可复现的量级前，此项不写进 README 性能表。

### 新增
- **整文件操作 API：`ayafileio.read_bytes()` / `write_bytes()` / `read_text()` / `write_text()`。** 一次 await 完成 open + 读写 + close，面向 `asyncio.gather` 批量处理多文件的形态（模型权重按层加载、配置批量读取等）。语义与手动 `async with ayafileio.open(...)` 逐行写法完全一致，只是省掉样板。

### CI / 测试
- 新增 `tests/test_parallel_read.py`（并行分块数据逐字节校验、短尾块、`read_at` 交叉、`readinto` 大缓冲、整文件 API round-trip、gather 批量读），三平台测试矩阵接入。

## [1.9.0] - 2026-09-29

### 性能
- **macOS：小读 mincore 内联快车道。** dispatch_io 每个 op 都要过队列交付（handler hop + 唤醒），缓存命中的小读（< 256 KiB，与 `LARGE_IO_THRESHOLD` 同界）改为内联完成：open 时 mmap 一份只读映射，读前 `mincore` 判定整个请求范围常驻页缓存，全命中即在调用线程（持 GIL）直接 `pread` 进预建 PyBytes，经 `complete_inline` 即刻 resolve——`await` 不再让出事件循环。这是 macOS 侧对应 Windows `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`（1.7.0）与 io_uring `COOP_TASKRUN` 内联收割（1.8.0）的热路径机制，三平台自此补齐。冷页 / 无映射 / 超出 open 时映射窗口（文件后续增长的区域）一律落回 dispatch_io 异步路径，语义不变；映射只供 mincore 探测、从不解引用，无 SIGBUS 风险；`mincore` 与 `pread` 之间页被换出的竞态由预读窗口自然兜底（最坏退化一次同步读，EINTR 透明重试）。
- **实测**（macOS-15 CI runner，Python 3.14，`tests/t_compare.py` 口径，256 MiB 缓存热文件；“旧”为 `cb8a99f` 上的同口径基线）：

  | 场景 | 旧代码 | 新代码 | 对 aiofiles |
  | --- | ---: | ---: | ---: |
  | 4 KiB 顺序读 | 6,751 ops/s（0.67x） | 416,318 ops/s（**61.7x**） | **24.3x** |
  | 4 KiB 随机读 x1 | 4,575 ops/s | 289,251 ops/s（**63.2x**） | **37.7x** |
  | 4 KiB 随机读 x16 | 32,706 ops/s | 304,136 ops/s（9.3x） | 20.9x |
  | 4 KiB 随机读 x64 | 37,765 ops/s | 311,607 ops/s（8.3x） | 15.4x |
  | 64 KiB 顺序读 | 384 MB/s（0.63x） | 7,474 MB/s（**19.5x**） | **7.3x** |

  写路径不经快车道，64 KiB 顺序写 1.02x 持平（无回归）；256 KiB 以上的读/写沿用既有双模调度与线程快速路，数字在原有区间内（4 MiB 顺序读 0.86x、大块写的高低均为 macOS runner 抖动）。

### 变更（内部）
- **`complete_inline` 上提至 `IOBackendBase`。** 原 `IOUringBackend::complete_inline` 移动为所有后端共享的内联完成实现，io_uring 的 COOP_TASKRUN 内联收割与 macOS mincore 快车道共用；取值/清理逻辑与批量器记账策略不变。

## [1.8.0] - 2026-09-27

### 性能
- **io_uring：提交即完成的内联收割（缓存命中快车道）。** 1.7.0 探测启用的 `IORING_SETUP_COOP_TASKRUN` 让缓存命中的读在提交者自己的 `io_uring_enter` 里就完成了，但所有完成仍要绕 reaper 线程一圈。现在每次提交后，提交线程会非阻塞地瞥一眼 CQ——与 reaper 共享的新消费侧互斥锁上 `try_lock`，每次最多摘 32 个队首数据 CQE，wakeup CQE 永远留给 reaper——并用直接 `set_result`/`set_exception` 内联完成，这正是 Windows `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` 路径的 uring 对应物。I/O 调用返回时 future 已 resolve，缓存命中的 `await` 不再让出事件循环；批量器记账保持平衡。不支持 `COOP_TASKRUN` 的旧内核自动落回 reaper 路径。

### 新增
- **`ayafileio.acopy(src, dst)` —— 异步整文件复制。** 优先在 worker 线程里走 OS 级快车道：Linux 用内核态零拷贝 `copy_file_range`，Windows 用 `CopyFile2`（顺带保留时间戳等元数据）；其它平台、以及快车道被系统拒绝时（跨设备、NFS 等）透明回退到 `read_at`/`write_at` 流水线：最多 `concurrency` 个 `chunk_size` 块在飞（内存上界 `concurrency × chunk_size`，默认 8 × 4 MiB；目标文件预先扩展到全尺寸，避免反复扩展 EOF）。语义对齐 `shutil.copyfile`：目标先截断，复制到自身抛 `shutil.SameFileError`，`copy_stat=True` 时附加 `shutil.copystat`。本地实测 512 MiB 缓存热文件 3,300 MiB/s，与 `robocopy`（3,413 MiB/s）持平。

### 构建 / CI
- **Python 3.15 支持。** 测试矩阵覆盖 3.15 与 3.15t（10 月 1 日正式版发布前经 `allow-prereleases` 解析到 3.15.0 RC），wheel 流水线新增 `cp315`/`cp315t` 构建——cibuildwheel 3.2.1 → 4.2.1，`CIBW_ENABLE` 换成 `cpython-prerelease`（4.x 移除了 `cpython-freethreading` 组，自由线程 wheel 改为默认构建）。标签发布后经手动 dispatch 补传，`cp315`/`cp315t` wheel 已于 2026-09-27 上传 PyPI，覆盖 Windows / Linux（x86-64、aarch64）/ macOS。
- **riscv64 从 wheel 流水线拆出。** QEMU 全模拟的 riscv64 构建（比其它所有目标加起来还慢）独立成单独 job；手动触发新增 `linux_targets` 输入（`all`/`fast`/`riscv64`）——补发 wheel 不必再陪跑慢路径。1.8.0 的补传走 `fast` 通道，`cp315`/`cp315t` 的 riscv64 wheel 于 2026-09-29 经 `linux_targets=riscv64` 手动 dispatch 单独补发。

## [1.7.0] - 2026-09-26

### 变更
- **语义提示：写入缓冲在写 future 完成前会被钉住。** 所有后端的写路径已改为直接从调用方缓冲零拷贝读取（见下），因此写在飞期间缓冲对象不可 resize（持有的缓冲视图会对 resize 抛 `BufferError`），内容也不应原地修改——与 `readinto()` 和原生 `os.FileIO.write` 同级契约。

### 性能
- **所有后端真正用上完成批量器了。** io_uring、macOS Dispatch I/O 与线程池后端的 `complete_ok()`/`complete_error()` 此前每完成一个操作就 flush 一次，悄悄绕过了自适应批量机制。现在与 1.6.1 的 IOCP 路径同策略：达到数量阈值或本事件循环最后一个在飞操作完成时才 flush。
- **全平台零拷贝读。** 读取直接进预建的 `PyBytes`，完成时它就是结果对象，每次读省一整趟数据拷贝（短读回退为收缩拷贝）。
- **全平台零拷贝写。** 写路径不再把调用方缓冲 memcpy 进池缓冲，内核通过持有的缓冲视图直接读用户内存。Windows 大块顺序写：1MiB 1,363 → 2,858 MB/s，4MiB 929 → 3,046 MB/s（本地实测，对 aiofiles 从 0.73x 转为 1.47x）。
- **io_uring：背压取代虚假 EBUSY。** 提交队列打满时请求排入溢出队列，由 reaper 在每批 CQE 后补提交，不再让操作直接失败。SQ 访问改由提交互斥锁串行化（Python 线程与 reaper 并发提交此前理论上有竞争）。
- **io_uring：reaper 批量持 GIL**——先不持 GIL 摘取 CQE，再一次持 GIL 完成整批，与 IOCP worker 同款节奏。
- **io_uring：探测启用 `IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG`**（内核 5.19+），旧内核静默回退，每个完成省一次软中断调度。刻意不用 `IORING_SETUP_SINGLE_ISSUER`（提交合法地来自多个线程）。
- **macOS：大 I/O 双模调度。** `dispatch_io` 的 high-water 设置会把大请求切成 64KiB 小块逐块回调加拷贝，大块顺序吞吐一度只剩 aiofiles 的 4%。≥256KiB 的请求现在绕过 `dispatch_io`，在线程池上直接一发 `pread`/`pwrite` 进最终缓冲；小请求保留低延迟 `dispatch_io` 路径。CI 实测：1MiB 顺序写 139 → 5,502 MB/s（40 倍），4MiB 208 → 6,787 MB/s；1MiB 顺序读对 aiofiles 从 0.28x 回到 0.97x。
- **macOS 与线程池后端：open 时缓存文件大小**（写/截断时乐观刷新），消灭每操作一发的 `fstat` 系统调用；`close()` 改用 `m_close_wake` 信号量等待，取代指数退避 sleep 轮询，关闭延迟从平均 ~16ms 降到即时。
- **Windows：`FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`。** 同步完成的操作不再投递 IOCP 完成包，热缓存小块 I/O 每次操作省一次 worker 唤醒和上下文切换。模式设置失败的会话保持旧的 worker 排水路径（两条路径严格互斥）。
- **IORequest 对象池。** thread_local 空闲链表（64 槽位）+ placement new 重建，取代每操作的 `new`/`delete`，消除小块高频 I/O 的分配器抖动；与幽幽子内存追踪器完全兼容。

### 变更（内部）
- 整个 C++ 核心收进 `namespace ayafileio`，`bindings.cpp` 以 using 指令引入。公开 API 无变化。

### CI / 测试
- **发布流水线版本守护**：`workflow_run` 触发时若 `pyproject.toml` 的版本号已存在于 PyPI，则跳过 wheel 构建与发布（普通测试推送从 ~1 小时 → ~6 秒）。tag 与手动触发总是构建；要发布 bump 版本号即可。
- **重写跨平台基准**（`tests/t_compare.py`）：时间盒轮次 + 双库交错测量 + 中位数报告，并发场景无竞态（每 worker 一句柄、位置 I/O），产出 JSON 产物（`benchmark_results.json` / `benchmark_results_detailed.json`）由 CI 在三平台每次运行上传。旧基准测的是自己 seek+read 竞态造出来的错误，且从未写出产物文件。

## [1.6.1] - 2026-09-26

### 修复
- **Windows (IOCP)：串行与低并发负载不再为完成批量器的空闲超时买单。** `ResultBatcher` 把完成通知聚合成少量 `loop.call_soon_threadsafe` 回调，此前只在数量阈值（64）或空闲超时（~5ms）触发时才 flush。孤零零一个在飞操作——串行 `await` 循环和低并发管线的常态——永远够不到阈值，每个操作都要干等满空闲超时才能拿到结果。现在批量器按事件循环跟踪在飞 IOCP 请求数（`op_submitted()` / `op_completed()`，挂在此前闲置的 `IORequest::batcher` 字段上），当本 loop 最后一个在飞操作完成时立即 flush：单操作延迟从 ~5ms 降到事件循环唤醒级别，高并发下的批量行为完全不变。计数是 fail-soft 的——即使失衡也只会退化为原来的空闲超时路径。实测 256MB 文件、4KB 随机 `read_at`（2 万次）：16 在飞时 1,313 → 21,435 ops/s（16 倍）；64KB 块顺序 `read_at` 整文件：11 → 314 MB/s（28 倍）；大块（4MB）顺序读现已反超 aiofiles（2.17 倍）；高并发吞吐不变。Linux io_uring、macOS Dispatch I/O 与线程池回退后端不受影响。

## [1.6.0] - 2026-09-22

### 变更
- **文本模式 `read(n)` / `readline()` 对齐 CPython `open()` 逐字符语义**（行为变更，故升 minor）。1.5.x 把 `size` 当字节预算；1.6.0 移植 `io.TextIOWrapper` 三层 I/O 栈的完整思想：
  - **块读底层**：`read(n)` 不再每次调用一次系统调用——按 64KB 块预读，分块读循环的 IOCP/io_uring 提交次数大幅下降。
  - **增量解码**：`codecs` 增量解码器拼接跨 chunk 的多字节序列；结尾残缺字节在进解码器**之前**被 strict 探测切出，解码器因此永不持有内部状态，`errors` 各模式（strict/replace/ignore）行为全部正确。
  - **换行翻译改为入段时做**（与 CPython 相同：解码后、返回前）；段尾孤立 `\r` 挂起一拍，跨 chunk 的 `\r\n` 绝不会被切成两个行尾。
  - `read(n)` 按**翻译后字符数**计数——UTF-8 中文 `read(3)` 精确返回 3 个字符。
  - **`tell()` 保持普通字节偏移**（比 CPython 的不透明 cookie 更好用）：分段字符缓冲为每段登记来源字节数，未消费字节始终可知。纯 `\n` 内容字节精确；`\r\n` 密集内容每处可能漂移 1 字节（换行压缩的固有问题——CPython 在此彻底放弃精确性改用 cookie）。二进制模式一直不受影响。
  - 文本模式 `seek()` 策略（1.5.2）不变：仅 `seek(0)`；位置访问仍仅二进制（`read_at`/`write_at`）。

## [1.5.3] - 2026-09-22

### 修复
- **文本模式 `read(n)` 不再对半切多字节字符**：此前 `size` 被直接当作原始字节数交给底层，`read(7)` 读 UTF-8 中文会切在汉字中间抛 `UnicodeDecodeError`——或在 `errors="replace"` 下静默插入 U+FFFD（最危险的形态：数据被悄悄换掉而不报错）。文本模式 `read(size)` 现在把 `size` 视为**字节预算**：strict 探测结尾的残缺字符，把残缺字节推回（impl 位置回退 / 预读缓冲回退），只解码完整字符部分；预算连一个字符都装不下时（如 `read(1)` 遇 3 字节汉字）有界补读，至少返回一个完整字符。ASCII 下行为不变（字节=字符）；`read(-1)` 与 `readline()` 本来就按边界解码，不受影响。

## [1.5.2] - 2026-09-22

### 变更
- **文本模式 `seek()` 收敛为仅允许 `seek(0)`**（纯 Python 策略，零后端改动）：文本模式的裸字节偏移可能落在多字节字符中间，污染解码——strict 模式抛 `UnicodeDecodeError`，`errors="replace"` 则静默插入 U+FFFD（更危险：数据被悄悄换掉而不报错）。文本模式现在对一切非零偏移与非 SEEK_SET 模式抛 `ValueError`。二进制模式不受影响（位置访问本就是 `read_at`/`write_at` 的原生场景），需要位置访问请以二进制模式打开。`r+`/`w+` 文本模式下 `seek(0)` 的重写流程不变。

## [1.5.1] - 2026-09-16

### 新增
- **位置写 API —— `AsyncFile.write_at(offset, data)`**：在显式 offset 处写入字节，不改变文件逻辑位置。Windows IOCP、Linux io_uring/线程池、macOS Dispatch I/O 后端均支持。
- **批量位置写 —— `AsyncFile.write_many(writes)`**：并发提交多个 `(offset, data)` 写请求，按输入顺序返回写入字节数。

### 变更
- 批量读现在直接收集原生 Future，不再为每个请求创建一个 Python Task。
- 长行扫描不再重复扫描前置读缓冲中已检查过的部分。

### 修复
- Windows 文件对象在事件循环关闭后的析构过程中，不再尝试关闭已失效的 IOCP 会话。

## [1.5.0] - 2026-09-02

### 新增
- **位置读 API —— `AsyncFile.read_at(offset, size=-1)`**：从显式 `offset` 读取最多 `size` 字节，不触碰文件逻辑位置（`tell()` 前后不变，与并发的 `read()`/`seek()` 无竞争）。随机读不再需要 `await seek(off)` + `await read(n)` 两次协程往返。语义对齐 POSIX `pread`：`size < 0` 读到 EOF；读到 EOF 截断返回短数据；`offset` 越界返回 `b""`；`offset < 0` 抛 `ValueError`。仅二进制模式（文本模式抛 `ValueError`——位置读与解码/换行翻译语义不兼容）。四个后端全部端到端实现：
  - **Windows (IOCP)**：新增 `IOCPContext::submit_read_at`，`ReadFile` 的 `OVERLAPPED.Offset/OffsetHigh` 直接取参数，绝不触碰 `Session::filePos`。
  - **线程池回退（POSIX）**：worker 内做真正的位置读 `pread`（不用 `lseek`+`read`，避免与其他操作竞争）。
  - **Linux (io_uring)**：`io_uring_prep_read` 显式偏移。
  - **macOS (Dispatch I/O)**：`DISPATCH_IO_RANDOM` 通道上 `dispatch_io_read` 显式偏移。
- **批量位置读 —— `AsyncFile.read_many(spans)`**：通过 `asyncio.gather` 在一个事件循环周期内提交一批 `(offset, size)` 读请求，返回与 spans 同序的结果。面向模型权重流式加载的按层批量读场景：所有 I/O 在首个挂起点前全部提交，C++ 层 `ResultBatcher` 会把一批完成通知聚合成少量 `loop.call_soon_threadsafe` 回调——比逐个 `await read_at(...)` 少 N-1 次协程往返。

## [1.4.8] - 2026-07-02

### 修复
- **Windows 上偶发的整进程死锁（CI 卡死）**：`IOCPContext::flush_batchers()` 持着 `m_batchersMtx` 调用 `ResultBatcher` 的 flush，而后者内部会调 `loop.call_soon_threadsafe()`——asyncio 的实现走 socket `send()`，期间会临时释放再重新获取 GIL。如果此刻恰好有另一个线程持着 GIL 在等 `m_batchersMtx`（另一个 IOCP worker 的 `flush_batchers()`，或主线程 open() 文件），两个线程互等对方，整个进程冻结——包括看门狗线程。此 bug 在 1.4.7 及更早版本就存在（两个 IOCP worker 之间即可相撞），表现为 GitHub CI 的 Windows 测试步骤偶发超时。通过对卡死进程的原生栈转储确认根因，并用循环复现验证（修复前 1–3 次内必卡，修复后 100 次全部通过）。修复原则：绝不在持有 C++ 锁时执行可能释放 GIL 的 Python 调用：
  - `flush_batchers()` 先在锁内拷出 batcher 指针，锁外再 flush。
  - `get_or_create_batcher()`（非 IOCP 注册表）在 `g_batchersMtx` 锁外构造 batcher。
  - `remove_session()` 把 `Session` 的析构（内含 `Py_DECREF`）移到 `m_sessionsMtx` 锁外。
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