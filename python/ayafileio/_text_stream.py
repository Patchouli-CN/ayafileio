"""文本模式的增量解码、换行转换与字符缓冲。"""

import codecs
import os
from collections import deque


def _translate_for_read(text: str, newline: str | None) -> str:
    """Apply input newline translation for text mode."""
    if newline is None:
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


def _chars_covering_bytes(
    data: str,
    written_bytes: int,
    newline: str | None,
    encoding: str | None,
    errors: str | None,
) -> int:
    """Count whole characters of *data* whose transcoded form fits in a short write.

    Text-mode ``write()`` reports characters, but the backend only knows bytes:
    on the (rare) short-write path the byte count has to be mapped back onto
    characters one at a time, honouring both newline translation and the codec.
    """
    consumed = 0
    for index, char in enumerate(data):
        consumed += len(
            _translate_for_write(char, newline).encode(encoding, errors=errors)
        )
        if consumed > written_bytes:
            return index
    return len(data)


def _split_complete_tail(raw: bytes, encoding: str) -> tuple[bytes, bytes]:
    """把 *raw* 切成「字符完整部分」与「结尾残缺字符部分」。

    用 strict 解码探测：只有结尾处被 chunk 边界切出来的残缺字符
    （`unexpected end of data`）算残缺、需要回推；数据本身非法
    （`invalid start byte` 等）不算残缺——整体交还，由调用方的
    errors 模式处理。空输入返回 `(b"", b"")`。
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
    """流式换行翻译（通用模式 None 专用）：整段翻译，段尾孤立 \r 挂起。"""
    if newline is not None or not text:
        return text, False
    if text.endswith("\r"):
        body, pending = text[:-1], True
    else:
        body, pending = text, False
    return body.replace("\r\n", "\n").replace("\r", "\n"), pending


def _find_line_end_text(text: str, newline: str | None) -> tuple[int, int]:
    """在已解码文本里找行尾，返回 (起始下标, 长度)；无则 (-1, 0)。"""
    if newline is None or newline == "\n":
        idx = text.find("\n")
        return (idx, 1) if idx != -1 else (-1, 0)
    if newline == "\r":
        idx = text.find("\r")
        return (idx, 1) if idx != -1 else (-1, 0)
    if newline == "\r\n":
        idx = text.find("\r\n")
        return (idx, 2) if idx != -1 else (-1, 0)
    best = (-1, 0)
    for sep in ("\r\n", "\r", "\n"):
        idx = text.find(sep)
        if idx != -1 and (best[0] == -1 or idx < best[0]):
            best = (idx, len(sep))
    return best


class _CharStream:
    """文本模式字符流 —— CPython TextIOWrapper 三层 I/O 栈思想的移植。

    - 增量解码器拼接跨 chunk 的多字节序列；结尾残缺字节在进入解码器之前
      切出，与下一批读取拼接。
    - 分段字符缓冲记录已翻译文本及来源字节数，供 tell() 计算逻辑位置。
    - 换行翻译在入段时做，段尾孤立 \r 挂起，避免跨 chunk 拆错 \r\n。
    - read(n) 按翻译后字符数计数，与 CPython 一致。
    """

    def __init__(self, encoding: str, errors: str, newline: str | None) -> None:
        self._encoding = encoding
        self._newline = newline
        self._errors = errors
        self._decoder = codecs.getincrementaldecoder(encoding)(errors)
        self._segments: deque[tuple[str, int]] = deque()
        self._pending_raw = 0
        self._carry = b""
        self._pending_cr = False
        self._eof = False

    @property
    def eof(self) -> bool:
        return self._eof

    def buffered(self) -> int:
        return sum(len(text) for text, _ in self._segments)

    def byte_pending(self) -> int:
        return self._pending_raw + len(self._carry) + (1 if self._pending_cr else 0)

    def feed(self, raw: bytes) -> None:
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
            text, pending_cr = text[:-1], True
        self._pending_cr = pending_cr

        if text:
            seg_raw = len(complete) - (1 if pending_cr else 0)
            self._segments.append((text, seg_raw))
            self._pending_raw += seg_raw

    def finish(self) -> str:
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

    def take(self, n: int) -> str:
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
        out = "".join(text for text, _ in self._segments)
        self._segments.clear()
        self._pending_raw = 0
        return out

    def take_line(self) -> str | None:
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

    def clear(self) -> None:
        self._segments.clear()
        self._pending_raw = 0
        self._carry = b""
        self._pending_cr = False
        self._eof = False
        self._decoder = codecs.getincrementaldecoder(self._encoding)(self._errors)

    def _raw_len(self, prefix: str, seg_raw: int, seg_len: int) -> int:
        if not prefix:
            return 0
        try:
            return len(prefix.encode(self._encoding, errors="strict"))
        except UnicodeEncodeError:
            return round(seg_raw * len(prefix) / seg_len) if seg_len else 0
