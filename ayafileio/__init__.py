"""
ayafileio - 跨平台异步文件 I/O 库
====================================

三平台真异步支持:
    - Windows: IOCP (I/O Completion Ports)
    - Linux: io_uring (kernel 5.1+) 或线程池降级
    - macOS: Dispatch I/O (GCD) 或线程池降级

提供与 aiofiles 兼容的 API, 但性能更优。
"""

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _pkg_version

try:
    # 单一事实来源是 pyproject.toml；运行时从安装元数据读，杜绝发版忘 bump
    __version__ = _pkg_version("ayafileio")
except PackageNotFoundError:  # 源码树直跑（未安装）时兜底
    __version__ = "0.0.0+unknown"

del _pkg_version, PackageNotFoundError

from .util import warn_fake_async

warn_fake_async()

from . import _cleanup  # noqa: F401  # 副作用：注册 atexit

from ._async_file import AsyncFile
from ._copy import acopy
from ._open import open, aopen
from ._whole import (
    read_bytes,
    write_bytes,
    read_text,
    write_text,
    read_bytes_many,
    read_text_many,
    write_bytes_many,
    write_text_many,
)
from ._wrap import wrap_file
from ._types import AyaFileIO
from ._config import configure, get_config, reset_config, get_backend_info
from ._compat import (
    set_handle_pool_limits,
    get_handle_pool_limits,
    set_io_worker_count,
    set_iocp_worker_count,
    drain_handle_pool,
    drain_buffer_pool,
)

__all__ = [
    "open",
    "aopen",
    "wrap_file",
    "acopy",
    "read_bytes",
    "write_bytes",
    "read_text",
    "write_text",
    "read_bytes_many",
    "read_text_many",
    "write_bytes_many",
    "write_text_many",
    "AyaFileIO",
    "AsyncFile",
    "configure",
    "get_config",
    "reset_config",
    "get_backend_info",
    "set_handle_pool_limits",
    "get_handle_pool_limits",
    "set_io_worker_count",
    "set_iocp_worker_count",
    "drain_handle_pool",
    "drain_buffer_pool",
]
