""" 工具模块 """
import sys
import warnings

def _check_io_uring_available():
    """纯 Python 检测 io_uring 是否可用"""
    if sys.platform != "linux":
        return False
    
    # 方法1：检查内核版本
    try:
        import platform
        release = platform.release()
        # 解析内核版本，格式如 "5.10.0" 或 "6.1.0"
        parts = release.split('.')
        if len(parts) >= 2:
            major = int(parts[0])
            minor = int(parts[1])
            if major > 5 or (major == 5 and minor >= 1):
                # 内核版本 >= 5.1，理论上支持
                pass
            else:
                return False
    except:
        pass
    
    # 方法2：检查 liburing 库文件是否存在
    import ctypes
    import ctypes.util
    
    liburing_path = ctypes.util.find_library("uring")
    if liburing_path:
        try:
            lib = ctypes.CDLL(liburing_path)
            # 尝试调用一个简单的函数确认可用
            # 不实际初始化，只检查符号是否存在
            if hasattr(lib, 'io_uring_queue_init'):
                return True
        except:
            pass
    
    # 方法3：检查 /usr/include/liburing.h 是否存在
    import os
    include_paths = [
        "/usr/include/liburing.h",
        "/usr/local/include/liburing.h",
    ]
    for path in include_paths:
        if os.path.exists(path):
            return True
    
    return False

_WARNED = False
""" 是否已经发出过警告 """


def warn_fake_async():
    global _WARNED
    """如果当前平台不支持真异步，发出 UserWarning"""
    if _WARNED:
        return
    _WARNED = True
    if sys.platform == "win32":
        # Windows: 真异步 IOCP
        return
    elif sys.platform == "linux":
        # Linux: 可能支持 io_uring，由 C++ 层检测
        if _check_io_uring_available():
            return  # 真异步，不警告
        warnings.warn(
            "Current Linux backend uses ThreadIOBackend (fake async). "
            "the io_uring has not support on your linux kernel! (<= 5.1)",
            UserWarning,
            stacklevel=3
        )
    elif sys.platform == "darwin":
        # MacOS: 只能假异步，因为系统不支持
        warnings.warn(
            "MacOS does not support native async file I/O. "
            "Falling back to ThreadIOBackend (fake async). "
            "This is an OS limitation, not a library issue.",
            UserWarning,
            stacklevel=3
        )
    else:
        # 其他 Unix-like 系统
        warnings.warn(
            f"Platform '{sys.platform}' uses ThreadIOBackend (fake async). "
            "Native async I/O is not available on this platform.",
            UserWarning,
            stacklevel=3
        )
