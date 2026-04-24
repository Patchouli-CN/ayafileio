import sys
from ._ayafileio import (
    set_handle_pool_limits as _set_handle_pool_limits,
    get_handle_pool_limits as _get_handle_pool_limits,
)

# 尝试导入 Windows 专用的 set_iocp_worker_count
_has_native_set_iocp = False
try:
    if sys.platform == "win32":
        from ._ayafileio import set_iocp_worker_count as _set_iocp_worker_count
        _has_native_set_iocp = True
except Exception:
    _has_native_set_iocp = False

# 尝试导入跨平台的 set_worker_count
_has_native_set_worker = False
try:
    from ._ayafileio import set_worker_count as _set_worker_count
    _has_native_set_worker = True
except Exception:
    _has_native_set_worker = False


def set_handle_pool_limits(max_per_key: int, max_total: int) -> None:
    if max_per_key <= 0 or max_total <= 0:
        raise ValueError("max_per_key and max_total must be positive integers")
    _set_handle_pool_limits(max_per_key, max_total)

def get_handle_pool_limits() -> tuple[int, int]:
    return _get_handle_pool_limits()

def set_io_worker_count(count: int = 0) -> None:
    if not isinstance(count, int):
        raise TypeError("count must be int")
    if not (count == 0 or (1 <= count <= 128)):
        raise ValueError("worker count must be 0 (auto) or 1-128")
    if _has_native_set_worker:
        _set_worker_count(count)
    elif _has_native_set_iocp:
        _set_iocp_worker_count(count)
    else:
        globals()["_io_worker_count"] = count

def set_iocp_worker_count(count: int = 0) -> None:
    set_io_worker_count(count)