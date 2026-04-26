"""统一配置 API"""

import sys

if sys.version_info >= (3, 11):
    from typing import TypedDict, NotRequired
else:
    from typing_extensions import TypedDict, NotRequired

from ._ayafileio import (  # type: ignore[missing-imports]
    configure as _configure,
    get_config as _get_config,
    reset_config as _reset_config,
    get_backend_info as _get_backend_info,
)

# ════════════════════════════════════════════════════════════════════════════
# TypedDict
# ════════════════════════════════════════════════════════════════════════════


class AyafileioConfig(TypedDict, total=False):
    """ayafileio 配置字典类型 — 所有键均为可选"""

    # Windows 句柄池
    handle_pool_max_per_key: NotRequired[int]
    handle_pool_max_total: NotRequired[int]

    # 线程池
    io_worker_count: NotRequired[int]

    # 缓冲区池
    buffer_pool_max: NotRequired[int]
    buffer_size: NotRequired[int]

    # 超时
    close_timeout_ms: NotRequired[int]

    # io_uring (Linux)
    io_uring_queue_depth: NotRequired[int]
    io_uring_sqpoll: NotRequired[bool]


# ════════════════════════════════════════════════════════════════════════════
# 公开 API
# ════════════════════════════════════════════════════════════════════════════


def configure(options: AyafileioConfig) -> None:
    """统一配置 ayafileio。

    options 支持以下键：

    C++ 层配置
    ----------
    ``handle_pool_max_per_key``
        每个文件最大缓存句柄数（Windows，默认 64）。
    ``handle_pool_max_total``
        全局最大缓存句柄数（Windows，默认 2048）。
    ``io_worker_count``
        I/O 工作线程数，0=自动（默认 0，最大 128）。
    ``buffer_pool_max``
        最大缓存缓冲区数（默认 512）。
    ``buffer_size``
        单个缓冲区大小，字节（默认 65536）。
    ``close_timeout_ms``
        关闭时等待 pending I/O 的最大毫秒数（默认 4000）。
    ``io_uring_queue_depth``
        io_uring 队列深度（Linux，默认 256）。
    ``io_uring_sqpoll``
        是否启用 SQPOLL 模式（Linux，默认 ``False``）。

    Example::

        ayafileio.configure({
            "io_worker_count": 8,
            "buffer_size": 131072,
            "close_timeout_ms": 2000,
        })
    """
    _configure(options)


def get_config() -> AyafileioConfig:
    """获取当前配置。

    :rtype: 包含所有配置项的字典。
    """
    config = _get_config()
    return config  # type: ignore[return-value]


def reset_config() -> None:
    """重置配置为默认值。"""
    global _CACHE_MAX_SIZE, _CACHE_ENABLED
    _CACHE_MAX_SIZE = 128
    _CACHE_ENABLED = True
    _reset_config()


def get_backend_info() -> dict[str, str]:
    """获取当前后端信息。

    :rtype: 包含 ``platform``、``backend``、``is_truly_async`` 和 ``description`` 的字典。
    """
    return _get_backend_info()
