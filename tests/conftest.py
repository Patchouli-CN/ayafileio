"""pytest 引导文件 — 两个测试入口都靠它对齐。

本目录的用例有两种运行方式：

    python -m pytest            # 统一入口（CI 走这条）
    python tests/test_xxx.py    # 脚本模式，打印更详细的逐项报告

conftest 只做一件 pytest 需要、脚本模式本来就满足的事：把 ``tests/`` 放进
``sys.path``，让 ``from i18n import i18n`` 在 pytest 的 import 模式下也能解析。
"""

import sys
from pathlib import Path

_TESTS_DIR = str(Path(__file__).resolve().parent)
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)
