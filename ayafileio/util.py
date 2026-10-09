"""工具模块"""

import os
import stat
import warnings

def is_real_file(obj: int) -> bool:
    """检查一个 fd 或文件对象是否指向真正的常规文件。

    排除 socket、pipe、tty 等特殊文件类型。
    """
    try:
        # 如果是文件对象，拿它的 fileno()
        mode = os.fstat(obj).st_mode
        return stat.S_ISREG(mode)
    except (OSError, TypeError, AttributeError):
        return False


_WARNED = False
"""是否已经发出过警告"""


def warn_fake_async():
    """如果当前后端不是真异步，发出 UserWarning。

    结论直接来自 C++ 扩展的权威运行时探测（Linux 上真的试建了 io_uring
    ring，macOS 上真的建了 dispatch channel），Python 侧不做任何版本号
    解析或 ctypes 猜测。必须在扩展加载之后调用。
    """
    global _WARNED
    if _WARNED:
        return
    _WARNED = True

    from ._ayafileio import get_backend_info

    info = get_backend_info()
    if info["is_truly_async"]:
        return

    warnings.warn(
        f"ayafileio: {info['description']} (fake async). "
        f"Platform: {info['platform']}, kernel: {info['os_version']}. "
        "Run ayafileio.get_capabilities() for probed backend details.",
        UserWarning,
        stacklevel=3,
    )
