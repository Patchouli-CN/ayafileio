"""``read_bytes`` / ``read_bytes_many`` 的整文件快车道回归。

这两个入口现在走一条**原生单跳**路径：open + size + read + close 在一个线程池任务
里完成（对应 Node.js #65327 的 ReadFileJob 思路）。实测把每文件的 3 次「提交 ->
完成包 -> 回到事件循环」压成 1 次，4 KiB / 并发 64 下 13.1k -> 22.4k files/s。

快车道最容易出的不是性能问题，而是**语义悄悄漂移**：错误类型、错误文案、大文件、
空文件、顺序、句柄泄漏。这里逐条钉住，并且**以旧的异步路径为基准做对照**——这正是
"两条实现必须等价"该有的测法。
"""

import asyncio
import os
import pathlib
import sys

import pytest

import ayafileio

# 跨越几个有意义的尺寸：空、1 字节、刚好一页、以及远大于任何内部块大小
SIZES = [0, 1, 4095, 4096, 700_000]


def _payload(size: int) -> bytes:
    return bytes((i * 37 + 11) & 0xFF for i in range(size))


async def _async_path(path) -> bytes:
    """旧路径，作为语义基准。"""
    async with ayafileio.aopen(path, "rb") as handle:
        return await handle.read()


@pytest.mark.parametrize("size", SIZES, ids=[f"{s}B" for s in SIZES])
async def test_content_matches_async_path(tmp_path, size):
    path = tmp_path / f"f{size}.bin"
    path.write_bytes(_payload(size))
    assert await ayafileio.read_bytes(path) == await _async_path(path)
    assert await ayafileio.read_bytes(str(path)) == _payload(size)


async def test_accepts_pathlike_and_str(tmp_path):
    path = tmp_path / "p.bin"
    path.write_bytes(b"payload")
    assert await ayafileio.read_bytes(path) == b"payload"
    assert await ayafileio.read_bytes(str(path)) == b"payload"
    assert await ayafileio.read_bytes(os.fspath(path)) == b"payload"


async def test_missing_file_error_matches(tmp_path):
    missing = tmp_path / "nope.bin"
    with pytest.raises(FileNotFoundError) as new:
        await ayafileio.read_bytes(missing)
    with pytest.raises(FileNotFoundError) as old:
        await _async_path(missing)
    # 类型一致之外，文案也必须带路径——否则用户日志里就丢了关键信息
    assert str(missing) in str(new.value)
    assert type(new.value) is type(old.value)
    assert new.value.errno == old.value.errno


async def test_directory_error_matches(tmp_path):
    directory = tmp_path / "sub"
    directory.mkdir()
    new_exc = old_exc = None
    try:
        await ayafileio.read_bytes(directory)
    except OSError as exc:
        new_exc = exc
    try:
        await _async_path(directory)
    except OSError as exc:
        old_exc = exc
    assert new_exc is not None and old_exc is not None
    assert type(new_exc) is type(old_exc)
    assert new_exc.errno == old_exc.errno


async def test_many_preserves_order_and_content(tmp_path):
    paths = []
    for index, size in enumerate([3, 0, 5000, 1]):
        path = tmp_path / f"m{index}.bin"
        path.write_bytes(_payload(size))
        paths.append(path)
    results = await ayafileio.read_bytes_many(paths)
    assert [len(r) for r in results] == [3, 0, 5000, 1]
    assert results == [await _async_path(p) for p in paths]


async def test_many_propagates_first_failure(tmp_path):
    good = tmp_path / "good.bin"
    good.write_bytes(b"ok")
    with pytest.raises(FileNotFoundError):
        await ayafileio.read_bytes_many([good, tmp_path / "missing.bin"])


async def test_concurrent_reads_are_independent(tmp_path):
    """连续 100 轮并发读取：内容独立，且任何一轮都不得因漏投递而卡住。"""
    paths = []
    expected = []
    for index in range(120):
        path = tmp_path / f"c{index}.bin"
        payload = _payload(index)
        path.write_bytes(payload)
        paths.append(path)
        expected.append(payload)
    for _ in range(100):
        results = await asyncio.wait_for(
            ayafileio.read_bytes_many(paths, max_concurrency=32),
            timeout=5,
        )
        assert results == expected


@pytest.mark.skipif(sys.platform != "win32", reason="句柄计数用 Windows API")
async def test_no_handle_leak(tmp_path):
    """快车道每条路都要 close；这里量句柄数是否回到基线。"""
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    def handle_count() -> int:
        count = ctypes.c_ulong(0)
        kernel32.GetProcessHandleCount(kernel32.GetCurrentProcess(), ctypes.byref(count))
        return count.value

    good = tmp_path / "leak.bin"
    good.write_bytes(_payload(4096))
    missing = tmp_path / "leak-missing.bin"

    for _ in range(20):                       # 预热，让运行时自己的一次性分配完成
        await ayafileio.read_bytes(good)
    before = handle_count()
    for _ in range(300):
        await ayafileio.read_bytes(good)
        try:
            await ayafileio.read_bytes(missing)
        except FileNotFoundError:
            pass
    after = handle_count()
    assert abs(after - before) <= 2, f"句柄数 {before} -> {after}，快车道有泄漏"
