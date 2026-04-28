"""
ayafileio - 跨平台异步文件 I/O 库
====================================

三平台真异步支持:
    - Windows: IOCP (I/O Completion Ports)
    - Linux: io_uring (kernel 5.1+) 或线程池降级
    - macOS: Dispatch I/O (GCD) 或线程池降级

提供与 aiofiles 兼容的 API, 但性能更优。
"""

__version__ = "1.0.5"

from .util import warn_fake_async

warn_fake_async()

from . import _cleanup  # noqa: F401  # 副作用：注册 atexit

from ._async_file import AsyncFile
from ._open import open
from ._wrap import wrap_fd
from .types import AyaFileIO
from ._config import configure, get_config, reset_config, get_backend_info
from ._compat import (
    set_handle_pool_limits,
    get_handle_pool_limits,
    set_io_worker_count,
    set_iocp_worker_count,
)

__all__ = [
    "open",
    "wrap_fd",
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
]
