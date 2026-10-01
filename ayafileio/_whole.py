"""整文件操作：一次 await 完成 open + 读写 + close。

面向 ``asyncio.gather`` 批量处理多个文件的常见形态（模型权重按层加载、
配置文件批量读取、小文件聚合等），省去手写 ``async with`` 的样板。
底层就是 ``ayafileio.open`` + 单次 ``read()``/``write()``——行为、编码
语义与手动逐行写法完全一致。
"""

import asyncio

from ._open import open as _aopen
from ._open import aopen as _aopen_async


async def read_bytes(path) -> bytes:
    """一次性读取整个文件，返回 ``bytes``。

    示例::

        data = await ayafileio.read_bytes("model.safetensors")
    """
    async with _aopen(path, "rb") as f:
        return await f.read()


async def write_bytes(path, data: bytes) -> int:
    """一次性写入整个文件（覆盖写），返回写入的字节数。

    示例::

        n = await ayafileio.write_bytes("out.bin", payload)
    """
    async with _aopen(path, "wb") as f:
        return await f.write(data)


async def read_text(path, encoding: "str | None" = None) -> str:
    """一次性读取整个文本文件，返回 ``str``。

    *encoding* 为 None 时沿用平台 locale 偏好编码（同内置 ``open()``）。

    示例::

        text = await ayafileio.read_text("config.yaml", encoding="utf-8")
    """
    async with _aopen(path, "r", encoding=encoding) as f:
        return await f.read()


async def write_text(path, data: str, encoding: "str | None" = None) -> int:
    """一次性写入整个文本文件（覆盖写），返回编码后写入的字节数。

    注意与内置 ``open()`` 的语义差异：内置文本模式 ``write()`` 返回
    字符数，这里返回 encode 之后实际落盘的字节数（与 ``AsyncFile.write``
    一致）。

    示例::

        n = await ayafileio.write_text("out.txt", "hello", encoding="utf-8")
    """
    async with _aopen(path, "w", encoding=encoding) as f:
        return await f.write(data)


async def read_bytes_many(paths, *, max_concurrency: int = 64) -> "list[bytes]":
    """并发读取多个文件，返回与输入顺序一致的 ``bytes`` 列表。

    打开动作经 :func:`ayafileio.aopen` 下沉到 C++ 线程池（多个 OS open
    真正并行），读取走各平台真异步后端；``max_concurrency`` 限制最大
    并发数，避免海量路径打爆 fd。任一路径失败则整体抛出（同
    ``asyncio.gather`` 默认语义）。

    示例::

        results = await ayafileio.read_bytes_many(model_shard_paths)
    """
    sem = asyncio.Semaphore(max_concurrency)

    async def _one(p) -> bytes:
        async with sem:
            async with await _aopen_async(p, "rb") as f:
                return await f.read()

    return list(await asyncio.gather(*(_one(p) for p in paths)))


async def read_text_many(paths, encoding: "str | None" = None, *,
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
            async with await _aopen_async(p, "r", encoding=encoding) as f:
                return await f.read()

    return list(await asyncio.gather(*(_one(p) for p in paths)))
