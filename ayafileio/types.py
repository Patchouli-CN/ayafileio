"""AyaIO 类型定义"""

from typing import Protocol, runtime_checkable, TypeVar, Generic
import sys

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
    async def seek(self, offset: int, whence: int = 0) -> int: ...
    async def flush(self) -> None: ...
    async def close(self) -> None: ...

    @property
    def closed(self) -> bool: ...

    async def __aenter__(self) -> Self: ...
    async def __aexit__(self, *args) -> None: ...