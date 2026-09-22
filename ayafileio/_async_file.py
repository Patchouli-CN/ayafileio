"""异步文件对象"""

import asyncio
import os
import locale
from pathlib import Path
from collections.abc import AsyncGenerator, Iterable
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


def _split_complete_tail(raw: bytes, encoding: str) -> tuple[bytes, bytes]:
    """把 *raw* 切成「字符完整部分」与「结尾残缺字符部分」。

    用 strict 解码探测：只有结尾处被 chunk 边界切出来的残缺字符
    （`unexpected end of data`）算残缺、需要回推；数据本身非法
    （`invalid start byte` 等）不算残缺——整体交还，由调用方的
    errors 模式处理。空输入返回 `(b"", b"")`。

    Args:
        raw: 一段原始字节（可能以残缺字符结尾）
        encoding: 编码名

    Returns:
        tuple[bytes, bytes]: (完整部分, 结尾残缺部分)
    """
    if not raw:
        return b"", b""
    try:
        raw.decode(encoding, "strict")
        return raw, b""
    except UnicodeDecodeError as error:
        if error.reason == "unexpected end of data":
            return raw[: error.start], raw[error.start :]
        return raw, b""


async def _gather_io(requests):
    """Submit native Futures directly; drain submitted I/O before reporting errors."""
    pending = []
    try:
        for request in requests:
            pending.append(request)
    except BaseException:
        if pending:
            await asyncio.gather(*pending, return_exceptions=True)
        raise
    results = await asyncio.gather(*pending, return_exceptions=True)
    for result in results:
        if isinstance(result, BaseException):
            raise result
    return results


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
        """读取内容。

        文本模式下 `size` 是**字节预算**而非字符数（1.5.3 起明确的行为）：
        返回该预算内**完整字符**对应的文本，绝不在多字节字符中间下刀——
        结尾残缺的字节会被推回（impl 位置回退 / 预读缓冲回退），后续读取
        接着用。ASCII 下字节即字符，与旧行为完全一致。需要精确字符数请
        用 `readline()` 或 `read(-1)`（两者本来就按行/按整体边界解码）。

        Args:
            size: 字节预算；<0 表示读到 EOF

        Returns:
            T: 文本（文本模式）或字节（二进制模式）
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text and size >= 0:
            return await self._read_text_bounded(size)  # type: ignore[return-value]
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

    async def _read_text_bounded(self, size: int) -> str:
        """文本模式定长读：size 为字节预算，只返回完整字符（1.5.3 修复）。

        此前直接把 size 字节交给解码器：多字节编码下会切在字符中间，
        strict 抛 UnicodeDecodeError、replace 则静默插入 U+FFFD（数据被
        悄悄污染，最危险的形态）。现在：

        1. **窥视**预读缓冲（只推进消费位、不销毁字节），再从 impl 补齐
           剩余预算；
        2. 对合并字节做「结尾残缺字符」探测，残缺部分推回——
           impl 段用 seek 回退，行缓冲段把消费位拨回去（字节还在，
           后续 readline/read 接着用，不丢数据）；
        3. 预算连一个字符都装不下时（如 read(1) 遇 3 字节汉字）有界补读，
           至少返回一个完整字符。
        """
        if size <= 0 and not self._buffered():
            return ""

        avail = self._buffered()
        line_len = min(avail, size)
        line_raw = bytes(self._line_buffer[self._line_pos : self._line_pos + line_len])
        budget = size - line_len
        impl_raw = await self._impl.read(budget) if budget > 0 else b""

        complete, tail = _split_complete_tail(line_raw + impl_raw, self._encoding)
        attempt = 0
        while not complete and (line_raw or impl_raw) and attempt < 2:
            more = await self._impl.read(4)  # 预算装不下一个字符：补读几个字节
            if not more:
                break  # EOF：文件本身以残缺字节结尾
            impl_raw += more
            attempt += 1
            complete, tail = _split_complete_tail(line_raw + impl_raw, self._encoding)

        if tail:
            impl_push = min(len(tail), len(impl_raw))
            if impl_push:
                await self._impl.seek(-impl_push, 1)
            line_push = len(tail) - impl_push
        else:
            line_push = 0

        # 消费位只前进「完整字符」对应的部分（推回的字节留给下次）
        self._line_pos += line_len - line_push
        if self._line_pos and self._line_pos >= len(self._line_buffer):
            del self._line_buffer[:]  # 全部消费完，清理消费过的前缀
            self._line_pos = 0

        return _translate_for_read(
            complete.decode(self._encoding, errors=self._errors), self._newline
        )

    async def readline(self) -> str | bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

        buf = self._line_buffer
        newline = self._newline if self._is_text else "\n"
        scan_pos = self._line_pos
        while True:
            sep_start, sep_len = _find_line_end(
                buf, scan_pos, newline, eof=False
            )
            if sep_start != -1:
                # Include the separator in the returned line.
                line = self._take_buffered(sep_start + sep_len - self._line_pos)
                if self._is_text:
                    text = line.decode(self._encoding, errors=self._errors)
                    return _translate_for_read(text, newline)
                return bytes(line)

            # Only rescan the final byte: a CRLF may straddle chunks.
            scan_pos = max(self._line_pos, len(buf) - 1)
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
                scan_pos -= self._line_pos
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

    async def read_at(self, offset: int, size: int = -1) -> bytes:
        """位置读（POSIX pread 语义）：从 *offset* 读取最多 *size* 字节。

        不改变文件的逻辑位置——``tell()`` 在调用前后不变，与并发的
        ``read()``/``seek()`` 无竞争。随机读不再需要 ``seek()`` + ``read()``
        两步协程往返。

        语义：

        - ``size < 0``：从 *offset* 一直读到文件末尾。
        - 读到 EOF 时截断返回短数据；``offset`` 超出文件大小返回 ``b""``。
        - 不触碰 readline 预读缓冲（``_line_buffer``）。

        Raises:
            ValueError: 文本模式（位置读与解码/换行翻译语义不兼容）、
                        文件已关闭、或 ``offset < 0``。
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("read_at() only supports binary mode")
        return await self._impl.read_at(offset, size)

    async def read_many(self, spans: Iterable[tuple[int, int]]) -> list[bytes]:
        """批量位置读：一次提交多个 ``(offset, size)`` 读请求。

        返回与 *spans* 同序的 ``bytes`` 列表（``asyncio.gather`` 保序）。
        每个 span 的语义同 :meth:`read_at`；``size < 0`` 表示读到 EOF。

        设计意图：对标模型权重流式加载的按层批量读场景。``gather`` 让所有
        I/O 在首个挂起点之前全部提交给后端，共享同一个事件循环周期；C++ 层
        的 ResultBatcher 会把一批完成通知聚合成少量
        ``loop.call_soon_threadsafe`` 回调，比逐个 ``await read_at(...)``
        少 N-1 次协程往返。直接聚合底层 Future，避免为每个 span 创建 Task。
        提交失败或 I/O 报错时，等待已提交的请求完成后再抛出异常。

        Raises:
            ValueError: 文本模式、文件已关闭、或任一 ``offset < 0``。
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("read_many() only supports binary mode")
        return await _gather_io(self._impl.read_at(o, s) for o, s in spans)

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

    async def write_at(self, offset: int, data: bytes | bytearray | memoryview) -> int:
        """Write bytes at an absolute offset without changing tell().

        Binary, non-append files only. Returns the number of bytes written;
        a short write is possible, as with write(). Overlapping concurrent
        writes have unspecified ordering. Read-ahead is invalidated first.
        """
        self._check_positioned_write()
        await self._rewind_readahead()
        return await self._impl.write_at(offset, data)

    def _check_positioned_write(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text:
            raise ValueError("positioned writes only support binary mode")
        if "a" in self._mode:
            raise ValueError("positioned writes do not support append mode")
        if not self.writable():
            raise OSError("File not open for writing")

    async def write_many(
        self, writes: Iterable[tuple[int, bytes | bytearray | memoryview]]
    ) -> list[int]:
        """Submit (offset, data) writes concurrently, returning counts in order.

        Semantics match write_at(). This is not an atomic transaction: an
        error can leave other writes committed. Submitted I/O is drained
        before an error is raised. Avoid overlapping ranges when ordering
        matters. Mutable buffers are copied by the backend on submission.
        """
        self._check_positioned_write()
        await self._rewind_readahead()
        return await _gather_io(self._impl.write_at(o, data) for o, data in writes)

    # ── seek / flush / close / tell 等 ──────────────────────────────────────────────────

    async def seek(self, offset: int, whence: int = 0) -> int:
        """移动文件逻辑位置，返回新的绝对位置。

        **文本模式安全策略（1.5.2 起，纯 Python 层，零后端改动）**：
        只允许 `seek(0)`（whence=0，回到开头）。文本模式的裸字节偏移可能落在
        多字节字符中间，导致下次 `read()` 抛 `UnicodeDecodeError`（strict）
        或静默插入 U+FFFD（errors="replace"）——后者尤其危险：数据被悄悄
        污染而不报错。因此文本模式拒绝一切非零偏移与非 SEEK_SET 模式。
        二进制模式不受限制（位置访问本就是 `read_at`/`write_at` 的原生场景）；
        需要位置访问请以二进制模式打开文件。

        Args:
            offset: 目标偏移（文本模式必须为 0）
            whence: 0=从开头（SEEK_SET），1=从当前位置，2=从末尾
                （文本模式只接受 0）

        Returns:
            int: 新的绝对位置

        Raises:
            ValueError: 文件已关闭；或文本模式下 seek 到非零位置
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._is_text and not (whence == 0 and offset == 0):
            raise ValueError(
                "text mode only supports seek(0) (back to start); arbitrary byte "
                "offsets may land mid-character and silently corrupt decoding. "
                "Open the file in binary mode for positional access."
            )
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
