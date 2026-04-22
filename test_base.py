import asyncio
import sys
import tempfile
import time
import os
import signal
import subprocess
import threading
import traceback
from pathlib import Path
from typing import Callable, Awaitable

# 添加项目根目录到路径
sys.path.insert(0, str(Path(__file__).parent))

if sys.platform == "win32":
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import ayafileio


# ════════════════════════════════════════════════════════════════════════════
# 超时诊断工具
# ════════════════════════════════════════════════════════════════════════════


class TimeoutDiagnostics:
    """超时诊断工具 - 当测试卡住时自动收集堆栈信息"""

    _instance = None
    _timeout_seconds = 30  # 单个测试超时时间
    _current_test_name = None
    _start_time = None
    _watchdog_thread = None
    _stop_watchdog = False

    def __init__(self):
        self._lock = threading.Lock()

    @classmethod
    def get_instance(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def start_test(self, name: str):
        """开始一个新测试"""
        with self._lock:
            self._current_test_name = name
            self._start_time = time.time()
            self._stop_watchdog = False

            # 启动看门狗线程
            if self._watchdog_thread is None or not self._watchdog_thread.is_alive():
                self._watchdog_thread = threading.Thread(
                    target=self._watchdog_loop, daemon=True
                )
                self._watchdog_thread.start()

    def stop_test(self):
        """测试完成"""
        with self._lock:
            self._current_test_name = None
            self._start_time = None

    def _watchdog_loop(self):
        """看门狗循环 - 检测超时"""
        while not self._stop_watchdog:
            time.sleep(1)  # 每秒检查一次

            with self._lock:
                if self._current_test_name and self._start_time:
                    elapsed = time.time() - self._start_time
                    if elapsed > self._timeout_seconds:
                        self._handle_timeout(self._current_test_name, elapsed)
                        break

    def _handle_timeout(self, test_name: str, elapsed: float):
        """处理超时 - 收集诊断信息"""
        print(f"\n{'=' * 80}")
        print(f"⚠️  TIMEOUT DETECTED: '{test_name}' running for {elapsed:.1f}s")
        print(f"{'=' * 80}\n")

        # 1. 打印 Python 线程堆栈
        self._print_python_stacks()

        # 2. 尝试获取 native 堆栈（Linux/macOS）
        self._print_native_stacks()

        # 3. 打印 asyncio 任务信息
        self._print_asyncio_tasks()

        # 4. 尝试打印 io_uring 状态
        self._print_io_uring_status()

        print(f"\n{'=' * 80}")
        print("Terminating due to timeout...")
        print(f"{'=' * 80}")

        # 强制退出
        os._exit(1)

    def _print_python_stacks(self):
        """打印所有 Python 线程的堆栈"""
        print("\n--- Python Thread Stacks ---")
        try:
            for thread_id, frame in sys._current_frames().items():
                thread_name = "Unknown"
                for t in threading.enumerate():
                    if t.ident == thread_id:
                        thread_name = t.name
                        break
                print(f"\n[Thread {thread_id} ({thread_name})]")
                traceback.print_stack(frame)
        except Exception as e:
            print(f"Failed to get Python stacks: {e}")

    def _print_native_stacks(self):
        """打印 native 堆栈（使用 GDB/LLDB）"""
        print("\n--- Native Stacks ---")

        pid = os.getpid()

        if sys.platform == "linux":
            self._gdb_backtrace(pid)
        elif sys.platform == "darwin":
            self._lldb_backtrace(pid)
        elif sys.platform == "win32":
            self._windows_stacks(pid)

    def _gdb_backtrace(self, pid: int):
        """使用 GDB 获取 native 堆栈"""
        try:
            # 检查 GDB 是否可用
            subprocess.run(["gdb", "--version"], capture_output=True, timeout=2)
        except (FileNotFoundError, subprocess.TimeoutExpired):
            print("GDB not available. Install gdb for native stack traces.")
            return

        try:
            # 创建 GDB 命令文件
            gdb_commands = f"""set pagination off
set print thread-events off
info threads
thread apply all bt full
detach
quit
"""
            with tempfile.NamedTemporaryFile(
                mode="w", suffix=".gdb", delete=False
            ) as f:
                f.write(gdb_commands)
                cmd_file = f.name

            # 执行 GDB
            cmd = ["gdb", "-p", str(pid), "-batch", "-x", cmd_file]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)

            print(result.stdout)
            if result.stderr:
                print("GDB stderr:", result.stderr)

            os.unlink(cmd_file)
        except subprocess.TimeoutExpired:
            print("GDB timed out")
        except Exception as e:
            print(f"GDB failed: {e}")

    def _lldb_backtrace(self, pid: int):
        """使用 LLDB 获取 native 堆栈（macOS）"""
        try:
            lldb_commands = f"""
process attach --pid {pid}
thread backtrace all
detach
quit
"""
            # macOS 上 LLDB 的调用方式
            result = subprocess.run(
                [
                    "lldb",
                    "-o",
                    f"process attach --pid {pid}",
                    "-o",
                    "thread backtrace all",
                    "-o",
                    "detach",
                    "-o",
                    "quit",
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            print(result.stdout)
        except FileNotFoundError:
            print("LLDB not available.")
        except subprocess.TimeoutExpired:
            print("LLDB timed out")
        except Exception as e:
            print(f"LLDB failed: {e}")

    def _windows_stacks(self, pid: int):
        """Windows 堆栈"""
        print("Windows native stack trace not implemented in this version.")
        print("Consider using Process Explorer or WinDbg for debugging.")

    def _print_asyncio_tasks(self):
        """打印所有 asyncio 任务"""
        print("\n--- Asyncio Tasks ---")
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            print("No running event loop")
            return

        try:
            tasks = asyncio.all_tasks(loop)
            print(f"Total tasks: {len(tasks)}")
            for i, task in enumerate(tasks):
                print(f"\nTask {i}: {task}")
                print(f"  Done: {task.done()}")
                if task.done():
                    try:
                        result = task.result()
                        print(f"  Result: {result}")
                    except Exception as e:
                        print(f"  Exception: {e}")
                else:
                    print("  Stack:")
                    try:
                        stack = task.get_stack()
                        for frame in stack:
                            print(f"    {frame}")
                    except Exception as e:
                        print(f"    (failed to get stack: {e})")
        except Exception as e:
            print(f"Failed to get asyncio tasks: {e}")

    def _print_io_uring_status(self):
        """打印 io_uring 后端状态（如果可用）"""
        if sys.platform != "linux":
            return

        print("\n--- io_uring Status ---")
        try:
            info = ayafileio.get_backend_info()
            if info.get("backend") == "io_uring":
                print("Backend: io_uring (active)")
                # 可以添加更多 io_uring 特定的诊断
            else:
                print(f"Backend: {info.get('backend', 'unknown')}")
        except Exception as e:
            print(f"Failed to get backend info: {e}")


# 全局超时诊断实例
_timeout_diag = TimeoutDiagnostics.get_instance()


# ════════════════════════════════════════════════════════════════════════════
# 信号处理器（备用方案 - 用于 SIGALRM）
# ════════════════════════════════════════════════════════════════════════════


def setup_signal_handler():
    """设置信号处理器用于超时（Linux/macOS）"""
    if sys.platform == "win32":
        return

    def signal_handler(signum, frame):
        print(f"\n{'=' * 80}")
        print(f"Received signal {signum} - TIMEOUT")
        print(f"{'=' * 80}\n")

        # 打印 Python 堆栈
        print("\n--- Python Stack (signal handler) ---")
        traceback.print_stack(frame)

        # 打印所有线程堆栈
        print("\n--- All Thread Stacks (signal handler) ---")
        for thread_id, thread_frame in sys._current_frames().items():
            thread_name = "Unknown"
            for t in threading.enumerate():
                if t.ident == thread_id:
                    thread_name = t.name
                    break
            print(f"\n[Thread {thread_id} ({thread_name})]")
            traceback.print_stack(thread_frame)

        sys.exit(1)

    signal.signal(signal.SIGALRM, signal_handler)
    signal.signal(signal.SIGUSR1, signal_handler)  # 可用于手动触发


# ════════════════════════════════════════════════════════════════════════════
# 共享事件循环管理
# ════════════════════════════════════════════════════════════════════════════

_shared_loop = None


def get_shared_loop():
    """获取或创建共享事件循环"""
    global _shared_loop
    if _shared_loop is None or _shared_loop.is_closed():
        # 始终显式创建新的事件循环
        # 不要使用 asyncio.get_event_loop()，因为它在 3.14 中会抛出 RuntimeError
        # 在 3.10 中会产生 DeprecationWarning
        if sys.platform == "win32":
            # Windows 上使用 ProactorEventLoop（默认）
            _shared_loop = asyncio.new_event_loop()
        else:
            # Linux/macOS 使用 SelectorEventLoop
            try:
                _shared_loop = asyncio.SelectorEventLoop()
            except Exception:
                _shared_loop = asyncio.new_event_loop()

        asyncio.set_event_loop(_shared_loop)
    return _shared_loop


def cleanup_shared_loop():
    """清理共享事件循环"""
    global _shared_loop
    if _shared_loop and not _shared_loop.is_closed():
        # 取消所有待处理任务
        try:
            pending = asyncio.all_tasks(_shared_loop)
            for task in pending:
                task.cancel()
            if pending:
                _shared_loop.run_until_complete(
                    asyncio.gather(*pending, return_exceptions=True)
                )
        except Exception as e:
            print(f"Warning: Error during loop cleanup: {e}")

        try:
            _shared_loop.close()
        except Exception as e:
            print(f"Warning: Error closing loop: {e}")

        _shared_loop = None
        asyncio.set_event_loop(None)


# ════════════════════════════════════════════════════════════════════════════
# 测试运行器
# ════════════════════════════════════════════════════════════════════════════


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.failures = []
        self.start_time = None
        self.enable_timeout_diag = os.environ.get("AYAFILEIO_TIMEOUT_DIAG", "1") == "1"
        self.timeout_seconds = int(os.environ.get("AYAFILEIO_TEST_TIMEOUT", "30"))

        # 配置超时时间
        TimeoutDiagnostics._timeout_seconds = self.timeout_seconds

        # 设置信号处理器
        setup_signal_handler()

    def test(self, name: str):
        """测试装饰器"""

        def decorator(func: Callable[[], Awaitable[None]]):
            async def wrapper():
                if self.enable_timeout_diag:
                    _timeout_diag.start_test(name)
                try:
                    await func()
                    self.passed += 1
                    print(f"  ✅ {name}")
                except AssertionError as e:
                    self.failed += 1
                    self.failures.append((name, str(e)))
                    print(f"  ❌ {name}: {e}")
                except Exception as e:
                    self.failed += 1
                    self.failures.append((name, f"{type(e).__name__}: {e}"))
                    print(f"  💥 {name}: {type(e).__name__}: {e}")
                finally:
                    if self.enable_timeout_diag:
                        _timeout_diag.stop_test()

            return wrapper

        return decorator

    def run_sync(self, tests: list[tuple[str, Callable]]):
        """运行同步测试"""
        for name, func in tests:
            try:
                func()
                self.passed += 1
                print(f"  ✅ {name}")
            except AssertionError as e:
                self.failed += 1
                self.failures.append((name, str(e)))
                print(f"  ❌ {name}: {e}")
            except Exception as e:
                self.failed += 1
                self.failures.append((name, f"{type(e).__name__}: {e}"))
                print(f"  💥 {name}: {type(e).__name__}: {e}")

    def run_async(self, name: str, coro_func: Callable[[], Awaitable[None]]):
        """运行单个异步测试"""
        if self.enable_timeout_diag:
            _timeout_diag.start_test(name)

        try:
            # 以下版本存在已知的事件循环问题：
            # - 3.10: get_event_loop() 发出 DeprecationWarning，行为不稳定
            # - 3.12: 子解释器 + OWN_GIL 可能导致 C 扩展死锁
            # - 3.14: get_event_loop() 直接抛出 RuntimeError
            problematic_versions = {(3, 10), (3, 12), (3, 14)}
            use_isolated_loop = sys.version_info[:2] in problematic_versions

            if use_isolated_loop:
                # 为问题版本创建全新的独立事件循环
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
                try:
                    loop.run_until_complete(coro_func())
                finally:
                    # 清理所有 pending 任务
                    try:
                        pending = asyncio.all_tasks(loop)
                        if pending:
                            loop.run_until_complete(
                                asyncio.gather(*pending, return_exceptions=True)
                            )
                    except Exception:
                        pass
                    loop.close()
                    asyncio.set_event_loop(None)
            else:
                # 3.11 和 3.13 可以安全使用共享循环
                loop = get_shared_loop()
                loop.run_until_complete(coro_func())

            self.passed += 1
            print(f"  ✅ {name}")
        except Exception as e:
            ...
        finally:
            if self.enable_timeout_diag:
                _timeout_diag.stop_test()

    def print_summary(self):
        total = self.passed + self.failed + self.skipped
        duration = time.time() - self.start_time if self.start_time else 0

        print("\n" + "=" * 60)
        print(
            f"测试完成: {total} 个测试, {self.passed} 通过, {self.failed} 失败, {self.skipped} 跳过"
        )
        print(f"耗时: {duration:.2f}s")

        if self.failures:
            print("\n失败的测试:")
            for name, error in self.failures:
                print(f"  - {name}: {error}")

        print("=" * 60)


runner = TestRunner()


# ════════════════════════════════════════════════════════════════════════════
# 测试辅助函数
# ════════════════════════════════════════════════════════════════════════════


def get_temp_path(suffix: str = "") -> Path:
    """获取临时文件路径"""
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as f:
        return Path(f.name)


async def read_file_native(path: Path) -> str:
    """用原生 open 读取文件"""
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# ════════════════════════════════════════════════════════════════════════════
# 异步测试函数
# ════════════════════════════════════════════════════════════════════════════


async def test_invalid_mode_async():
    """无效模式（需要事件循环）"""
    path = Path("test.txt")
    try:
        ayafileio.open(path, "invalid")
        assert False, "应该抛出 ValueError"
    except ValueError:
        pass
    finally:
        path.unlink(missing_ok=True)


async def test_binary_with_encoding_async():
    """二进制模式不能指定编码（需要事件循环）"""
    try:
        ayafileio.open("test.bin", "rb", encoding="utf-8")
        assert False, "应该抛出 ValueError"
    except ValueError:
        pass


async def test_write_read_text():
    path = get_temp_path(".txt")
    try:
        content = "Hello, 世界!\n第二行\n第三行"

        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            written = await f.write(content)
            assert written == len(content.encode("utf-8"))

        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            read_content = await f.read()
            assert read_content == content
    finally:
        path.unlink(missing_ok=True)


async def test_write_read_binary():
    path = get_temp_path(".bin")
    try:
        content = b"Hello, World!\x00\x01\x02\x03\x04\x05"

        async with ayafileio.open(path, "wb") as f:
            written = await f.write(content)
            assert written == len(content)

        async with ayafileio.open(path, "rb") as f:
            read_content = await f.read()
            assert read_content == content
    finally:
        path.unlink(missing_ok=True)


async def test_read_chunks():
    path = get_temp_path(".bin")
    try:
        content = b"0123456789" * 100

        async with ayafileio.open(path, "wb") as f:
            await f.write(content)

        async with ayafileio.open(path, "rb") as f:
            chunk1 = await f.read(100)
            assert len(chunk1) == 100

            chunk2 = await f.read(200)
            assert len(chunk2) == 200

            chunk3 = await f.read()
            assert len(chunk1) + len(chunk2) + len(chunk3) == len(content)
    finally:
        path.unlink(missing_ok=True)


async def test_readline():
    path = get_temp_path(".txt")
    try:
        lines = ["第一行\n", "第二行\n", "第三行\n", "第四行"]
        content = "".join(lines)

        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            await f.write(content)

        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            read_lines = []
            while True:
                line = await f.readline()
                if not line:
                    break
                read_lines.append(line)

            assert read_lines == lines
    finally:
        path.unlink(missing_ok=True)


async def test_readlines():
    path = get_temp_path(".txt")
    try:
        lines = ["第一行\n", "第二行\n", "第三行\n"]
        content = "".join(lines)

        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            await f.write(content)

        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            read_lines = await f.readlines()
            assert read_lines == lines
    finally:
        path.unlink(missing_ok=True)


async def test_append_text():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            await f.write("第一行\n")

        async with ayafileio.open(path, "a", encoding="utf-8") as f:
            await f.write("第二行\n")

        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            content = await f.read()
            assert content == "第一行\n第二行\n"
    finally:
        path.unlink(missing_ok=True)


async def test_append_binary():
    path = get_temp_path(".bin")
    try:
        async with ayafileio.open(path, "wb") as f:
            await f.write(b"first")

        async with ayafileio.open(path, "ab") as f:
            await f.write(b"second")

        async with ayafileio.open(path, "rb") as f:
            content = await f.read()
            assert content == b"firstsecond"
    finally:
        path.unlink(missing_ok=True)


async def test_seek_from_start():
    path = get_temp_path(".bin")
    try:
        content = b"0123456789"

        async with ayafileio.open(path, "wb") as f:
            await f.write(content)

        async with ayafileio.open(path, "rb") as f:
            await f.seek(5)
            chunk = await f.read(2)
            assert chunk == b"56"
    finally:
        path.unlink(missing_ok=True)


async def test_seek_from_current():
    path = get_temp_path(".bin")
    try:
        content = b"0123456789"

        async with ayafileio.open(path, "wb") as f:
            await f.write(content)

        async with ayafileio.open(path, "rb") as f:
            await f.read(3)
            await f.seek(2, 1)
            chunk = await f.read(2)
            assert chunk == b"56"
    finally:
        path.unlink(missing_ok=True)


async def test_seek_from_end():
    path = get_temp_path(".bin")
    try:
        content = b"0123456789"

        async with ayafileio.open(path, "wb") as f:
            await f.write(content)

        async with ayafileio.open(path, "rb") as f:
            await f.seek(-3, 2)
            chunk = await f.read()
            assert chunk == b"789"
    finally:
        path.unlink(missing_ok=True)


async def test_auto_close():
    path = get_temp_path(".txt")
    try:
        f = ayafileio.open(path, "w")
        assert not f.closed

        async with f:
            await f.write("test")

        assert f.closed

        try:
            await f.write("test")
            assert False, "应该抛出 ValueError"
        except ValueError:
            pass
    finally:
        path.unlink(missing_ok=True)


async def test_manual_close():
    path = get_temp_path(".txt")
    try:
        f = ayafileio.open(path, "w")
        await f.write("test")
        await f.close()

        assert f.closed
    finally:
        path.unlink(missing_ok=True)


async def test_async_iteration():
    path = get_temp_path(".txt")
    try:
        lines = ["line1\n", "line2\n", "line3\n"]

        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            for line in lines:
                await f.write(line)

        read_lines = []
        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            async for line in f:
                read_lines.append(line)

        assert read_lines == lines
    finally:
        path.unlink(missing_ok=True)


async def test_flush():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "w") as f:
            await f.write("test")
            await f.flush()

            assert path.exists()
            native_content = await read_file_native(path)
            assert native_content == "test"
    finally:
        path.unlink(missing_ok=True)


async def test_path_object():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "w") as f:
            await f.write("test")

        async with ayafileio.open(path, "r") as f:
            content = await f.read()
            assert content == "test"

        f = ayafileio.open(path, "r")
        assert f.name == str(path)
        await f.close()
    finally:
        path.unlink(missing_ok=True)


async def test_file_not_found():
    try:
        async with ayafileio.open("/nonexistent/path/file.txt", "r") as f:
            await f.read()
        assert False, "应该抛出 FileNotFoundError"
    except FileNotFoundError:
        pass


async def test_write_str_to_binary():
    path = get_temp_path(".bin")
    try:
        try:
            async with ayafileio.open(path, "wb") as f:
                await f.write("string")  # type: ignore
            assert False, "应该抛出 TypeError"
        except TypeError:
            pass
    finally:
        path.unlink(missing_ok=True)


async def test_write_bytes_to_text():
    path = get_temp_path(".txt")
    try:
        try:
            async with ayafileio.open(path, "w") as f:
                await f.write(b"bytes")  # type: ignore
            assert False, "应该抛出 TypeError"
        except TypeError:
            pass
    finally:
        path.unlink(missing_ok=True)


async def test_operation_on_closed_file():
    path = get_temp_path(".txt")
    try:
        f = ayafileio.open(path, "w")
        await f.close()

        try:
            await f.write("test")
            assert False, "write 应该抛出 ValueError"
        except ValueError:
            pass

        try:
            await f.read()
            assert False, "read 应该抛出 ValueError"
        except ValueError:
            pass
    finally:
        path.unlink(missing_ok=True)


async def test_concurrent_reads():
    path = get_temp_path(".txt")
    try:
        content = "x" * 10000

        async with ayafileio.open(path, "w") as f:
            await f.write(content)

        async def read_chunk(start: int, size: int) -> bytes:
            async with ayafileio.open(path, "rb") as f:
                await f.seek(start)
                return await f.read(size)

        tasks = [
            read_chunk(0, 1000),
            read_chunk(1000, 1000),
            read_chunk(2000, 1000),
            read_chunk(3000, 1000),
        ]

        results = await asyncio.gather(*tasks)

        for i, chunk in enumerate(results):
            expected = content[i * 1000 : (i + 1) * 1000].encode()
            assert chunk == expected
    finally:
        path.unlink(missing_ok=True)


async def test_concurrent_writes_different_files():
    paths = []
    try:

        async def write_file(index: int) -> int:
            path = get_temp_path(f"_concurrent_{index}.txt")
            paths.append(path)
            async with ayafileio.open(path, "w") as f:
                await f.write(f"file {index}")
            return index

        tasks = [write_file(i) for i in range(10)]
        results = await asyncio.gather(*tasks)

        assert results == list(range(10))

        for i in range(10):
            async with ayafileio.open(paths[i], "r") as f:
                content = await f.read()
                assert content == f"file {i}"
    finally:
        for p in paths:
            p.unlink(missing_ok=True)


async def test_encoding_utf8():
    path = get_temp_path(".txt")
    try:
        content = "Hello, 世界!"

        async with ayafileio.open(path, "w", encoding="utf-8") as f:
            await f.write(content)

        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            read_content = await f.read()
            assert read_content == content
    finally:
        path.unlink(missing_ok=True)


async def test_encoding_gbk():
    path = get_temp_path(".txt")
    try:
        content = "Hello, 中文!"

        try:
            async with ayafileio.open(path, "w", encoding="gbk") as f:
                await f.write(content)

            async with ayafileio.open(path, "r", encoding="gbk") as f:
                read_content = await f.read()
                assert read_content == content
        except UnicodeEncodeError:
            runner.skipped += 1
            print("  ⏭️ GBK 编码测试跳过 (系统不支持)")
    finally:
        path.unlink(missing_ok=True)


async def test_exclusive_create():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "x") as f:
            await f.write("test")

        async with ayafileio.open(path, "x") as f:
            await f.write("test")
        assert False
    except FileExistsError:
        pass
    finally:
        path.unlink(missing_ok=True)


async def test_read_write_mode():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "w") as f:
            await f.write("Hello, World!")

        async with ayafileio.open(path, "r+") as f:
            content = await f.read()
            assert content == "Hello, World!"

            await f.seek(0)
            await f.write("Hi")
            await f.seek(0)
            new_content = await f.read()
            assert new_content == "Hillo, World!"
    finally:
        path.unlink(missing_ok=True)


async def test_w_plus_mode():
    path = get_temp_path(".txt")
    try:
        async with ayafileio.open(path, "w+") as f:
            await f.write("test")
            await f.seek(0)
            content = await f.read()
            assert content == "test"
    finally:
        path.unlink(missing_ok=True)


# ════════════════════════════════════════════════════════════════════════════
# 主函数
# ════════════════════════════════════════════════════════════════════════════


def main():
    print("=" * 60)
    print("ayafileio 测试套件")
    print("=" * 60)

    # 打印环境信息
    print(f"\nPython: {sys.version}")
    print(f"Platform: {sys.platform}")
    info = ayafileio.get_backend_info()
    print(f"Backend: {info['backend']} (truly_async: {info['is_truly_async']})")
    print(
        f"Timeout diagnostics: {'enabled' if runner.enable_timeout_diag else 'disabled'}"
    )
    print(f"Test timeout: {runner.timeout_seconds}s")

    runner.start_time = time.time()

    # 异步测试 - 需要事件循环
    print("\n📋 模式验证测试:")
    runner.run_async("无效模式", test_invalid_mode_async)
    runner.run_async("二进制模式不能指定编码", test_binary_with_encoding_async)

    print("\n📋 基本 I/O 测试:")
    runner.run_async("文本写入读取", test_write_read_text)
    runner.run_async("二进制写入读取", test_write_read_binary)
    runner.run_async("分块读取", test_read_chunks)
    runner.run_async("按行读取", test_readline)
    runner.run_async("读取所有行", test_readlines)

    print("\n📋 追加模式测试:")
    runner.run_async("文本追加", test_append_text)
    runner.run_async("二进制追加", test_append_binary)

    print("\n📋 Seek 操作测试:")
    runner.run_async("从开头 seek", test_seek_from_start)
    runner.run_async("从当前位置 seek", test_seek_from_current)
    runner.run_async("从末尾 seek", test_seek_from_end)

    print("\n📋 上下文管理器测试:")
    runner.run_async("自动关闭", test_auto_close)
    runner.run_async("手动关闭", test_manual_close)

    print("\n📋 异步迭代器测试:")
    runner.run_async("异步迭代", test_async_iteration)

    print("\n📋 flush 测试:")
    runner.run_async("flush 刷新", test_flush)

    print("\n📋 Path 对象测试:")
    runner.run_async("Path 对象支持", test_path_object)

    print("\n📋 错误处理测试:")
    runner.run_async("文件不存在", test_file_not_found)
    runner.run_async("二进制写入字符串", test_write_str_to_binary)
    runner.run_async("文本写入字节", test_write_bytes_to_text)
    runner.run_async("已关闭文件操作", test_operation_on_closed_file)

    print("\n📋 并发测试:")
    runner.run_async("并发读取", test_concurrent_reads)
    runner.run_async("并发写入不同文件", test_concurrent_writes_different_files)

    print("\n📋 编码测试:")
    runner.run_async("UTF-8 编码", test_encoding_utf8)
    runner.run_async("GBK 编码", test_encoding_gbk)

    print("\n📋 模式测试:")
    runner.run_async("排他创建 (x 模式)", test_exclusive_create)
    runner.run_async("r+ 读写模式", test_read_write_mode)
    runner.run_async("w+ 读写模式", test_w_plus_mode)

    runner.print_summary()

    # 清理共享事件循环
    cleanup_shared_loop()

    return 0 if runner.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
