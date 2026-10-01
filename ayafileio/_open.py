"""文件打开入口"""

import locale
from pathlib import Path
from typing import overload, Literal, Any, Generic, TypeVar, cast
from collections.abc import Generator
from ._async_file import AsyncFile, _normalize_mode
from ._ayafileio import open_async as _native_open_async

T = TypeVar("T")


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


# ════════════════════════════════════════════════════════════════════════════
# _AOpen — aopen() 返回的惰性句柄：同时是 awaitable 和异步上下文管理器
#
# async with 会调用对象的 __aenter__（本身就是 async 方法），打开动作在
# 其中发生，所以两种用法等价（aiofiles 同款手法）：
#   async with aopen(...) as f      —— __aenter__ 隐式触发异步打开
#   f = await aopen(...)            —— 显式等待打开结果
# 一次性对象：打开结果会被缓存，复用同一对象再次进入拿到的是同一个文件
# （可能已关闭），不要复用。
# ════════════════════════════════════════════════════════════════════════════


class _AOpen(Generic[T]):
    def __init__(
        self,
        path: str | Path,
        mode: str,
        encoding: str | None,
        newline: str | None,
        errors: str | None,
        auto_flush: bool
    ) -> None:
        self._path = path
        self._mode = mode
        self._encoding = encoding
        self._newline = newline
        self._errors = errors
        self._auto_flush = auto_flush
        self._file: AsyncFile[T] | None = None

    async def _do_open(self) -> AsyncFile[T]:
        if self._file is None:
            clean_mode, is_text = _normalize_mode(self._mode, self._encoding, self._newline)
            impl = await _native_open_async(str(self._path), clean_mode)
            enc = (self._encoding or locale.getpreferredencoding(False)) if is_text else "utf-8"
            self._file = cast(AsyncFile[T], AsyncFile._from_impl(
                impl, clean_mode,
                path=str(self._path), is_text=is_text, encoding=enc,
                newline=self._newline, errors=self._errors or "strict",
                auto_flush=self._auto_flush
            ))
        return self._file

    def __await__(self) -> Generator[Any, None, AsyncFile[T]]:
        return self._do_open().__await__()

    async def __aenter__(self) -> AsyncFile[T]:
        return await self._do_open()

    async def __aexit__(self, *exc: object) -> None:
        if self._file is not None:
            await self._file.__aexit__(*exc)


# ════════════════════════════════════════════════════════════════════════════
# aopen 类型重载：文本模式 → str，二进制模式 → bytes（与 open 一致）
# ════════════════════════════════════════════════════════════════════════════


@overload
def aopen(
    path: str | Path,
    mode: Literal["r", "w", "a", "x", "r+", "w+", "a+", "x+"],
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
    auto_flush: bool = False
) -> _AOpen[str]:
    """文本模式：打开后 read() 返回 str"""
    ...


@overload
def aopen(
    path: str | Path,
    mode: Literal["rb", "wb", "ab", "xb", "rb+", "wb+", "ab+", "xb+"] = "rb",
    encoding: None = None,
    newline: str | None = None,
    errors: str | None = None,
    auto_flush: bool = False
) -> _AOpen[bytes]:
    """二进制模式：打开后 read() 返回 bytes"""
    ...


def aopen(
    path: str | Path,
    mode: str = "rb",
    encoding: str | None = None,
    newline: str | None = None,
    errors: str | None = None,
    auto_flush: bool = False
) -> _AOpen[str] | _AOpen[bytes]:
    """异步打开文件：OS open 在 C++ 线程池执行，不阻塞事件循环。

    与 :func:`open` 的参数、模式语义、返回的 ``AsyncFile`` 完全一致，
    唯一区别是打开动作本身也是异步的——批量打开大量文件时多个
    open 系统调用真正并行（配合 ``read_bytes_many`` 食用最佳）。

    返回惰性句柄，两种用法等价::

        async with ayafileio.aopen('data.bin', 'rb') as f:
            data = await f.read()

        f = await ayafileio.aopen('data.bin', 'rb')
        data = await f.read()
        await f.close()
    """
    return _AOpen(path, mode, encoding, newline, errors, auto_flush)
