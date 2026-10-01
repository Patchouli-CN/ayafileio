"""文件打开入口"""

import locale
from pathlib import Path
from typing import overload, Literal
from ._async_file import AsyncFile, _normalize_mode
from ._ayafileio import open_async as _native_open_async


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
    auto_flush: bool = False
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
    auto_flush: bool = False
) -> AsyncFile[bytes]:
    """二进制模式：read() 返回 bytes"""
    ...


def open(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
    auto_flush: bool = False
) -> AsyncFile[str] | AsyncFile[bytes]:
    """打开一个 AsyncFile 实例，自动复用已缓存的句柄。

    示例::

        async with ayafileio.open('data.bin', 'rb') as f:
            data = await f.read()

        async with ayafileio.open('log.txt', 'w', encoding='utf-8') as f:
            await f.write('hello')

    💡 性能提示：尽量复用同一个句柄，避免在循环中反复 open/close。
    """
    return AsyncFile(path, mode, encoding, newline, errors, auto_flush)


async def aopen(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
    auto_flush: bool = False
) -> AsyncFile[str] | AsyncFile[bytes]:
    """异步打开文件：OS open 在 C++ 线程池执行，不阻塞事件循环。

    与 :func:`open` 的参数、模式语义、返回的 ``AsyncFile`` 完全一致，
    唯一区别是打开动作本身也是异步的——批量打开大量文件时多个
    open 系统调用真正并行（配合 ``read_bytes_many`` 食用最佳）。

    示例::

        async with await ayafileio.aopen('data.bin', 'rb') as f:
            data = await f.read()
    """
    clean_mode, is_text = _normalize_mode(mode, encoding, newline)
    impl = await _native_open_async(str(path), clean_mode)
    enc = (encoding or locale.getpreferredencoding(False)) if is_text else "utf-8"
    return AsyncFile._from_impl(
        impl, clean_mode,
        path=str(path), is_text=is_text, encoding=enc,
        newline=newline, errors=errors or "strict", auto_flush=auto_flush
    )
