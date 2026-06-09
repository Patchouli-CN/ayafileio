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
# 模块级状态
# ════════════════════════════════════════════════════════════════════════════

_CACHE_MAX_SIZE = 128
_CACHE_ENABLED = True

# ════════════════════════════════════════════════════════════════════════════
# TypedDict
# ════════════════════════════════════════════════════════════════════════════


class AyafileioConfig(TypedDict, total=False):
    """ayafileio 配置字典类型 — 所有键均为可选"""

    # ── 跨平台 / 通用 ──────────────────────────────────────────────────────
    io_worker_count: NotRequired[int]
    buffer_pool_max: NotRequired[int]
    buffer_size: NotRequired[int]
    close_timeout_ms: NotRequired[int]

    # ── Windows / IOCP ─────────────────────────────────────────────────────
    handle_pool_max_per_key: NotRequired[int]
    handle_pool_max_total: NotRequired[int]
    iocp_batch_size: NotRequired[int]

    # ── Linux / io_uring ──────────────────────────────────────────────────
    io_uring_queue_depth: NotRequired[int]
    io_uring_sqpoll: NotRequired[bool]

    # ── ResultBatcher / 自适应批处理 ────────────────────────────────
    adaptive_batch: NotRequired[bool]
    adaptive_target_latency_us: NotRequired[int]


# ════════════════════════════════════════════════════════════════════════════
# 公开 API
# ════════════════════════════════════════════════════════════════════════════


def configure(options: AyafileioConfig) -> None:
    """统一配置 ayafileio。

    options 支持以下键：

    **跨平台 / 通用**
    ``io_worker_count``
        I/O 工作线程数，0=自动（默认 0，最大 128）。
    ``buffer_pool_max``
        最大缓存缓冲区数（默认 512）。
    ``buffer_size``
        单个缓冲区大小，字节（默认 65536）。
    ``close_timeout_ms``
        关闭时等待 pending I/O 的最大毫秒数（默认 4000）。

    **Windows / IOCP**
    ``handle_pool_max_per_key``
        每个文件最大缓存句柄数（默认 64）。
    ``handle_pool_max_total``
        全局最大缓存句柄数（默认 2048）。
    ``iocp_batch_size``
        IOCP 批量收割大小（默认 64，范围 1-256）。

    **Linux / io_uring**
    ``io_uring_queue_depth``
        提交队列深度（默认 256，范围 1-4096）。
    ``io_uring_sqpoll``
        是否启用 SQPOLL 模式（默认 ``False``）。

    **ResultBatcher / 自适应批处理**
    ``adaptive_batch``
        是否启用自适应批次大小（默认 ``True``）。
        开启后，ResultBatcher 根据 I/O 完成速率动态调整批处理阈值，
        快盘自动增大批次减少调度开销，慢盘自动减小批次避免延迟。
    ``adaptive_target_latency_us``
        目标最大额外延迟，微秒（默认 1000，范围 1-10000）。
        值越小延迟越低但批处理机会越少，值越大批处理越激进但延迟越高。

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
