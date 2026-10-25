"""异步整文件复制：OS 级快车道 + `read_at`/`write_at` 流水线兜底。

快车道（数据不过 Python 层，跑在 worker 线程里）：

- Linux: ``copy_file_range`` 内核态零拷贝
- Windows: ``CopyFile2`` 系统级复制（顺带保留时间戳等元数据）
- 其它平台 / 快车道被内核拒绝（跨设备、NFS 等）: 位置读写流水线
"""

import asyncio
import os
import shutil

from ._open import open as _aopen

_DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024  # 4 MiB
_DEFAULT_CONCURRENCY = 8


def _copy_file_range_all(src, dst) -> int:
    """Linux 内核复制全程。中途失败抛 OSError 交给调用方兜底。"""
    fd_in = os.open(src, os.O_RDONLY)
    try:
        size = os.fstat(fd_in).st_size
        fd_out = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o666)
        try:
            done = 0
            while done < size:
                n = os.copy_file_range(
                    fd_in, fd_out, size - done, offset_src=done, offset_dst=done
                )
                if n <= 0:
                    raise OSError("copy_file_range stalled (source changed during copy?)")
                done += n
            return done
        finally:
            os.close(fd_out)
    finally:
        os.close(fd_in)


def _make_os_copy():
    """挑选本平台的 OS 级复制函数；没有就返回 None（走流水线）。"""
    try:
        from _winapi import CopyFile2
    except ImportError:
        pass
    else:
        def _windows_copy_file(src, dst) -> int:
            CopyFile2(os.fsdecode(src), os.fsdecode(dst), 0)  # flags=0: 覆盖已存在的目标
            return os.path.getsize(dst)

        return _windows_copy_file

    if hasattr(os, "copy_file_range"):
        return _copy_file_range_all
    return None


_os_copy = _make_os_copy()
"""OS 级复制快车道（无则 None）。测试可置 None 强制走流水线。"""


async def acopy(
    src: "str | bytes | os.PathLike",
    dst: "str | bytes | os.PathLike",
    *,
    chunk_size: int = _DEFAULT_CHUNK_SIZE,
    concurrency: int = _DEFAULT_CONCURRENCY,
    copy_stat: bool = False,
) -> int:
    """异步复制整个文件，返回复制的字节数。

    优先走 OS 级快车道（Linux ``copy_file_range`` 内核态零拷贝、Windows
    ``CopyFile2``，数据不过 Python 层，跑在 worker 线程里）；快车道不可用
    或被系统拒绝（跨设备、NFS 等）时自动回退到 ``read_at``/``write_at``
    流水线：最多 *concurrency* 个 *chunk_size* 块在飞，读写互相重叠，
    内存占用上界为 ``concurrency × chunk_size``。

    语义与 ``shutil.copyfile`` 对齐：

    - *dst* 已存在则被截断覆盖；复制到自身抛 ``shutil.SameFileError``。
    - *copy_stat* 为 True 时复制完成后附加 ``shutil.copystat``
      （Windows 快车道的 ``CopyFile2`` 本身已保留元数据）。
    - 快车道中途不可取消；复制期间源文件被并发修改的结果未定义（与 ``cp`` 相同）。
    """
    if chunk_size <= 0:
        raise ValueError("chunk_size must be positive")
    if concurrency < 1:
        raise ValueError("concurrency must be >= 1")
    try:
        if os.path.samefile(src, dst):
            raise shutil.SameFileError(f"{src!r} and {dst!r} are the same file")
    except FileNotFoundError:
        pass

    if _os_copy is not None:
        try:
            written = await asyncio.to_thread(_os_copy, src, dst)
        except OSError:
            pass  # 快车道被拒 → 流水线兜底
        else:
            if copy_stat:
                await asyncio.to_thread(shutil.copystat, src, dst)
            return written

    written = await _copy_pipelined(src, dst, chunk_size, concurrency)
    if copy_stat:
        await asyncio.to_thread(shutil.copystat, src, dst)
    return written


async def _copy_pipelined(src, dst, chunk_size: int, concurrency: int) -> int:
    """位置读/写流水线：读写按绝对偏移各走各的，无需任何顺序协调。"""
    size = os.path.getsize(src)
    async with _aopen(src, "rb") as fin, _aopen(dst, "wb") as fout:
        if size == 0:
            return 0
        await fout.truncate(size)  # 预分配 EOF，省掉并发写过程中的反复扩展

        sem = asyncio.Semaphore(concurrency)

        async def copy_chunk(offset: int, length: int) -> int:
            async with sem:
                data = await fin.read_at(offset, length)
                view = memoryview(data)
                pos = 0
                while pos < len(view):
                    pos += await fout.write_at(offset + pos, view[pos:])
                return pos

        tasks = [
            asyncio.ensure_future(copy_chunk(off, min(chunk_size, size - off)))
            for off in range(0, size, chunk_size)
        ]
        try:
            results = await asyncio.gather(*tasks)
        except BaseException:
            # 出错或取消时收编剩余任务，避免文件关闭后还有写操作在飞
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            raise
        return sum(results)
