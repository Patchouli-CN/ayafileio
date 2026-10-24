"""pytest 引导文件 — 两个测试入口都靠它对齐。

本目录的用例有两种运行方式：

    python -m pytest            # 统一入口（CI 走这条）
    python tests/test_xxx.py    # 脚本模式，打印更详细的逐项报告

conftest 做两件事：

1. 把 ``tests/`` 放进 ``sys.path``，让 ``from i18n import i18n`` 在 pytest 的
   import 模式下也能解析；
2. 读 ``AYAFILEIO_TEST_BACKEND`` 环境变量，用 ``configure({"backend": ...})``
   把整套用例切到指定后端。

第 2 条是为了让**线程池回退后端**终于有 CI 覆盖：它能跑起来的前提是 io_uring /
GCD / IOCP 全都不参与（Linux 装了 liburing 就自动走 io_uring、macOS 走 GCD、
Windows 走 IOCP），所以正常 CI 里它一行都跑不到，而它恰恰是「io_uring 不可用」
时的唯一路径。
"""

import os
import sys
from pathlib import Path

_TESTS_DIR = str(Path(__file__).resolve().parent)
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)


# ════════════════════════════════════════════════════════════════════════════
# 后端覆盖（CI 用）
# ════════════════════════════════════════════════════════════════════════════

_TEST_BACKEND = os.environ.get("AYAFILEIO_TEST_BACKEND", "").strip()

if _TEST_BACKEND:
    import ayafileio

    try:
        ayafileio.configure({"backend": _TEST_BACKEND})
    except ValueError as exc:   # 本平台没有这个后端
        raise RuntimeError(
            f"AYAFILEIO_TEST_BACKEND={_TEST_BACKEND!r} 在本平台不可用：{exc}"
        ) from exc

    # 切完立刻核对，别让「配置静默无效」把整轮 CI 变成假绿
    _active = ayafileio.get_config()["backend"]
    if _active != _TEST_BACKEND:
        raise RuntimeError(
            f"configure({{'backend': {_TEST_BACKEND!r}}}) 没有生效（当前 {_active!r}）"
        )

    import pytest

    @pytest.fixture(autouse=True)
    def _reenforce_test_backend():
        """每个用例结束后把后端重新压回 CI 指定的值。

        有些用例会调 ``reset_config()``（例如 test_capabilities 的 sqpoll 校验
        在 finally 里重置），那会把 ``backend`` 一并重置回 ``auto``——于是后面的
        用例悄悄跑在平台默认后端上，整轮「回退后端」覆盖就白跑了。实测过这个
        坑：Linux 上 reset 之后悄悄切回 io_uring，而当时 io_uring 那份大小缓存
        还没修，红得莫名其妙。
        """
        yield
        if ayafileio.get_config()["backend"] != _TEST_BACKEND:
            ayafileio.configure({"backend": _TEST_BACKEND})
