"""文件打开入口"""

from pathlib import Path
from typing import overload, Literal
from ._async_file import AsyncFile


# ════════════════════════════════════════════════════════════════════════════
# 类型重载：文本模式 → str，二进制模式 → bytes
# ════════════════════════════════════════════════════════════════════════════


@overload
def open(
    path: str | Path,
    mode: Literal["r", "w", "a", "x", "r+", "w+", "a+", "x+"],
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
) -> AsyncFile[str]:
    """文本模式：read() 返回 str"""
    ...


@overload
def open(
    path: str | Path,
    mode: Literal["rb", "wb", "ab", "xb", "rb+", "wb+", "ab+", "xb+"] = "rb",
    encoding: None = None,
    newline: str | None = None,
    errors: str | None = None,
) -> AsyncFile[bytes]:
    """二进制模式：read() 返回 bytes"""
    ...


def open(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
) -> AsyncFile[str] | AsyncFile[bytes]:
    """打开一个 AsyncFile 实例，自动复用已缓存的句柄。

    示例::

        async with ayafileio.open('data.bin', 'rb') as f:
            data = await f.read()

        async with ayafileio.open('log.txt', 'w', encoding='utf-8') as f:
            await f.write('hello')

    💡 性能提示：尽量复用同一个句柄，避免在循环中反复 open/close。
    """
    return AsyncFile(path, mode, encoding, newline, errors)
