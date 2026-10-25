"""整文件操作：一次 await 完成 open + 读写 + close。

面向 ``asyncio.gather`` 批量处理多个文件的常见形态（模型权重按层加载、
配置文件批量读取、小文件聚合等），省去手写 ``async with`` 的样板。
底层就是 ``ayafileio.open`` + 单次 ``read()``/``write()``——行为、编码
语义与手动逐行写法完全一致。
"""

import asyncio
import os
from collections.abc import Iterable

from ._open import open as _open
from ._open import aopen as _aopen


async def read_bytes(path: str | os.PathLike[str]) -> bytes:
    """一次性读取整个文件，返回 ``bytes``。

    示例::

        data = await ayafileio.read_bytes("model.safetensors")
    """
    async with _open(path, "rb") as f:
        return await f.read()


async def write_bytes(path: str | os.PathLike[str], data: bytes) -> int:
    """一次性写入整个文件（覆盖写），返回写入的字节数。

    示例::

        n = await ayafileio.write_bytes("out.bin", payload)
    """
    async with _open(path, "wb") as f:
        return await f.write(data)


async def read_text(path: str | os.PathLike[str], encoding: "str | None" = None) -> str:
    """一次性读取整个文本文件，返回 ``str``。

    *encoding* 为 None 时沿用平台 locale 偏好编码（同内置 ``open()``）。

    示例::

        text = await ayafileio.read_text("config.yaml", encoding="utf-8")
    """
    async with _open(path, "r", encoding=encoding) as f:
        return await f.read()


async def write_text(path: str | os.PathLike[str], data: str, encoding: "str | None" = None) -> int:
    """一次性写入整个文本文件（覆盖写），返回写入的字符数。

    返回值的口径与 :meth:`AsyncFile.write` 及内置 ``open()`` 一致：
    文本模式数**字符**，不是编码（或换行翻译）之后的字节数。需要字节数
    用 ``len(data.encode(encoding))``，或改用 :func:`write_bytes`。

    示例::

        n = await ayafileio.write_text("out.txt", "hello", encoding="utf-8")
    """
    async with _open(path, "w", encoding=encoding) as f:
        return await f.write(data)


async def read_bytes_many(paths: Iterable[str | os.PathLike[str]], *, max_concurrency: int = 64) -> "list[bytes]":
    """并发读取多个文件，返回与输入顺序一致的 ``bytes`` 列表。

    打开动作经 :func:`ayafileio.aopen` 优先使用 Linux io_uring（不可用时回退 C++ 线程池）（多个 OS open
    真正并行），读取走各平台真异步后端；``max_concurrency`` 限制最大
    并发数，避免海量路径打爆 fd。任一路径失败则整体抛出（同
    ``asyncio.gather`` 默认语义）。

    示例::

        results = await ayafileio.read_bytes_many(model_shard_paths)
    """
    sem = asyncio.Semaphore(max_concurrency)

    async def _one(p) -> bytes:
        async with sem:
            async with _aopen(p, "rb") as f:
                return await f.read()

    return list(await asyncio.gather(*(_one(p) for p in paths)))


async def read_text_many(paths: Iterable[str | os.PathLike[str]], encoding: "str | None" = None, *,
                         max_concurrency: int = 64) -> "list[str]":
    """并发读取多个文本文件，返回与输入顺序一致的 ``str`` 列表。

    语义与 :func:`read_bytes_many` 一致；``encoding`` 为 None 时沿用
    平台 locale 偏好编码（同内置 ``open()``）。

    示例::

        configs = await ayafileio.read_text_many(paths, encoding="utf-8")
    """
    sem = asyncio.Semaphore(max_concurrency)

    async def _one(p) -> str:
        async with sem:
            async with _aopen(p, "r", encoding=encoding) as f:
                return await f.read()

    return list(await asyncio.gather(*(_one(p) for p in paths)))


async def write_bytes_many(pairs: Iterable[tuple[str | os.PathLike[str], bytes]], *, max_concurrency: int = 64) -> "list[int]":
    """并发写入多个文件，返回与输入顺序一致的字节数列表。

    *pairs* 是 ``(path, data)`` 对的可迭代对象（覆盖写）。打开动作经
    :func:`ayafileio.aopen` 优先使用 Linux io_uring（不可用时回退 C++ 线程池），写入走各平台真异步后端；
    ``max_concurrency`` 限制最大并发数。任一失败则整体抛出（同
    ``asyncio.gather`` 默认语义）——注意批量不是事务：抛出时其它
    文件的写入可能已经落盘。

    示例::

        counts = await ayafileio.write_bytes_many(
            [(p, payload) for p, payload in shard_items])
    """
    sem = asyncio.Semaphore(max_concurrency)

    async def _one(p, data: bytes) -> int:
        async with sem:
            async with _aopen(p, "wb") as f:
                return await f.write(data)

    items = list(pairs)
    return list(await asyncio.gather(*(_one(p, d) for p, d in items)))


async def write_text_many(pairs: Iterable[tuple[str | os.PathLike[str], str]], encoding: "str | None" = None, *,
                          max_concurrency: int = 64) -> "list[int]":
    """并发写入多个文本文件，返回与输入顺序一致的字符数列表。

    *pairs* 是 ``(path, str)`` 对的可迭代对象（覆盖写）；``encoding``
    为 None 时沿用平台 locale 偏好编码（同内置 ``open()``）。计数口径与
    :func:`write_text` 一致（字符数）。其余语义与 :func:`write_bytes_many`
    一致，包括非事务性。

    示例::

        counts = await ayafileio.write_text_many(items, encoding="utf-8")
    """
    sem = asyncio.Semaphore(max_concurrency)

    async def _one(p, data: str) -> int:
        async with sem:
            async with _aopen(p, "w", encoding=encoding) as f:
                return await f.write(data)

    items = list(pairs)
    return list(await asyncio.gather(*(_one(p, d) for p, d in items)))
