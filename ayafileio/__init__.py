import sys
import locale
from pathlib import Path

from ._ayafileio import (
    AsyncFile as _AsyncFile,
    set_handle_pool_limits as _set_handle_pool_limits,
    get_handle_pool_limits as _get_handle_pool_limits,
)
# 尝试导入 Windows 专用的 set_iocp_worker_count（如果扩展在该平台上提供）
_has_native_set_iocp = False
try:
    if sys.platform == "win32":
        from ._ayafileio import set_iocp_worker_count as _set_iocp_worker_count
        _has_native_set_iocp = True
except Exception:
    _has_native_set_iocp = False

# 导入本机 cleanup（若存在）
try:
    from ._ayafileio import cleanup as _native_cleanup
except Exception:
    _native_cleanup = None

_DEFAULT_READLINE_BUF = 65536  # 64 KB – much faster than 4 KB for large files


def set_handle_pool_limits(max_per_key: int, max_total: int) -> None:
    """设置句柄池容量限制。"""
    if max_per_key <= 0 or max_total <= 0:
        raise ValueError("max_per_key and max_total must be positive integers")
    _set_handle_pool_limits(max_per_key, max_total)


def get_handle_pool_limits() -> tuple[int, int]:
    """获取当前句柄池容量限制 (max_per_key,max_total)。"""
    return _get_handle_pool_limits()


def set_io_worker_count(count: int = 0) -> None:
    """通用的设置 I/O worker 数量的接口（跨平台）。

    在 Windows 上这会委托给底层的 `set_iocp_worker_count`；在非 Windows 平台
    暂时仅做参数校验并记录到模块状态，供后续后端实现使用。

    Args:
        count: 0=自动（推荐），1-128=固定数量
    """
    if not isinstance(count, int):
        raise TypeError("count must be int")
    if not (count == 0 or (1 <= count <= 128)):
        raise ValueError("worker count must be 0 (auto) or 1-128")
    if _has_native_set_iocp:
        _set_iocp_worker_count(count) # type: ignore
    else:
        # 非 Windows 平台：记录到模块变量，供未来后端使用或查询
        globals()['_io_worker_count'] = count


def set_iocp_worker_count(count: int = 0) -> None:
    """兼容旧 API 的别名，指向 `set_io_worker_count`。"""
    set_io_worker_count(count)


def _register_native_cleanup() -> None:
    """在 Python 层统一注册本机清理逻辑，避免直接在本机层注册 atexit 导致的潜在不安全行为。"""
    if _native_cleanup is None:
        return

    try:
        import atexit as _atexit

        def _cleanup_wrapper() -> None:
            try:
                _native_cleanup() # type: ignore
            except Exception:
                # 在解释器退出期间忽略异常，避免抛出
                pass

        _atexit.register(_cleanup_wrapper)
    except Exception:
        # 极少情况：若无法导入 atexit，则静默忽略
        pass


# 执行注册
_register_native_cleanup()


class AsyncFile:
    """跨平台异步文件对象。

    支持模式: r/rb/w/wb/a/ab/x/xb 及 + 组合。
    指定 encoding 时自动处理文本编解码（底层始终以二进制操作）。
    """

    __slots__ = (
        "_impl",
        "_path",
        "_is_text",
        "_encoding",
        "_line_buffer",
        "_closed",
    )

    def __init__(
        self,
        path: str | Path,
        mode: str = "rb",
        encoding: str | None = None,
    ) -> None:
        self._path = str(path)
        self._closed = False

        # ── 文本 / 二进制模式判断 ──────────────────────────────────────────
        self._is_text = "b" not in mode

        if self._is_text:
            self._encoding = encoding or locale.getpreferredencoding(False)
        else:
            if encoding is not None:
                raise ValueError("Binary mode does not accept an encoding argument.")
            self._encoding = None

        # ── 规范化传给 C++ 的模式（始终二进制）────────────────────────────
        clean = mode.replace("t", "")
        if "b" not in clean:
            has_plus = "+" in clean
            base_char = next((c for c in clean if c in "rwax"), None)
            if not base_char:
                raise ValueError(f"Invalid mode: '{mode}'")
            clean = base_char + ("+" if has_plus else "") + "b"

        self._impl = _AsyncFile(self._path, clean)
        self._line_buffer = b""

    # ── context manager ───────────────────────────────────────────────────────

    async def __aenter__(self) -> "AsyncFile":
        return self

    async def __aexit__(self, *_) -> None:
        await self.close()

    # ── async iterator ────────────────────────────────────────────────────────

    def __aiter__(self) -> "AsyncFile":
        return self

    async def __anext__(self) -> str | bytes:
        line = await self.readline()
        if not line:
            raise StopAsyncIteration
        return line

    # ── read ──────────────────────────────────────────────────────────────────

    async def read(self, size: int = -1) -> str | bytes:
        data: bytes = await self._impl.read(size)
        if not data:
            return "" if self._is_text else b""
        return data.decode(self._encoding) if self._is_text else data  # type: ignore

    async def readline(self) -> str | bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        sep = b"\n"
        while True:
            idx = self._line_buffer.find(sep)
            if idx != -1:
                line, self._line_buffer = (
                    self._line_buffer[: idx + 1],
                    self._line_buffer[idx + 1 :],
                )
                return line.decode(self._encoding) if self._is_text else line  # type: ignore

            chunk: bytes = await self._impl.read(_DEFAULT_READLINE_BUF)
            if not chunk:
                if self._line_buffer:
                    out, self._line_buffer = self._line_buffer, b""
                    return out.decode(self._encoding) if self._is_text else out  # type: ignore
                return "" if self._is_text else b""
            self._line_buffer += chunk

    async def readlines(self, hint: int = -1) -> list[str | bytes]:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        lines = []
        total = 0
        while True:
            line = await self.readline()
            if not line:
                break
            lines.append(line)
            if hint > 0:
                total += len(line)
                if total >= hint:
                    break
        return lines

    # ── write ─────────────────────────────────────────────────────────────────

    async def write(self, data: str | bytes | bytearray | memoryview) -> int:
        if self._is_text:
            if not isinstance(data, str):
                raise TypeError("Text mode requires str input.")
            raw: bytes = data.encode(self._encoding)  # type: ignore
        else:
            if isinstance(data, str):
                raise TypeError("Binary mode requires bytes-like input, not str.")
            # Pass memoryview/bytearray directly – C++ accepts any buffer protocol
            raw = data  # type: ignore[assignment]
        return await self._impl.write(raw)

    # ── seek / flush / close ──────────────────────────────────────────────────

    async def seek(self, offset: int, whence: int = 0) -> int:
        return await self._impl.seek(offset, whence)

    async def flush(self) -> None:
        await self._impl.flush()

    async def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._impl.close_impl()  # synchronous — no future needed

    # ── properties ────────────────────────────────────────────────────────────

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def name(self) -> str:
        return self._path

    @property
    def mode(self) -> str:
        return "t" if self._is_text else "b"

    @classmethod
    def open(
        cls,
        path: str | Path,
        mode: str = "rb",
        encoding: str | None = None,
    ) -> "AsyncFile":
        return cls(path, mode, encoding)


def open(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
) -> AsyncFile:
    """打开一个 AsyncFile 实例，用法与内置 open() 类似。

    示例::

        async with ayafileio.open('data.bin', 'rb') as f:
            data = await f.read()

        async with ayafileio.open('log.txt', 'w', encoding='utf-8') as f:
            await f.write('hello')
    """
    return AsyncFile(path, mode, encoding)
