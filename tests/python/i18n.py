"""
i18n 国际化模块 — 自动检测系统语言，通过 YAML 映射翻译字符串。

用法:
    from i18n import i18n
    print(i18n("Hello, world!"))  # 中文环境 → "你好，世界！"

语言文件: tests/lang/<lang>.yaml
"""

import locale
import os
import sys
import yaml
from pathlib import Path
from functools import lru_cache

_LANG_DIR = Path(__file__).parent / "lang"


def _detect_language() -> str:
    """自动检测系统语言，返回 'zh' 或 'en'"""
    # 1. 环境变量
    for var in ("AYAFILEIO_LANG", "LANGUAGE", "LANG", "LC_ALL", "LC_MESSAGES"):
        val = os.environ.get(var, "")
        if val:
            low = val.lower().split(".")[0].split("_")[0]
            if low in ("zh", "cn"):
                return "zh"
            if low == "en":
                return "en"

    # 2. 系统 locale
    try:
        sys_locale = locale.getdefaultlocale()
        if sys_locale and sys_locale[0]:
            low = sys_locale[0].lower().split("_")[0]
            if low in ("zh", "cn"):
                return "zh"
    except (ValueError, locale.Error):
        pass

    # 3. Windows 控制台代码页（936 = 简体中文 GBK）
    if sys.platform == "win32":
        try:
            import ctypes
            if ctypes.windll.kernel32.GetConsoleOutputCP() == 936:
                return "zh"
        except Exception:
            pass

    return "en"


@lru_cache(maxsize=1)
def _load_translations() -> dict:
    """加载当前语言的翻译映射表"""
    lang = _detect_language()
    if lang == "en":
        return {}

    yaml_path = _LANG_DIR / f"{lang}.yaml"
    if not yaml_path.exists():
        return {}

    try:
        with open(yaml_path, "r", encoding="utf-8") as f:
            return yaml.safe_load(f) or {}
    except Exception:
        return {}


def i18n(text: str) -> str:
    """
    国际化翻译。

    Args:
        text: 英文原文（作为 key）

    Returns:
        翻译后的文本。英文环境返回原文，中文环境返回翻译。

    Example:
        >>> i18n("Performance Comparison")
        '性能对比'  # zh
    """
    translations = _load_translations()
    if not translations:
        return text
    return translations.get(text, text)


def i18n_format(text: str, **kwargs) -> str:
    """
    先翻译，再格式化。

    Args:
        text: 英文原文（作为 key）
        **kwargs: 格式化参数

    Example:
        >>> i18n_format("Concurrency: {n}", n=50000)
        '并发: 50000'  # zh
    """
    return i18n(text).format(**kwargs)


# ── 辅助：清除缓存（测试用）──────────────────────────────────────────────
def reload_lang():
    """清除翻译缓存，重新检测语言"""
    _load_translations.cache_clear()
