import sys
from ._ayafileio import drain_buffer_pool as _drain_buffer

# 尝试导入 Windows 专用的 set_iocp_worker_count
_has_native_set_iocp = False
try:
    if sys.platform == "win32":
        from ._ayafileio import set_iocp_worker_count as _set_iocp_worker_count

        _has_native_set_iocp = True
except ImportError:
    pass

# 尝试导入跨平台的 set_worker_count
_has_native_set_worker = False
try:
    from ._ayafileio import set_worker_count as _set_worker_count

    _has_native_set_worker = True
except ImportError:
    pass

# ── 模块级状态 ──────────────────────────────────────────────────────────

_io_worker_count = 0

def drain_buffer_pool() -> None: _drain_buffer()


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
        global _io_worker_count
        _io_worker_count = count


def set_iocp_worker_count(count: int = 0) -> None:
    set_io_worker_count(count)
