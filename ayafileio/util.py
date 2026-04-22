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
        parts = release.split('.')
        if len(parts) >= 2:
            major = int(parts[0])
            minor = int(parts[1])
            if major > 5 or (major == 5 and minor >= 1):
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


def _check_dispatch_io_available():
    """检测 macOS Dispatch I/O 是否可用"""
    if sys.platform != "darwin":
        return False
    
    # 尝试导入扩展并检查后端信息
    try:
        from ._ayafileio import get_backend_info
        info = get_backend_info()
        return info.get("backend") == "dispatch_io"
    except (ImportError, AttributeError):
        pass
    
    # 如果无法导入扩展，检查系统版本（macOS 10.10+ 支持 Dispatch I/O）
    try:
        import platform
        version = platform.mac_ver()[0]
        if version:
            parts = version.split('.')
            if len(parts) >= 2:
                major = int(parts[0])
                minor = int(parts[1])
                # macOS 10.10 (Yosemite) 及以上支持 Dispatch I/O
                return major >= 11 or (major == 10 and minor >= 10)
    except:
        pass
    
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
        # Linux: 可能支持 io_uring
        if _check_io_uring_available():
            return
        warnings.warn(
            "Current Linux backend uses ThreadIOBackend (fake async). "
            "io_uring is not available on your system (kernel < 5.1 or liburing not installed).",
            UserWarning,
            stacklevel=3
        )
    elif sys.platform == "darwin":
        # macOS: 检测 Dispatch I/O 是否可用
        if _check_dispatch_io_available():
            return  # 真异步，不警告
        
        # 尝试在导入后再次检测（可能扩展还没加载）
        try:
            from ._ayafileio import get_backend_info
            info = get_backend_info()
            if info.get("backend") == "dispatch_io":
                return
        except (ImportError, AttributeError):
            pass
        
        warnings.warn(
            "macOS is using ThreadIOBackend (fake async). "
            "Dispatch I/O (native async) is not available. "
            "This may be due to an older macOS version (< 10.10).",
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