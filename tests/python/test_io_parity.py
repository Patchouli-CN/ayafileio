"""与内置 ``io`` 语义对齐的回归测试。

来源：一次针对 stdlib ``io`` 的差分测试（同一串操作两边各跑一遍，比较返回值
与异常类型）挖出两处静默偏差：

* **负偏移的 ``seek()`` 被当成无符号数**。``seek(-1, 0)`` 返回
  ``18446744073709551615``，位置被静默污染；此后每次 ``read()`` 只报
  ``ReadFile failed, errno 87``——错误信息和真正的原因毫无关系。Windows /
  io_uring / 线程池三个后端都有，只有 macOS 的 GCD 后端早就做了校验。
* **非法模式串被接受**。``"rbb"`` / ``"rw"`` / ``"rtb"`` / ``"r++"`` 全部
  通过校验，其中 ``"rw"`` 最危险：挑起始模式字符时取第一个命中项，于是一个
  写着读写的模式实际按**只读**打开。

两者都对齐内置 ``open()`` 的行为（内置对负 seek 抛 ``OSError(EINVAL)``，
这里用 ``ValueError``——与本项目 macOS 后端既有的报错类型保持一致，避免
同一个错误在不同平台上抛出不同异常）。
"""

import pytest

import ayafileio


# ════════════════════════════════════════════════════════════════════════════
# 负偏移 seek
# ════════════════════════════════════════════════════════════════════════════

NEGATIVE_SEEKS = [
    (-1, 0),
    (-10, 0),
    (-1, 1),      # 从当前位置 0 往后退
    (-11, 1),
    (-11, 2),     # 文件 10 字节，从末尾再往前 11 → -1
]


@pytest.mark.parametrize("offset,whence", NEGATIVE_SEEKS)
async def test_negative_seek_is_rejected(tmp_path, offset, whence):
    """负目标位置抛 ValueError，且**不改变**当前逻辑位置。"""
    path = tmp_path / "neg.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        before = await handle.tell()
        with pytest.raises(ValueError):
            await handle.seek(offset, whence)
        assert await handle.tell() == before, (
            f"seek({offset}, {whence}) 报错后位置仍被改动："
            f"{before} -> {await handle.tell()}"
        )


async def test_stream_stays_usable_after_rejected_seek(tmp_path):
    """负 seek 被拒之后，流必须还能正常读。

    回归点：位置曾被静默改成 2^64-1，随后 ``read()`` 报
    ``ReadFile failed``（Windows）/ 读到 EOF（POSIX），数据再也读不出来。
    """
    path = tmp_path / "usable.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        await handle.seek(4)
        with pytest.raises(ValueError):
            await handle.seek(-1, 0)
        assert await handle.tell() == 4
        assert await handle.read() == b"456789"


async def test_negative_seek_mid_stream(tmp_path):
    """从非零位置往前退过头，同样要拒绝，而不是回绕。"""
    path = tmp_path / "mid.bin"
    path.write_bytes(b"x" * 32)

    async with ayafileio.open(str(path), "rb") as handle:
        await handle.seek(20)
        with pytest.raises(ValueError):
            await handle.seek(-21, 1)
        assert await handle.tell() == 20
        # 合法回退仍然可用
        assert await handle.seek(-10, 1) == 10


async def test_rewind_for_readline_still_works(tmp_path):
    """内部靠 ``seek(-n, 1)`` 回退预读缓冲，校验不能把这个路径误杀。"""
    path = tmp_path / "rewind.bin"
    path.write_bytes(b"line1\nline2\nline3\n")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.readline() == b"line1\n"
        assert await handle.tell() == 6
        assert await handle.readline() == b"line2\n"
        assert await handle.tell() == 12
        await handle.seek(0)
        assert await handle.read(5) == b"line1"


# ════════════════════════════════════════════════════════════════════════════
# 模式串校验
# ════════════════════════════════════════════════════════════════════════════

INVALID_MODES = [
    "rbb",   # 重复 b
    "rr",    # 重复基础模式
    "rw",    # 两个基础模式（此前被静默当成只读）
    "rtb",   # 文本 + 二进制
    "r++",   # 重复 +
    "r+ab",  # 两个基础模式
    "",      # 空
    "b",     # 只有 b
    "+",     # 只有 +
    "rtb+",  # 文本+二进制且重复
]


@pytest.mark.parametrize("mode", INVALID_MODES)
def test_invalid_mode_is_rejected(tmp_path, mode):
    path = tmp_path / "mode.bin"
    path.write_bytes(b"x")

    with pytest.raises(ValueError):
        ayafileio.open(str(path), mode)


VALID_MODES = ["r", "rb", "rt", "r+", "rb+", "r+b", "rt+",
               "w", "wb", "wt", "w+", "w+b", "a", "ab", "a+", "ab+", "x", "xb", "x+"]


@pytest.mark.parametrize("mode", VALID_MODES)
async def test_valid_mode_is_accepted(tmp_path, mode):
    path = tmp_path / f"ok_{mode.replace('+', 'p')}.bin"
    if "x" in mode and "r" not in mode:
        pass                      # x 模式要求文件不存在，别预建
    else:
        path.write_bytes(b"x")

    async with ayafileio.open(str(path), mode) as handle:
        assert handle.mode.replace("b", "").replace("t", "")[0] in "rwax"


def test_invalid_mode_beats_binary_encoding_check(tmp_path):
    """模式本身非法时，先报模式错误（与内置 open() 的优先级一致）。"""
    path = tmp_path / "order.bin"
    path.write_bytes(b"x")

    with pytest.raises(ValueError, match="invalid mode|Invalid mode"):
        ayafileio.open(str(path), "rbb", encoding="utf-8")


# ════════════════════════════════════════════════════════════════════════════
# 打开模式决定可读/可写：越界操作抛 io.UnsupportedOperation
# ════════════════════════════════════════════════════════════════════════════
#
# 内置 open() 对「用错模式」的操作抛 io.UnsupportedOperation（它的 MRO 同时含
# OSError 和 ValueError，所以 `except OSError` / `except ValueError` 两种既有
# 写法都继续有效）。此前这里会把请求真的发到 OS，拿回一个与原因无关的
# OSError（Windows 上 errno 5、POSIX 上 EBADF）——错误类型和消息都对不上。


UNSUPPORTED_CASES = [
    ("wb", "read", "read"),
    ("wb", "read", "readline"),
    ("wb", "read", "readlines"),
    ("wb", "read", "read_at"),
    ("wb", "read", "read_many"),
    ("wb", "read", "readinto"),
    ("ab", "read", "read"),
    ("rb", "write", "write"),
    ("rb", "write", "write_at"),
    ("rb", "write", "write_many"),
    ("rb", "truncate", "truncate"),
]


@pytest.mark.parametrize("mode,op,method", UNSUPPORTED_CASES)
async def test_wrong_mode_raises_unsupported_operation(tmp_path, mode, op, method):
    """对只读文件写入、或对只写文件读取，都要抛 io.UnsupportedOperation。"""
    import io

    path = tmp_path / f"guard_{mode}_{method}.bin"
    path.write_bytes(b"abcd")

    call = {
        "read": lambda f: f.read(2),
        "readline": lambda f: f.readline(),
        "readlines": lambda f: f.readlines(),
        "read_at": lambda f: f.read_at(0, 2),
        "read_many": lambda f: f.read_many([(0, 2)]),
        "readinto": lambda f: f.readinto(bytearray(4)),
        "write": lambda f: f.write(b"x"),
        "write_at": lambda f: f.write_at(0, b"x"),
        "write_many": lambda f: f.write_many([(0, b"x")]),
        "truncate": lambda f: f.truncate(1),
    }[method]

    async with ayafileio.open(str(path), mode) as handle:
        with pytest.raises(io.UnsupportedOperation) as info:
            await call(handle)

    assert str(info.value) == op, f"消息应与内置一致（{op}），实际 {info.value!s}"
    # MRO 与内置一致：既是 OSError 也是 ValueError，既有 catch 写法都还能用
    assert isinstance(info.value, OSError)
    assert isinstance(info.value, ValueError)


async def test_chunk_on_write_only_raises(tmp_path):
    """chunk() 是异步生成器：守卫在首次迭代时触发（与内置的惰性一致）。"""
    import io

    path = tmp_path / "guard_chunk.bin"
    path.write_bytes(b"abcd")

    async with ayafileio.open(str(path), "wb") as handle:
        generator = handle.chunk(2)
        with pytest.raises(io.UnsupportedOperation):
            await generator.__anext__()


@pytest.mark.parametrize("mode", ["r+b", "w+b", "a+b", "x+b"])
async def test_read_write_modes_are_not_guarded(tmp_path, mode):
    """带 + 的模式读写都必须放行。"""
    path = tmp_path / f"rw_{mode.replace('+', 'p')}.bin"
    if "x" not in mode:
        path.write_bytes(b"abcdef")

    async with ayafileio.open(str(path), mode) as handle:
        assert await handle.write(b"XY") == 2
        await handle.truncate(2)
        await handle.seek(0)
        data = await handle.read()

    # 只断言「读写都没被守卫拦住」+ 长度；内容随模式不同（a+ 写在末尾，
    # 所以最终字节不一定是 b"XY"）
    assert len(data) == 2


async def test_truncate_allowed_on_write_only(tmp_path):
    """只写模式截断是合法的（内置 BufferedWriter.truncate 同样允许）。"""
    path = tmp_path / "trunc_w.bin"

    async with ayafileio.open(str(path), "wb") as handle:
        await handle.write(b"abcdef")
        await handle.truncate(3)

    assert path.read_bytes() == b"abc"
