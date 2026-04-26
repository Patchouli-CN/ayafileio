"""文件打开入口 — 带 AsyncFile 缓存复用"""

from pathlib import Path
from ._async_file import AsyncFile

# ════════════════════════════════════════════════════════════════════════════
# 公开的 open 函数
# ════════════════════════════════════════════════════════════════════════════


def open(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
) -> AsyncFile:
    """打开一个 AsyncFile 实例，自动复用已缓存的句柄。

    示例::

        async with ayafileio.open('data.bin', 'rb') as f:
            data = await f.read()

        async with ayafileio.open('log.txt', 'w', encoding='utf-8') as f:
            await f.write('hello')

    💡 性能提示：尽量复用同一个句柄，避免在循环中反复 open/close。
    """

    return AsyncFile(path, mode, encoding, newline, errors)
