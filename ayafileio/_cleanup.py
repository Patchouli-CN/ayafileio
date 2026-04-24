try:
    from ._ayafileio import cleanup as _native_cleanup
except Exception:
    _native_cleanup = None

def _register_native_cleanup() -> None:
    if _native_cleanup is None:
        return
    try:
        import atexit as _atexit

        def _cleanup_wrapper() -> None:
            try:
                _native_cleanup()
            except Exception:
                pass

        _atexit.register(_cleanup_wrapper)
    except Exception:
        pass

# 模块导入时自动注册
_register_native_cleanup()