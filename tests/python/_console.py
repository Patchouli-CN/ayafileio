"""脚本模式的控制台设置。

这些用例既可以直接 ``python tests/python/test_xxx.py`` 跑（打印中文/emoji 报告），
也可以被 ``python -m pytest`` 收集。两种模式对 ``sys.stdout`` 的要求是冲突的：

* 脚本模式希望把 Windows 控制台的编码换成 UTF-8（否则 ✓ / 📋 会乱码）；
* pytest 模式**绝不能**在 import 时替换 ``sys.stdout`` —— pytest 的 capture
  对象挂在原 stream 上，被替换掉之后 ``capture.snap()`` 会摸到已关闭的文件，
  整个会话直接死在 ``ValueError: I/O operation on closed file``。

所以只在 ``__main__`` 里调用，而且就地 ``reconfigure``，不换对象。
"""

import sys


def use_utf8_console() -> None:
    """尽力把 Windows 控制台切到 UTF-8（失败就算了，不影响测试结果）。"""
    if sys.platform != "win32":
        return
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            # 不是 TextIOWrapper、已被替换成 capture 对象，或控制台拒绝
            pass
