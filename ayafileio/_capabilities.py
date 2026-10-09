"""能力发现：get_capabilities()

区分两个层面：

- **库级能力**（``features``）：ayafileio 自身实现的特性，键跨平台统一
- **后端明细**（``backend_detail``）：当前后端在此机器上探测到的原生能力，
  由 C++ 层直接问内核（io_uring PROBE / 试建 ring），不按版本号猜测
"""

import os
import sys
from typing import Any

from ._ayafileio import get_capabilities as _native_get_capabilities


def _fast_copy_available() -> bool:
    """acopy() 的 OS 级快车道在本平台是否可用"""
    if sys.platform == "win32":
        try:
            from _winapi import CopyFile2  # noqa: F401
            return True
        except ImportError:
            return False
    # Linux: copy_file_range（内核 4.5+，CPython 按平台条件导出）
    # macOS / 其他 POSIX：无快车道，acopy 走 read_at/write_at 流水线
    return hasattr(os, "copy_file_range")


def get_capabilities() -> dict[str, Any]:
    """获取能力矩阵：后端身份 + 库级特性 + 后端原生能力明细。

    返回字典包含 ``get_backend_info()`` 的全部键，外加：

    ``features`` : dict[str, bool]
        库级能力，跨平台统一键：

        - ``positional_io`` —— read_at / write_at 位置读写
        - ``batch_positional_io`` —— read_many / write_many 批量位置读写
        - ``zero_copy_readinto`` —— readinto 零拷贝读
        - ``chunked_streaming`` —— chunk() 流式分块
        - ``async_open`` —— aopen() 异步打开（C++ 线程池）
        - ``adaptive_batching`` —— ResultBatcher 自适应完成批处理
        - ``fast_file_copy`` —— acopy() 的 OS 级快车道
          （Linux copy_file_range / Windows CopyFile2）

    ``backend_detail`` : dict
        后端原生能力明细，键随后端不同（io_uring 后端含逐 opcode
        的 PROBE 探测结果等），详见各平台文档。

    示例::

        caps = ayafileio.get_capabilities()
        if caps["backend_detail"].get("sqpoll"):
            ayafileio.configure({"io_uring_sqpoll": True})
    """
    caps: dict[str, Any] = _native_get_capabilities()
    caps["features"] = {
        "positional_io": True,
        "batch_positional_io": True,
        "zero_copy_readinto": True,
        "chunked_streaming": True,
        "async_open": True,
        "adaptive_batching": True,
        "fast_file_copy": _fast_copy_available(),
    }
    return caps
