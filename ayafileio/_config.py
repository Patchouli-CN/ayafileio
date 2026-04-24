import sys
from ._ayafileio import (
    configure as _configure,
    get_config as _get_config,
    reset_config as _reset_config,
    get_backend_info as _get_backend_info,
)

def configure(options: dict) -> None:
    """统一配置 ayafileio。
    
    options: 配置字典，支持以下键:
        - handle_pool_max_per_key (int): 每个文件最大缓存句柄数 (Windows, 默认 64)
        - handle_pool_max_total (int): 全局最大缓存句柄数 (Windows, 默认 2048)
        - io_worker_count (int): I/O 工作线程数，0=自动 (默认 0, 最大 128)
        - buffer_pool_max (int): 最大缓存缓冲区数 (默认 512)
        - buffer_size (int): 单个缓冲区大小，字节 (默认 65536)
        - close_timeout_ms (int): 关闭时等待 pending I/O 的最大毫秒数 (默认 4000)
        - io_uring_queue_depth (int): io_uring 队列深度 (Linux, 默认 256)
        - io_uring_sqpoll (bool): 是否启用 SQPOLL 模式 (Linux, 默认 False)
        - enable_debug_log (bool): 是否启用调试日志 (默认 False)

    Example:
    ```python
        ayafileio.configure({
            "io_worker_count": 8,
            "buffer_size": 131072,
            "close_timeout_ms": 2000,
        })
    ```
    """
    _configure(options)

def get_config() -> dict:
    """获取当前配置。"""
    return _get_config()

def reset_config() -> None:
    """重置配置为默认值。"""
    _reset_config()

def get_backend_info() -> dict[str, str]:
    """获取当前后端信息。"""
    return _get_backend_info()