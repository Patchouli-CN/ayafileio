"""后端一致性（conformance）测试：同一套不变量，逐个可用后端跑一遍。

为什么要有这一份
================

`src/backend/` 下有四个各自独立的实现（IOCP / io_uring / Dispatch I/O / 线程池），
文件生命周期语义（open / close / seek / truncate / append / 大小缓存）在每一份里
各推导了一遍。单元测试只覆盖"能不能读写"是不够的——历史上真正出问题的是**语义
分歧**，而且每个都被单独修过：

    ee5ab44  POSIX 两个后端补上文件大小缓存刷新
    9ca83d4  io_uring 后端的大小缓存刷新
    ab6b521  ThreadIOBackend 的 pending 配平 + 文件原语跨平台化
    cd3cffa  Windows IOCP 的 flush 走线程池
    1d81fcf  fileno 句柄 / 事件循环引用 / 大小缓存 / quarantine 排空

这类 commit 的存在本身就说明：**不变量没有单一所有者**。这份测试的作用是在
"某天有人重构"之前，先把"当前四个后端到底哪里不一致"变成一堆会亮红灯的断言。

覆盖策略
========

本机只能直接跑平台自带的两个后端，但 CI 矩阵合起来刚好覆盖全部四个：

    Windows : iocp          + thread_pool
    Linux   : io_uring      + thread_pool
    macOS   : dispatch_io   + thread_pool

参数化在收集期按 ``configure({"backend": ...})`` 的真实可用性生成，所以
不可用的后端会被**跳过而不是假绿**。

每条断言的失败消息都带后端名——这是这份测试存在的意义，报错必须能直接告诉
你是哪个后端偏离了。
"""

import os

import pytest

import ayafileio

# 与 test_backend_select.FIXED_BACKENDS 保持一致；"auto" 不是具体实现。
FIXED_BACKENDS = ("iocp", "io_uring", "dispatch_io", "thread_pool")


def _is_available(name: str) -> bool:
    """本构建/本机器上能否强制选中该后端（临时改配置，立刻复原）。"""
    before = ayafileio.get_config()["backend"]
    try:
        ayafileio.configure({"backend": name})
        return ayafileio.get_config()["backend"] == name
    except ValueError:
        return False
    finally:
        ayafileio.configure({"backend": before})


AVAILABLE_BACKENDS = tuple(name for name in FIXED_BACKENDS if _is_available(name))


@pytest.fixture(params=AVAILABLE_BACKENDS or ("auto",))
def backend(request):
    """把全局后端切到 *request.param*，用例结束后复原。"""
    before = ayafileio.get_config()["backend"]
    ayafileio.configure({"backend": request.param})
    try:
        yield request.param
    finally:
        ayafileio.configure({"backend": before})


def _append_externally(path, payload: bytes) -> None:
    """绕过 ayafileio 直接改文件，用来检验后端的大小缓存是否过期。"""
    with open(path, "ab") as handle:
        handle.write(payload)


# ════════════════════════════════════════════════════════════════════════════
# 1. 大小缓存：外部追加之后必须看得见
# ════════════════════════════════════════════════════════════════════════════


async def test_size_cache_refresh_on_read(backend, tmp_path):
    """读到 EOF 之后，外部追加的内容必须能被下一次 read() 看见。

    后端会把文件大小缓存起来（省一次 stat）。缓存不刷新时的表现是**静默少读**：
    不报错、不截断，就是永远读不到新数据。
    """
    path = tmp_path / "grow.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.read() == b"0123456789", backend
        _append_externally(path, b"abc")
        tail = await handle.read()
    assert tail == b"abc", f"{backend}: 外部追加后 read() 没看到新数据，得到 {tail!r}"


async def test_size_cache_refresh_on_read_at(backend, tmp_path):
    path = tmp_path / "grow_at.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.read_at(0, 10) == b"0123456789", backend
        _append_externally(path, b"XYZ")
        tail = await handle.read_at(10, 3)
    assert tail == b"XYZ", f"{backend}: 外部追加后 read_at() 没看到新数据，得到 {tail!r}"


async def test_size_cache_refresh_on_seek_end(backend, tmp_path):
    path = tmp_path / "grow_seek.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        await handle.read()
        _append_externally(path, b"abcd")
        position = await handle.seek(0, 2)
    assert position == 14, f"{backend}: seek(0, 2) 报 {position}，实际文件已 14 字节"


# ════════════════════════════════════════════════════════════════════════════
# 2. EOF 规划
# ════════════════════════════════════════════════════════════════════════════


async def test_read_at_beyond_eof_is_empty(backend, tmp_path):
    path = tmp_path / "eof.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        at_end = await handle.read_at(10, 10)
        past_end = await handle.read_at(9999, 10)
        across = await handle.read_at(8, 10)
    assert at_end == b"", f"{backend}: read_at(size, n) 应为 b''，得到 {at_end!r}"
    assert past_end == b"", f"{backend}: read_at(远超 size, n) 应为 b''，得到 {past_end!r}"
    assert across == b"89", f"{backend}: 跨越 EOF 应截断返回 b'89'，得到 {across!r}"


async def test_short_read_at_eof(backend, tmp_path):
    path = tmp_path / "short.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        first = await handle.read(100)
        second = await handle.read(100)
        third = await handle.read(0)
    assert first == b"0123456789", f"{backend}: 不足时 read(100) 应返回全部 10 字节"
    assert second == b"", f"{backend}: EOF 之后 read() 应为 b''，得到 {second!r}"
    assert third == b"", f"{backend}: read(0) 应为 b''"


# ════════════════════════════════════════════════════════════════════════════
# 3. seek 语义
# ════════════════════════════════════════════════════════════════════════════


@pytest.mark.parametrize(
    "offset, whence",
    [(-1, 0), (-1, 1), (0, -1), (0, 3)],
    ids=["neg-abs", "neg-rel", "bad-whence-neg", "bad-whence-big"],
)
async def test_seek_rejects_invalid(backend, tmp_path, offset, whence):
    """非法 seek 必须报错，而不是静默钳到 0（8a03ba8 修的就是静默偏差）。"""
    path = tmp_path / "seek.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        with pytest.raises(ValueError):
            await handle.seek(offset, whence)
        assert await handle.tell() == 0, f"{backend}: 失败的 seek 不应改动位置"


async def test_seek_and_tell_roundtrip(backend, tmp_path):
    path = tmp_path / "seek_ok.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.seek(4) == 4, backend
        assert await handle.tell() == 4, backend
        assert await handle.read(2) == b"45", backend
        assert await handle.seek(-2, 1) == 4, f"{backend}: seek(-2, 1) 应回到 4"
        assert await handle.seek(0, 2) == 10, f"{backend}: seek(0, 2) 应为 10"


# ════════════════════════════════════════════════════════════════════════════
# 4. append 语义
# ════════════════════════════════════════════════════════════════════════════


async def test_append_opens_at_eof(backend, tmp_path):
    """append 模式的初始位置是 EOF —— 与 CPython 的 ``open(path, "a+b")`` 一致。

    这条容易被实现成 0（"文件刚打开，位置当然是 0"），但两个平台的标准库都在
    open 时 seek 到末尾；后端若不照做，``a+b`` 里第一次 read 会读到开头而不是 EOF。
    """
    path = tmp_path / "append_pos.bin"
    path.write_bytes(b"0123456789")

    handle = ayafileio.open(str(path), "a+b")
    position = await handle.tell()
    await handle.close()
    with open(path, "a+b") as reference:
        expected = reference.tell()
    assert position == expected, f"{backend}: append 初始位置 {position}，CPython 是 {expected}"


async def test_append_always_writes_at_eof(backend, tmp_path):
    """append 模式的写入必须永远落在 EOF，即使刚 seek(0)。"""
    path = tmp_path / "append.bin"
    path.write_bytes(b"base")  # 4 字节

    async with ayafileio.open(str(path), "ab") as handle:
        await handle.write(b"-1")  # +2 -> 6
        assert await handle.tell() == 6, f"{backend}: append 后 tell 应为 6"
        await handle.seek(0)
        await handle.write(b"-2")  # +2 -> 8
        assert await handle.tell() == 8, f"{backend}: seek(0) 之后写入仍应在 EOF（tell=8）"

    assert path.read_bytes() == b"base-1-2", f"{backend}: 落盘内容是 {path.read_bytes()!r}"


async def test_append_positioned_write_rejected(backend, tmp_path):
    path = tmp_path / "append_at.bin"
    path.write_bytes(b"base")

    async with ayafileio.open(str(path), "ab") as handle:
        with pytest.raises(ValueError):
            await handle.write_at(0, b"XX")


async def test_append_plus_read_position_independent(backend, tmp_path):
    """`a+b`：读位置和写位置互不干扰。

    注意进入时位置在 EOF（见 test_append_opens_at_eof），所以要显式 seek(0)。
    """
    path = tmp_path / "append_read.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "a+b") as handle:
        await handle.seek(0)
        assert await handle.read(3) == b"012", backend
        await handle.write(b"XY")
        assert await handle.tell() == 12, f"{backend}: a+b 写入后 tell 应为 12"
        assert await handle.read_at(0, 3) == b"012", backend
    assert path.read_bytes() == b"0123456789XY", f"{backend}: 落盘内容是 {path.read_bytes()!r}"


# ════════════════════════════════════════════════════════════════════════════
# 5. truncate 语义
# ════════════════════════════════════════════════════════════════════════════


async def test_truncate_shrinks_and_clamps_position(backend, tmp_path):
    path = tmp_path / "trunc.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "r+b") as handle:
        await handle.seek(8)
        await handle.truncate(4)
        assert await handle.tell() == 4, f"{backend}: truncate 之后 tell 应被夹到 4"
        assert await handle.read() == b"", f"{backend}: 截断后应立刻 EOF"
        assert await handle.read_at(2, 10) == b"23", f"{backend}: 截断后 read_at 应只见 2 字节"
    assert path.stat().st_size == 4, f"{backend}: 文件大小应为 4"


async def test_truncate_extends_with_zeros(backend, tmp_path):
    """扩大文件时新增区域必须是 0——POSIX 与 NTFS 语义一致，但实现路径不同。"""
    path = tmp_path / "grow_trunc.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "r+b") as handle:
        await handle.truncate(20)
        tail = await handle.read_at(10, 10)
    assert path.stat().st_size == 20, f"{backend}: 扩大后大小应为 20"
    assert len(tail) == 10, f"{backend}: 扩大后读到 {len(tail)} 字节，应为 10"
    assert tail == b"\x00" * 10, f"{backend}: 扩大区域应全为 0，得到 {tail!r}"


# ════════════════════════════════════════════════════════════════════════════
# 6. close 语义
# ════════════════════════════════════════════════════════════════════════════


async def test_close_is_idempotent(backend, tmp_path):
    path = tmp_path / "close.bin"
    path.write_bytes(b"x")

    handle = ayafileio.open(str(path), "rb")
    await handle.close()
    assert handle.closed is True, backend
    await handle.close()  # 第二次必须是 no-op
    assert handle.closed is True, backend


async def test_operations_after_close_raise(backend, tmp_path):
    path = tmp_path / "closed.bin"
    path.write_bytes(b"0123456789")

    handle = ayafileio.open(str(path), "rb")
    await handle.close()
    for name, call in (
        ("read", lambda: handle.read(1)),
        ("read_at", lambda: handle.read_at(0, 1)),
        ("read_many", lambda: handle.read_many([(0, 1)])),
        ("seek", lambda: handle.seek(0)),
        ("tell", lambda: handle.tell()),
        ("flush", lambda: handle.flush()),
    ):
        with pytest.raises(ValueError):
            await call()


# ════════════════════════════════════════════════════════════════════════════
# 7. 资源身份
# ════════════════════════════════════════════════════════════════════════════


async def test_fileno_is_stable(backend, tmp_path):
    """重复取 fileno 必须返回同一个号码（1d81fcf 修过每次新建描述符的泄漏）。"""
    path = tmp_path / "fd.bin"
    path.write_bytes(b"x")

    async with ayafileio.open(str(path), "rb") as handle:
        first = handle.fileno()
        second = handle.fileno()
        assert isinstance(first, int) and first >= 0, f"{backend}: fileno 返回 {first!r}"
        assert first == second, f"{backend}: fileno 不稳定（{first} vs {second}）"
        assert os.fstat(first).st_size == 1, f"{backend}: fileno 指向的不是这个文件"


# ════════════════════════════════════════════════════════════════════════════
# 8. 预读与位置读的相互影响
# ════════════════════════════════════════════════════════════════════════════


async def test_read_at_does_not_disturb_position(backend, tmp_path):
    """read_at 不得改动逻辑位置，也不得打乱 readline 的预读缓冲。"""
    path = tmp_path / "mix.bin"
    path.write_bytes(b"line1\nline2\nline3\n")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.readline() == b"line1\n", backend
        after_line = await handle.tell()
        assert await handle.read_at(0, 5) == b"line1", backend
        assert await handle.tell() == after_line, (
            f"{backend}: read_at 把位置从 {after_line} 改成了 {await handle.tell()}"
        )
        assert await handle.readline() == b"line2\n", (
            f"{backend}: read_at 之后 readline 拿到了 {await handle.readline()!r}"
        )


async def test_read_many_preserves_order_and_position(backend, tmp_path):
    path = tmp_path / "many.bin"
    payload = bytes(range(256)) * 4
    path.write_bytes(payload)

    async with ayafileio.open(str(path), "rb") as handle:
        spans = [(0, 4), (100, 3), (1023, 1), (1024, 4)]
        results = await handle.read_many(spans)
        expected = [payload[o : o + n] for o, n in spans]
        assert results == expected, f"{backend}: read_many 结果 {results!r} != {expected!r}"
        assert await handle.tell() == 0, f"{backend}: read_many 不应改动逻辑位置"
