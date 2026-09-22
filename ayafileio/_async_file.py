"""异步文件对象"""

import asyncio
import codecs
import os
import locale
from pathlib import Path
from collections import deque
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


def _translate_stream(text: str, newline: str | None) -> tuple[str, bool]:
    """流式换行翻译（通用模式 None 专用）：整段翻译，段尾孤立 \\r 挂起。

    挂起的 \\r 不翻译、本段不带出——下一段到达时拼回原文再判断是
    ``\\r`` 还是 ``\\r\\n``（跨 chunk 的 \\r\\n 不能被切成两个行尾）。
    非 None 模式不翻译（识别/终止语义由 seek/take_line 负责）。

    Args:
        text: 本段已解码文本（不含挂起 \\r）
        newline: 换行模式；仅 None 触发翻译

    Returns:
        tuple[str, bool]: (翻译后文本, 是否需挂起段尾 \\r)
    """
    if newline is not None or not text:
        return text, False
    if text.endswith("\r"):
        body, pending = text[:-1], True
    else:
        body, pending = text, False
    return body.replace("\r\n", "\n").replace("\r", "\n"), pending


def _find_line_end_text(text: str, newline: str | None) -> tuple[int, int]:
    """在已解码文本里找行尾，返回 (起始下标, 长度)；无则 (-1, 0)。

    `newline=None` 时文本已被翻译，行尾统一是 ``\\n``；其余模式按各自
    终止符查找（``""`` 模式认 ``\\r\\n``/``\\r``/``\\n`` 三种）。
    """
    if newline is None or newline == "\n":
        idx = text.find("\n")
        return (idx, 1) if idx != -1 else (-1, 0)
    if newline == "\r":
        idx = text.find("\r")
        return (idx, 1) if idx != -1 else (-1, 0)
    if newline == "\r\n":
        idx = text.find("\r\n")
        return (idx, 2) if idx != -1 else (-1, 0)
    # newline == ""：三种都认，取最先出现者
    best = (-1, 0)
    for sep in ("\r\n", "\r", "\n"):
        idx = text.find(sep)
        if idx != -1 and (best[0] == -1 or idx < best[0]):
            best = (idx, len(sep))
    return best


class _CharStream:
    """文本模式字符流 —— CPython TextIOWrapper 三层 I/O 栈思想的移植。

    部件与对应关系：

    - **增量解码**（对应 CPython 的 decoder）：`codecs` 增量解码器拼接跨
      chunk 的多字节序列；但残缺字节在进解码器**之前**就被
      `_split_complete_tail` 切出、留在 `_carry` 与下批拼接——解码器
      因此永远不持有内部缓冲，各编码行为一致，`errors` 模式
      （strict/replace/ignore）也全都正确。
    - **分段字符缓冲**（对应 CPython 的 `_decoded_chars`）：`deque` 段 +
      字节账本——feed 记入本段来源字节数，消费 k 个字符时按
      `len(段前缀.encode())` 扣减。未消费字节数始终可知 →
      `tell()` 保持**普通字节偏移**（CPython 因 \\r\\n→\\n 压缩只能返回
      不透明 cookie；我们按段记账，人类可读）。纯 \\n 行尾内容字节精确；
      \\r\\n 密集内容每处可能漂移 1 字节（压缩的固有问题，如实文档化）。
    - **换行翻译在入段时做**（对应 CPython：解码后、返回用户前）；
      通用 None / 不翻译的 "" / "\\r\\n" 模式下，段尾孤立 \\r 统一挂起
      一拍（下一段可能以 \\n 到达构成 \\r\\n）——段因此永不半截行尾，
      `take_line` 无需跨段拼接。
    - `read(n)` 按**翻译后字符数**计数，与 CPython 逐字符一致。
    """

    def __init__(self, encoding: str, errors: str, newline: str | None) -> None:
        """初始化。

        Args:
            encoding: 文本编码
            errors: 解码错误模式
            newline: 换行模式（同内置 open()）
        """
        self._encoding = encoding
        self._newline = newline
        self._errors = errors
        self._decoder = codecs.getincrementaldecoder(encoding)(errors)
        self._segments: deque[tuple[str, int]] = deque()
        """ (已翻译文本, 来源字节数) 分段队列 """
        self._pending_raw = 0
        """ 未消费字符对应的来源字节总数 """
        self._carry = b""
        """ 结尾残缺字节，与下一批读取拼接后再解码 """
        self._pending_cr = False
        """ 段尾孤立 \\r 挂起（下一段确认是 \\r 还是 \\r\\n） """
        self._eof = False

    # ---------------------------------------------------------------- 状态

    @property
    def eof(self) -> bool:
        """是否已读到 EOF 并收尾。"""
        return self._eof

    def buffered(self) -> int:
        """缓冲中可消费的字符数。"""
        return sum(len(text) for text, _ in self._segments)

    def byte_pending(self) -> int:
        """已读但未消费的字节数（carry + 未消费段 + 挂起的 \\r）。"""
        return self._pending_raw + len(self._carry) + (1 if self._pending_cr else 0)

    # ---------------------------------------------------------------- 喂入

    def feed(self, raw: bytes) -> None:
        """喂入一批从 impl 读到的原始字节。"""
        if not raw or self._eof:
            return
        if self._carry:
            raw = self._carry + raw
            self._carry = b""
        complete, tail = _split_complete_tail(raw, self._encoding)
        self._carry = tail
        if not complete:
            return

        text = self._decoder.decode(complete)
        if self._pending_cr:
            text = "\r" + text
            self._pending_cr = False

        pending_cr = False
        if self._newline is None:
            text, pending_cr = _translate_stream(text, None)
        elif self._newline in ("", "\r\n") and text.endswith("\r"):
            # 段尾 \r 歧义（可能是 \r\n 的前半）：挂起，不翻译模式原样带出
            text, pending_cr = text[:-1], True
        self._pending_cr = pending_cr

        if text:
            # 挂起的 \r 字节不计入本段（留在 byte_pending 的 +1 里）
            seg_raw = len(complete) - (1 if pending_cr else 0)
            self._segments.append((text, seg_raw))
            self._pending_raw += seg_raw

    def finish(self) -> str:
        """EOF 收尾：解出解码器与挂起 \\r 的剩余输出（末尾残缺按 errors 处理）。"""
        if self._eof:
            return ""
        self._eof = True
        raw_len = len(self._carry)
        text = self._decoder.decode(self._carry, final=True)
        self._carry = b""
        if self._pending_cr:
            text = "\r" + text
            self._pending_cr = False
        text, _ = _translate_stream(text, self._newline)
        if not text:
            return ""
        self._segments.append((text, raw_len))
        self._pending_raw += raw_len
        return text

    # ---------------------------------------------------------------- 消费

    def take(self, n: int) -> str:
        """取至多 *n* 个字符（跨段拼接，按段前缀字节扣账）。"""
        if n <= 0 or not self._segments:
            return ""
        out: list[str] = []
        remaining = n
        while remaining > 0 and self._segments:
            text, raw = self._segments[0]
            if len(text) <= remaining:
                out.append(text)
                remaining -= len(text)
                self._segments.popleft()
                self._pending_raw -= raw
            else:
                head = text[:remaining]
                consumed_raw = self._raw_len(head, raw, len(text))
                out.append(head)
                self._segments[0] = (text[remaining:], raw - consumed_raw)
                self._pending_raw -= consumed_raw
                remaining = 0
        return "".join(out)

    def take_all(self) -> str:
        """取走全部缓冲字符。"""
        out = "".join(text for text, _ in self._segments)
        self._segments.clear()
        self._pending_raw = 0
        return out

    def take_line(self) -> str | None:
        """取一行（含行尾）；无完整行返回 None。

        段尾不会出现歧义 \\r（feed 时已挂起），故行尾必在单段内找到，
        无需跨段拼接。
        """
        segments = self._segments
        for index, (text, raw) in enumerate(segments):
            sep_start, sep_len = _find_line_end_text(text, self._newline)
            if sep_start == -1:
                continue
            cut = sep_start + sep_len
            head_raw = self._raw_len(text[:cut], raw, len(text))
            if index == 0:
                rest = text[cut:]
                if rest:
                    segments[0] = (rest, raw - head_raw)
                else:
                    segments.popleft()
                self._pending_raw -= head_raw
                return text[:cut]

            # 行跨多个段：前 index 段整段 + 本段前缀
            head_texts = [t for t, _ in list(segments)[:index]]
            whole_raw = sum(r for _, r in list(segments)[:index])
            line = "".join(head_texts) + text[:cut]
            for _ in range(index):
                segments.popleft()
            self._pending_raw -= whole_raw + head_raw
            rest = text[cut:]
            if rest:
                segments[0] = (rest, raw - head_raw)
            else:
                segments.popleft()
            return line
        return None

    # ---------------------------------------------------------------- 重置

    def clear(self) -> None:
        """丢弃全部缓冲与挂起状态（seek(0) / 写前回退用）。"""
        self._segments.clear()
        self._pending_raw = 0
        self._carry = b""
        self._pending_cr = False
        self._eof = False
        self._decoder = codecs.getincrementaldecoder(self._encoding)(self._errors)

    def _raw_len(self, prefix: str, seg_raw: int, seg_len: int) -> int:
        """段内前缀 *prefix* 的来源字节数。

        UTF-8 等可 round-trip 的编码用 encode 精确计算；理论上失败的
        （有状态编码）退化为按字符数比例摊派。
        """
        if not prefix:
            return 0
        try:
            return len(prefix.encode(self._encoding, errors="strict"))
        except UnicodeEncodeError:
            return round(seg_raw * len(prefix) / seg_len) if seg_len else 0


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
        "_chars",
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
        # 文本模式的字符流（增量解码 + 分段字符缓冲）；二进制模式走裸字节缓冲
        self._chars = (
            _CharStream(self._encoding, self._errors, self._newline) if self._is_text else None
        )

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
        """丢弃预读内容，并把底层文件位置回退到逻辑位置。

        在 write/truncate 前调用，保证混用读取与写操作时
        位置语义与内置 ``open()`` 一致。文本模式回退字符流的字节账本，
        二进制模式回退预读缓冲。
        """
        if self._chars is not None:
            pending = self._chars.byte_pending()
            if pending:
                await self._impl.seek(-pending, 1)
            self._chars.clear()
            return
        n = self._buffered()
        if n:
            del self._line_buffer[:]
            self._line_pos = 0
            await self._impl.seek(-n, 1)

    async def read(self, size: int = -1) -> T:
        """读取内容。

        文本模式下 `size` 是**字符数**（1.6.0 起与 CPython `open()` 逐字符
        一致）：底层按 64KB 块读、增量解码、分段缓冲，按翻译后字符数
        计出——绝不在多字节字符中间下刀。`size < 0` 读到 EOF。
        二进制模式 `size` 为字节数。

        Args:
            size: 字符数（文本）/ 字节数（二进制）；<0 表示读到 EOF

        Returns:
            T: 文本（文本模式）或字节（二进制模式）
        """
        if self._closed:
            raise ValueError("I/O operation on closed file.")
        if self._chars is not None:
            return await self._read_text(size)  # type: ignore[return-value]
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

    async def _fill_chars(self, want_chars: int) -> None:
        """从 impl 读一块喂进字符流；EOF 时自动收尾。

        Args:
            want_chars: 本次期望消费的字符数（决定预读量下限）
        """
        chars = self._chars
        assert chars is not None
        if chars.eof:
            return
        raw = await self._impl.read(max(_DEFAULT_READLINE_BUF, want_chars * 4 + 4))
        if raw:
            chars.feed(raw)
        else:
            chars.finish()

    async def _read_text(self, size: int) -> str:
        """文本模式读：size 为字符数（CPython 语义）。"""
        chars = self._chars
        assert chars is not None
        if size < 0:
            while not chars.eof:
                await self._fill_chars(0)
            return chars.take_all()
        if size == 0:
            return ""
        while chars.buffered() < size and not chars.eof:
            await self._fill_chars(size)
        return chars.take(size)

    async def readline(self) -> str | bytes:
        if self._closed:
            raise ValueError("I/O operation on closed file.")

        # 文本模式：走字符流（已解码、已翻译，行尾按 newline 模式查找）
        chars = self._chars
        if chars is not None:
            while True:
                line = chars.take_line()
                if line is not None:
                    return line
                if chars.eof:
                    return chars.take_all()
                await self._fill_chars(1)

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
        if self._chars is not None:
            # 文本模式只允许 seek(0)：丢弃字符流全部缓冲与挂起状态
            self._chars.clear()
            return await self._impl.seek(0, 0)
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
        """返回当前文件位置（逻辑位置 = 用户消费到的位置）。

        文本模式：字节偏移。纯 \\n 行尾内容字节精确；\\r\\n 密集内容每处
        可能漂移 1 字节（换行压缩 \\r\\n→\\n 的固有问题——CPython 同样无法
        给出简单字节偏移，它返回不透明 cookie）。二进制模式始终字节精确。
        """
        return await self._impl.tell() - self._pending_bytes()

    def _pending_bytes(self) -> int:
        """已读但尚未消费的字节数（文本=字符流账本，二进制=预读缓冲）。"""
        if self._chars is not None:
            return self._chars.byte_pending()
        return len(self._line_buffer) - self._line_pos

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
