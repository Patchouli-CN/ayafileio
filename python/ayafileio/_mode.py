"""文件打开模式的校验与规范化。"""

_VALID_MODE_CHARS = frozenset("rwaxbt+")


def _normalize_mode(mode: str, encoding: "str | None", newline: "str | None") -> "tuple[str, bool]":
    """校验打开模式并规范化成传给 C++ 的二进制形式，返回 (clean_mode, is_text)。

    ``AsyncFile.__init__`` 与 ``aopen`` 共用——两条打开路径对模式的
    校验/清洗必须完全一致。
    """
    if any(c not in _VALID_MODE_CHARS for c in mode):
        raise ValueError(f"Invalid mode: '{mode}'")

    if sum(mode.count(c) for c in "rwax") != 1:
        raise ValueError(
            f"invalid mode: {mode!r}: must have exactly one of "
            "create/read/write/append mode"
        )
    if mode.count("+") > 1 or mode.count("b") > 1 or mode.count("t") > 1:
        raise ValueError(f"invalid mode: {mode!r}")
    if "b" in mode and "t" in mode:
        raise ValueError(
            f"invalid mode: {mode!r}: can't have text and binary mode at once"
        )

    is_text = "b" not in mode
    if not is_text:
        if encoding is not None:
            raise ValueError("Binary mode does not accept an encoding argument.")
        if newline is not None:
            raise ValueError("Binary mode does not accept a newline argument.")

    clean_mode = mode.replace("t", "")
    if "b" not in clean_mode:
        base_char = next(c for c in clean_mode if c in "rwax")
        clean_mode = base_char + ("+" if "+" in clean_mode else "") + "b"
    return clean_mode, is_text
