"""异步文件对象"""

import os
import locale
from pathlib import Path
from collections.abc import AsyncGenerator
from typing import Generic, TypeVar
from ._ayafileio import AsyncFile as _AsyncFile

_DEFAULT_READLINE_BUF = 65536  # 64 KB – much faster than 4 KB for large files

_VALID_MODE_CHARS = frozenset("rwaxbt+")

T = TypeVar("T", str, bytes)


# ── newline helpers ─────────────────────────────────────────────────────────
# Python open() newline semantics:
#   None  : universal mode; translate any of \r\n, \r, \n to \n on input,
#           and translate \n to os.linesep on output.
#   ""    : universal mode; recognize \r\n, \r, \n as line terminators but
#           do not translate them.
#   "\n"  : line terminator is \n only; no translation.
#   "\r"  : line terminator is \r only; write translates \n -> \r.
#   "\r\n": line terminator is \r\n only; write translates \n -> \r\n.


def _find_line_end(
    buf: bytearray, start: int, newline: str | None, eof: bool
) -> tuple[int, int]:
    """Locate the first complete line ending in *buf* at or after *start*.

    Returns ``(sep_start, sep_len)``. ``sep_start`` is the index of the first
    byte of the separator; ``sep_len`` is its length. Returns ``(-1, 0)`` when
    no complete terminator is present.

    For universal newline modes, a trailing ``\\r`` at the very end of *buf*
    is not treated as a line ending unless *eof* is ``True``, because it may
    be the first half of a ``\\r\\n`` pair split across read chunks.
    """
    if newline is None or newline == "":
        # Universal newline mode: the terminator is whichever of \n / \r /
        # \r\n appears first. Only a \r *before* the first \n can matter, so
        # bound the \r scan by the \n position to keep both scans O(line).
        nl = buf.find(b"\n", start)
        if nl == -1:
            cr = buf.find(b"\r", start)
        else:
            cr = buf.find(b"\r", start, nl)
        if cr == -1:
            return (nl, 1) if nl != -1 else (-1, 0)
        if cr + 1 < len(buf):
            return (cr, 2) if buf[cr + 1] == 10 else (cr, 1)
        # A lone \r at the buffer end may be a split \r\n.
        return (cr, 1) if eof else (-1, 0)
    if newline == "\r\n":
        idx = buf.find(b"\r\n", start)
        return (idx, 2) if idx != -1 else (-1, 0)
    # newline == "\n" or "\r"
    idx = buf.find(b"\n" if newline == "\n" else b"\r", start)
    return (idx, 1) if idx != -1 else (-1, 0)


def _translate_for_read(text: str, newline: str | None) -> str:
    """Apply input newline translation for text mode."""
    if newline is None:
        # Universal mode: normalize all line endings to \n.
        return text.replace("\r\n", "\n").replace("\r", "\n")
    return text


def _translate_for_write(text: str, newline: str | None) -> str:
    """Apply output newline translation for text mode."""
    if newline is None:
        return text.replace("\n", os.linesep)
    if newline == "\r":
        return text.replace("\n", "\r")
    if newline == "\r\n":
        return text.replace("\n", "\r\n")
    return text


class AsyncFile(Generic[T]):
    """跨平台异步文件对象。

    支持模式: r/rb/w/wb/a/ab/x/xb 及 + 组合。
    指定 encoding 时自动处理文本编解码（底层始终以二进制操作）。

    ``newline`` 参数遵循 Python 内置 ``open()`` 的约定：

    - ``None``（默认）：通用换行模式；读取时把 ``\\r\\n``、``\\r`` 都转为 ``\\n``，
      写入时把 ``\\n`` 转为 ``os.linesep``。
    - ``""``：通用换行模式但不翻译；``\\r\\n``、``\\r``、``\\n`` 都被识别为行尾，
      但原样返回。
    - ``"\\n"``、``"\\r"``、``"\\r\\n"``：只把对应字符串当作行尾；写入时会把
      ``\\n`` 翻译为指定的行尾。
    """

    __slots__ = (
        "_impl",
        "_path",
        "_is_text",
        "_encoding",
        "_line_buffer",
        "_line_pos",
        "_closed",
        "_newline",
        "_errors",
        "_mode",
        "_auto_flush"
    )

    def __init__(
        self,
        path: str | Path,
        mode: str = "rb",
        encoding: str | None = None,
        newline: str | None = None,
        errors: str | None = None,
        auto_flush: bool = False
    ) -> None:

        self._path = str(path)
        self._closed = False

        self._newline = newline
        self._errors = errors or "strict"
        self._auto_flush = auto_flush

        # ── 文本 / 二进制模式判断 ──────────────────────────────────────────
        self._mode = ""

        self._is_text = "b" not in mode

        if self._is_text:
            self._encoding = encoding or locale.getpreferredencoding(False)
        else:
            if encoding is not None:
                raise ValueError("Binary mode does not accept an encoding argument.")
            if newline is not None:
                raise ValueError("Binary mode does not accept a newline argument.")
            self._encoding = "utf-8"

        # ── 规范化传给 C++ 的模式（始终二进制）────────────────────────────
        if any(c not in _VALID_MODE_CHARS for c in mode):
            raise ValueError(f"Invalid mode: '{mode}'")

        clean_mode = mode.replace("t", "")
        if "b" not in clean_mode:
            has_plus = "+" in clean_mode
            base_char = next((c for c in clean_mode if c in "rwax"), None)
            if not base_char:
                raise ValueError(f"Invalid mode: '{mode}'")
            clean_mode = base_char + ("+" if has_plus else "") + "b"

        self._mode = clean_mode
        self._impl = _AsyncFile(self._path, clean_mode)
        self._line_buffer = bytearray()
        self._line_pos = 0

    # ── context manager ───────────────────────────────────────────────────────

    async def __aenter__(self) -> "AsyncFile[T]":
        return self

    async def __aexit__(self, *_) -> None:
        if self._auto_flush:
            await self.flush() # 如果要确保数据真的在硬盘了
            
        await self.close()

    # ── async iterator ────────────────────────────────────────────────────────

    def __aiter__(self) -> "AsyncFile[T]":
        return self

    async def __anext__(self) -> T:
        line = await self.readline()
        if not line:
            raise StopAsyncIteration
        return line  # type: ignore

    # ── read ──────────────────────────────────────────────────────────────────

    def _buffered(self) -> int:
        """readline 预读缓冲中尚未消费的字节数。"""
        return len(self._line_buffer) - self._line_pos

    def _take_buffered(self, n: int = -1) -> bytearray:
        """从预读缓冲取出最多 *n* 字节（-1 = 全部）并推进消费位置。"""
        buf, pos = self._line_buffer, self._line_pos
        end = len(buf) if n < 0 else min(pos + n, len(buf))
        out = buf[pos:end]
        if end == len(buf):
            del buf[:]
            self._line_pos = 0
        else:
            self._line_pos = end
        return out

    async def _rewind_readahead(self) -> None:
        """丢弃预读缓冲，并把底层文件位置回退到逻辑位置。

        在 write/truncate 前调用，保证混用 readline 与写操作时
        位置语义与内置 ``open()`` 一致。
        """
        n = self._buffered()
        if n:
            del self._line_buffer[:]
            self._line_pos = 0
            await self._impl.seek(-n, 1)

    async def read(self, size: int = -1) -> T:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._buffered():
            # 先消费 readline 的预读缓冲，避免丢数据
            prefix = self._take_buffered(size)
            if size < 0 or len(prefix) < size:
                rest = await self._impl.read(-1 if size < 0 else size - len(prefix))
                data = bytes(prefix) + rest
            else:
                data = bytes(prefix)
        else:
            data = await self._impl.read(size)
        if not data:
            return "" if self._is_text else b""  # type: ignore[return-value]
        if self._is_text:
            text = data.decode(self._encoding, errors=self._errors)
            return _translate_for_read(text, self._newline)  # type: ignore[return-value]
        return data  # type: ignore[return-value]

    async def readline(self) -> str | bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

        buf = self._line_buffer
        newline = self._newline
        while True:
            sep_start, sep_len = _find_line_end(
                buf, self._line_pos, newline, eof=False
            )
            if sep_start != -1:
                # Include the separator in the returned line.
                line = self._take_buffered(sep_start + sep_len - self._line_pos)
                if self._is_text:
                    text = line.decode(self._encoding, errors=self._errors)
                    return _translate_for_read(text, newline)
                return bytes(line)

            chunk: bytes = await self._impl.read(_DEFAULT_READLINE_BUF)
            if not chunk:
                # EOF: flush whatever remains in the line buffer.
                if self._buffered():
                    out = self._take_buffered()
                    # On EOF a trailing \\r is definitely a terminator.
                    if newline is None and out and out[-1] == 13:
                        out.append(10)  # canonicalize to include a \\n
                    if self._is_text:
                        text = out.decode(self._encoding, errors=self._errors)
                        return _translate_for_read(text, newline)
                    return bytes(out)
                return "" if self._is_text else b""
            # 追加新数据前先丢弃已消费的前缀，避免缓冲无限增长
            if self._line_pos:
                del buf[: self._line_pos]
                self._line_pos = 0
            buf.extend(chunk)

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

    async def writelines(self, lines) -> None:
        """批量写入多行。"""
        if not lines:
            return
        if self._is_text:
            await self.write("".join(lines))
        else:
            await self.write(b"".join(lines))

    async def readall(self) -> str | bytes:
        """读取整个文件。"""
        return await self.read(-1)

    async def readinto(self, buf: bytearray | memoryview) -> int:
        """零拷贝读取到预分配缓冲区，返回读取字节数。"""
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("readinto() only supports binary mode")
        if self._buffered():
            # 先吐出 readline 的预读数据，保持流位置一致
            pending = self._take_buffered(len(buf))
            n = len(pending)
            memoryview(buf)[:n] = pending
            return n
        return await self._impl.readinto(buf)

    async def chunk(
        self,
        chunk_size: int,
        *,
        buf: bytearray | memoryview | None = None,
    ) -> AsyncGenerator[memoryview, None]:
        """流式读取文件，每次返回一个固定大小的内存块（零拷贝）。

        底层使用 ``readinto`` 直接写入缓冲区，避免每次迭代分配新内存。
        适用于大文件流式处理、网络上传分片等场景。

        Args:
            chunk_size: 每次读取的最大字节数。若 ``buf`` 容量更小则自动取两者最小值。
            buf: 可选的预分配缓冲区（``bytearray`` 或可写 ``memoryview``）。
                 提供时在所有迭代间复用此缓冲区（零额外分配）；
                 为 ``None`` 时内部自动分配 ``bytearray(chunk_size)``。

        Yields:
            ``memoryview`` — 指向缓冲区中本次读取数据的内存视图。
            该视图仅在**下一次迭代前**有效——请及时消费每个 chunk，
            不要跨迭代持有引用。

        Raises:
            ValueError: 文本模式、文件已关闭、或 chunk_size <= 0。

        Example:
            >>> # 内置缓冲区（最简单）
            >>> async for chunk in f.chunk(4096):
            ...     process(chunk)  # chunk is memoryview

            >>> # 预分配缓冲区（高频场景更高效）
            >>> buf = bytearray(65536)
            >>> async for chunk in f.chunk(4096, buf=buf):
            ...     sock.send(chunk)  # 零拷贝发送
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("chunk() only supports binary mode. Use readline() for text.")
        if chunk_size <= 0:
            raise ValueError("chunk_size must be positive")

        if buf is None:
            buf = bytearray(chunk_size)
        else:
            # 若用户提供的缓冲区小于 chunk_size，以缓冲区容量为准
            buf_len = len(buf)  # type: ignore[arg-type]
            if buf_len < chunk_size:
                chunk_size = buf_len

        mv = memoryview(buf)
        while True:
            # 用切片限制 readinto 最多写入 chunk_size 字节
            n = await self.readinto(mv[:chunk_size])
            if n == 0:
                return
            yield mv[:n]

    # ── write ─────────────────────────────────────────────────────────────────

    async def write(self, data: str | bytes | bytearray | memoryview) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

        if self._is_text:
            if not isinstance(data, str):
                raise TypeError("Text mode requires str input.")
            raw: bytes = _translate_for_write(data, self._newline).encode(
                self._encoding, errors=self._errors
            )  # type: ignore
        else:
            if isinstance(data, str):
                raise TypeError("Binary mode requires bytes-like input, not str.")
            # Pass memoryview/bytearray directly – C++ accepts any buffer protocol
            raw = data  # type: ignore[assignment]
        await self._rewind_readahead()
        return await self._impl.write(raw)

    # ── seek / flush / close / tell 等 ──────────────────────────────────────────────────

    async def seek(self, offset: int, whence: int = 0) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        n = self._buffered()
        if n:
            if whence == 1:
                # 相对定位以逻辑位置（用户已消费到的位置）为基准
                offset -= n
            del self._line_buffer[:]
            self._line_pos = 0
        return await self._impl.seek(offset, whence)

    async def flush(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        await self._impl.flush()

    async def close(self) -> None:
        if not self._closed:
            self._closed = True
            await self._impl.close()

    async def tell(self) -> int:
        """返回当前文件位置。"""
        return await self._impl.tell() - self._buffered()

    async def truncate(self, size: int) -> None:
        """截断文件到指定大小。"""
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if size < 0:
            raise ValueError("negative size not allowed")
        await self._rewind_readahead()
        await self._impl.truncate(size)

    def _close_impl(self) -> None:
        """强制关闭函数（同步，供 atexit 等使用）"""
        if not self._closed:
            self._closed = True
        self._impl._close_impl()

    # ── readable / writeable / seekable ──────────────────────────────────────────────────

    def readable(self) -> bool:
        """文件是否可读。"""
        return "r" in self._mode or "+" in self._mode

    def writable(self) -> bool:
        """文件是否可写。"""
        return (
            "w" in self._mode
            or "a" in self._mode
            or "+" in self._mode
            or "x" in self._mode
        )

    def seekable(self) -> bool:
        """文件是否可随机访问。"""
        return True  # 所有常规文件都支持 seek

    # ── fileno / isatty ──────────────────────────────────────────────────

    def fileno(self) -> int:
        """返回底层文件描述符。"""
        return self._impl.fileno()

    def isatty(self) -> bool:
        """文件是否为 tty。"""
        try:
            return os.isatty(self.fileno())
        except OSError:
            return False

    # ── properties ────────────────────────────────────────────────────────────

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def name(self) -> str:
        return self._path

    @property
    def mode(self) -> str:
        return self._mode

    @classmethod
    def open(
        cls,
        path: str | Path,
        mode: str = "rb",
        encoding: str | None = None,
        newline: str | None = None,
        errors: str | None = None,
        auto_flush: bool = False,
    ) -> "AsyncFile":
        """类方法方式打开文件，等同 `AsyncFile(path, mode, encoding, ...)`"""
        return cls(path, mode, encoding, newline, errors, auto_flush)

    @classmethod
    def _from_impl(cls, impl: _AsyncFile, mode: str = "rb") -> "AsyncFile[T]":
        """从 C++ 层对象创建 AsyncFile（内部使用）"""
        instance = object.__new__(cls)
        instance._impl = impl
        instance._path = "<fd>"
        instance._is_text = False
        instance._encoding = None
        instance._line_buffer = bytearray()
        instance._line_pos = 0
        instance._closed = False
        instance._newline = None
        instance._errors = "strict"
        instance._mode = mode
        instance._auto_flush = False
        return instance
