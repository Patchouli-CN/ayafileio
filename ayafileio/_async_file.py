"""异步文件对象"""

import os
import locale
from pathlib import Path
from typing import Generic, TypeVar
from ._ayafileio import AsyncFile as _AsyncFile

_DEFAULT_READLINE_BUF = 65536  # 64 KB – much faster than 4 KB for large files

T = TypeVar("T", str, bytes)


class AsyncFile(Generic[T]):
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
        "_newline",
        "_errors",
        "_mode",
    )

    def __init__(
        self,
        path: str | Path,
        mode: str = "rb",
        encoding: str | None = None,
        newline: str | None = None,
        errors: str | None = None,
    ) -> None:

        self._path = str(path)
        self._closed = False

        self._newline = newline
        self._errors = errors or "strict"

        # ── 文本 / 二进制模式判断 ──────────────────────────────────────────
        self._mode = ""

        self._is_text = "b" not in mode

        if self._is_text:
            self._encoding = encoding or locale.getpreferredencoding(False)
        else:
            if encoding is not None:
                raise ValueError("Binary mode does not accept an encoding argument.")
            self._encoding = None

        # ── 规范化传给 C++ 的模式（始终二进制）────────────────────────────
        valid_chars = set("rwaxbt+")
        if any(c not in valid_chars for c in mode):
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
        self._line_buffer = b""

    # ── context manager ───────────────────────────────────────────────────────

    async def __aenter__(self) -> "AsyncFile[T]":
        return self

    async def __aexit__(self, *_) -> None:
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

    async def read(self, size: int = -1) -> T:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        data: bytes = await self._impl.read(size)
        if not data:
            return "" if self._is_text else b""  # type: ignore[return-value]
        if self._is_text:
            return data.decode(self._encoding, errors=self._errors)  # type: ignore[return-value]
        return data  # type: ignore[return-value]

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
                if self._is_text:
                    text = line.decode(self._encoding, errors=self._errors)  # type: ignore reason: 同下
                    # 处理 newline 参数
                    if self._newline is not None and self._newline != "\n":
                        text = (
                            text.replace("\n", self._newline)
                            if self._newline != ""
                            else text.replace("\n", "")
                        )
                    return text
                return line

            chunk: bytes = await self._impl.read(_DEFAULT_READLINE_BUF)
            if not chunk:
                if self._line_buffer:
                    out, self._line_buffer = self._line_buffer, b""
                    if self._is_text:
                        text = out.decode(self._encoding, errors=self._errors)  # type: ignore reason: self._encoding一定是str
                        if self._newline is not None and self._newline != "\n":
                            text = (
                                text.replace("\n", self._newline)
                                if self._newline != ""
                                else text.replace("\n", "")
                            )
                        return text
                    return out
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

    async def writelines(self, lines) -> None:
        """批量写入多行。"""
        for line in lines:
            await self.write(line)

    async def readall(self) -> str | bytes:
        """读取整个文件。"""
        return await self.read(-1)

    async def readinto(self, buf: bytearray | memoryview) -> int:
        """零拷贝读取到预分配缓冲区，返回读取字节数。"""
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("readinto() only supports binary mode")
        return await self._impl.readinto(buf)

    # ── write ─────────────────────────────────────────────────────────────────

    async def write(self, data: str | bytes | bytearray | memoryview) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

        if self._is_text:
            if not isinstance(data, str):
                raise TypeError("Text mode requires str input.")
            raw: bytes = data.encode(self._encoding, errors=self._errors)  # type: ignore
        else:
            if isinstance(data, str):
                raise TypeError("Binary mode requires bytes-like input, not str.")
            # Pass memoryview/bytearray directly – C++ accepts any buffer protocol
            raw = data  # type: ignore[assignment]
        return await self._impl.write(raw)

    # ── seek / flush / close / tell 等 ──────────────────────────────────────────────────

    async def seek(self, offset: int, whence: int = 0) -> int:
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
        return await self._impl.tell()

    async def truncate(self, size: int) -> None:
        """截断文件到指定大小。"""
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if size < 0:
            raise ValueError("negative size not allowed")
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
    ) -> "AsyncFile":
        """类方法方式打开文件，等同 `AsyncFile(path, mode, encoding)`"""
        return cls(path, mode, encoding, newline, errors)

    @classmethod
    def _from_impl(cls, impl: _AsyncFile) -> "AsyncFile[T]":
        """从 C++ 层对象创建 AsyncFile（内部使用）"""
        instance = object.__new__(cls)
        instance._impl = impl
        instance._path = "<fd>"
        instance._is_text = False
        instance._encoding = None
        instance._line_buffer = b""
        instance._closed = False
        instance._newline = None
        instance._errors = "strict"
        return instance
