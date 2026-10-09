"""AyaIO 类型定义"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path
from typing import TYPE_CHECKING, Protocol, TypeVar, runtime_checkable

if TYPE_CHECKING:
    # 惰性注解下 Self 只存在于类型检查期：3.10 运行时也无需 typing_extensions
    if sys.version_info >= (3, 11):
        from typing import Self
    else:
        from typing_extensions import Self

T = TypeVar("T", str, bytes, covariant=True)


@runtime_checkable
class AyaFileIO(Protocol[T]):
    """AyaFileIO 异步 I/O 文件协议

    所有通过 open() 或 wrap_fd() 创建的对象都遵循此协议。
    """

    async def read(self, size: int = -1) -> T: ...
    async def write(self, data: str | bytes | bytearray | memoryview) -> int: ...
    async def read_at(self, offset: int, size: int = -1) -> bytes: ...
    async def read_many(self, spans: Iterable[tuple[int, int]]) -> list[bytes]: ...
    async def write_at(self, offset: int, data: bytes | bytearray | memoryview) -> int: ...
    async def write_many(
        self, writes: Iterable[tuple[int, bytes | bytearray | memoryview]]
    ) -> list[int]: ...
    async def seek(self, offset: int, whence: int = 0) -> int: ...
    async def flush(self) -> None: ...
    async def close(self) -> None: ...

    @property
    def closed(self) -> bool: ...

    async def __aenter__(self) -> Self: ...
    async def __aexit__(self, *args) -> None: ...

@runtime_checkable
class _HasFileno(Protocol):
    """任何能提供文件描述符的对象（鸭子类型）"""
    def fileno(self) -> int: ...

FileObj = _HasFileno
""" 真实文件对象 """


# ════════════════════════════════════════════════════════════════════════════
# 配置与能力发现的返回类型
# ════════════════════════════════════════════════════════════════════════════

from typing import Literal, TypedDict


class BackendInfo(TypedDict):
    """``get_backend_info()`` 的返回"""
    platform: Literal["windows", "linux", "macos", "posix"]
    backend: Literal["iocp", "io_uring", "dispatch_io", "thread_pool"]
    is_truly_async: bool
    os_version: str
    description: str


class IocpDetail(TypedDict):
    """``backend_detail`` — Windows IOCP"""
    completion_model: bool  # 恒 True：IRP + 中断的提交-完成模型
    batch_harvest: bool
    handle_pool: bool


class IoUringDetail(TypedDict, total=False):
    """``backend_detail`` — Linux io_uring"""
    available: bool
    completion_model: bool
    sqpoll: bool
    features: dict[str, bool]        # ring.features 特性位矩阵
    opcodes: dict[str, bool] | None  # IORING_REGISTER_PROBE 结果；内核 < 5.6 为 None
    probe_error: bool
    note: str


class GcdDetail(TypedDict, total=False):
    """``backend_detail`` — macOS Dispatch I/O"""
    dispatch_io: bool
    completion_model: bool  # 恒 False：内核托管 workqueue 跑阻塞 pread
    kernel_managed_workqueue: bool
    mincore_fastpath: bool


class ThreadPoolDetail(TypedDict, total=False):
    """``backend_detail`` — 线程池降级"""
    completion_model: bool  # 恒 False
    note: str


BackendDetail = IocpDetail | IoUringDetail | GcdDetail | ThreadPoolDetail
"""``backend_detail`` 的联合类型——键随后端不同，用 ``.get()`` 或收窄后访问"""


class LibraryFeatures(TypedDict):
    """``capabilities['features']``：库级能力，键跨平台统一"""
    positional_io: bool
    batch_positional_io: bool
    zero_copy_readinto: bool
    chunked_streaming: bool
    async_open: bool
    adaptive_batching: bool
    fast_file_copy: bool


class Capabilities(BackendInfo):
    """``get_capabilities()`` 的返回：BackendInfo + features + backend_detail"""
    features: LibraryFeatures
    backend_detail: BackendDetail
