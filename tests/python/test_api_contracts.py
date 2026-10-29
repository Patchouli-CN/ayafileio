"""API 契约回归测试：三条曾经实际坏掉、且端到端用例都没覆盖的行为。

每一条都来自"用探针实测 → 确认是 bug → 修复"这个流程，不是猜出来的：

1. ``async with`` 退出时 ``flush()`` 失败会跳过 ``close()``，句柄泄漏到 GC。
2. ``configure()`` 静默忽略未知 key，把拼写错误变成"设置成功"的假象。
3. ``set_io_worker_count()`` / ``set_iocp_worker_count()``（1.x 遗留入口）是
   静默空操作 —— 而 worker 数恰是 IOCP / 线程池最关键的调优旋钮。

这些都属于"不报错、但行为与文档不符"，只有专门断言契约才抓得住。
"""

import os

import pytest

import ayafileio
from ayafileio._async_file import AsyncFile


@pytest.fixture(autouse=True)
def _restore_config():
    """用例会改全局配置；结束后一律复位（CI 强制的后端由 conftest 再压回去）。"""
    yield
    ayafileio.reset_config()


# ════════════════════════════════════════════════════════════════════════════
# 1. 上下文管理器：flush 失败不得吞掉 close
# ════════════════════════════════════════════════════════════════════════════


async def test_aexit_closes_even_when_flush_fails(tmp_path):
    """auto_flush 的语义是"退出时先 flush 再 close"。

    早先是顺序执行，flush 一抛异常，close 就永远不执行 —— 进程同时开着很多文件时
    这不是"晚一点关"而是真的耗尽句柄。异常本身仍必须向外传播。
    """
    path = tmp_path / "flush_fail.bin"
    path.write_bytes(b"x" * 16)

    async def boom(self):
        raise OSError(5, "simulated flush failure")

    original = AsyncFile.flush
    AsyncFile.flush = boom
    handle = ayafileio.open(str(path), "wb", auto_flush=True)
    try:
        with pytest.raises(OSError):
            async with handle:
                await handle.write(b"data")
        assert handle.closed, "flush 失败后文件没有被关闭（句柄泄漏）"
    finally:
        AsyncFile.flush = original
        if not handle.closed:
            await handle.close()


async def test_aexit_closes_normally(tmp_path):
    """正常路径同样要保证关闭（对照组，防止把 finally 写反）。"""
    path = tmp_path / "ok.bin"
    handle = ayafileio.open(str(path), "wb", auto_flush=True)
    async with handle:
        await handle.write(b"payload")
    assert handle.closed
    assert path.read_bytes() == b"payload"


# ════════════════════════════════════════════════════════════════════════════
# 2. configure() 的输入校验
# ════════════════════════════════════════════════════════════════════════════


@pytest.mark.parametrize(
    "options",
    [
        {"buffer_pool_maxx": 1024},              # 多打一个字母
        {"io_worker_cout": 8},                   # 少打一个字母
        {"backend": "iocp", "bogus": 1},         # 与合法 key 混在一起
        {"Backend": "iocp"},                     # 大小写错
    ],
)
def test_configure_rejects_unknown_key(options):
    """未知 key 必须当场报错。

    静默忽略比报错危险得多：它让"配置没生效"变成一个需要用户自己去比对
    get_config() 才能发现的隐性故障。
    """
    with pytest.raises(ValueError, match="unknown config key"):
        ayafileio.configure(options)


def test_configure_accepts_every_key_it_reports():
    """get_config() 报出来的每个 key 都必须能被 configure() 接受。

    这条守的是"白名单 ↔ 实现"的一致性：新增配置项时若只加了 contains() 分支
    而忘了白名单，用户设置它会被当成未知 key 拒绝。
    """
    for key in ayafileio.get_config():
        try:
            ayafileio.configure({key: ayafileio.get_config()[key]})
        except ValueError as exc:
            pytest.fail(f"configure() 拒绝了自己报告过的 key {key!r}: {exc}")


def test_configure_validates_after_key_check():
    """未知 key 必须在任何状态被改动之前就被拒绝（不能半生效）。"""
    before = ayafileio.get_config()["buffer_pool_max"]
    with pytest.raises(ValueError):
        ayafileio.configure({"buffer_pool_max": 1024, "nope": True})
    assert ayafileio.get_config()["buffer_pool_max"] == before, "被拒的批次部分生效了"


@pytest.mark.parametrize(
    "options",
    [{"io_worker_count": "8"}, {"backend": 123}, {"close_timeout_ms": None}],
    ids=["str-for-int", "int-for-str", "none"],
)
def test_configure_wrong_type_raises_type_error(options):
    """类型不对必须是 TypeError，而不是 RuntimeError("bad cast")。

    nanobind 默认把 cast 失败翻成 std::runtime_error，于是 Python 侧拿到
    RuntimeError —— 用户写 ``except TypeError`` 兜不住。这两种错误对调用方的
    意义完全不同：类型错是编码 bug，值错是输入数据问题。
    """
    with pytest.raises(TypeError, match="wrong type"):
        ayafileio.configure(options)


@pytest.mark.parametrize(
    "options",
    [
        {"buffer_size": 0},
        {"buffer_size": -1},
        {"buffer_pool_max": -5},
        {"iocp_batch_size": 999},
        {"adaptive_target_latency_us": 0},
    ],
    ids=["size-zero", "size-neg", "pool-neg", "batch-too-big", "latency-zero"],
)
def test_configure_bad_value_raises_value_error(options):
    """类型对但值非法必须是 ValueError。"""
    with pytest.raises(ValueError):
        ayafileio.configure(options)


# ════════════════════════════════════════════════════════════════════════════
# 3. 1.x 遗留的 worker 数入口必须真的生效
# ════════════════════════════════════════════════════════════════════════════


@pytest.mark.parametrize("value", [0, 1, 4, 128])
def test_set_io_worker_count_takes_effect(value):
    ayafileio.reset_config()
    ayafileio.set_io_worker_count(value)
    assert ayafileio.get_config()["io_worker_count"] == value
    ayafileio.reset_config()


@pytest.mark.parametrize("value", [0, 2, 128])
def test_set_iocp_worker_count_takes_effect(value):
    if not hasattr(ayafileio, "set_iocp_worker_count"):
        pytest.skip("本平台没有 set_iocp_worker_count")
    ayafileio.reset_config()
    ayafileio.set_iocp_worker_count(value)
    assert ayafileio.get_config()["io_worker_count"] == value
    ayafileio.reset_config()


def test_worker_count_matches_configure():
    """两个入口必须与 configure({"io_worker_count": n}) 等价，不能各走一套。"""
    ayafileio.reset_config()
    ayafileio.configure({"io_worker_count": 7})
    via_configure = ayafileio.get_config()["io_worker_count"]
    ayafileio.reset_config()
    ayafileio.set_io_worker_count(7)
    via_shim = ayafileio.get_config()["io_worker_count"]
    ayafileio.reset_config()
    assert via_configure == via_shim == 7


@pytest.mark.parametrize("bad", [-1, 129, 999])
def test_worker_count_rejects_invalid(bad):
    with pytest.raises((ValueError, TypeError)):
        ayafileio.set_io_worker_count(bad)


# ════════════════════════════════════════════════════════════════════════════
# 4. 顺带守住的既有契约
# ════════════════════════════════════════════════════════════════════════════


async def test_every_entry_point_rejects_closed_file(tmp_path):
    """关闭后所有入口都抛 ValueError（tell 曾经是唯一漏网的那个）。"""
    path = tmp_path / "closed.bin"
    path.write_bytes(b"0123456789")

    handle = ayafileio.open(str(path), "rb")
    await handle.close()
    calls = {
        "read": lambda: handle.read(1),
        "read_at": lambda: handle.read_at(0, 1),
        "read_many": lambda: handle.read_many([(0, 1)]),
        "readinto": lambda: handle.readinto(bytearray(1)),
        "readline": lambda: handle.readline(),
        "readlines": lambda: handle.readlines(),
        "seek": lambda: handle.seek(0),
        "tell": lambda: handle.tell(),
        "flush": lambda: handle.flush(),
        "truncate": lambda: handle.truncate(0),
        "write_at": lambda: handle.write_at(0, b"x"),
        "write_many": lambda: handle.write_many([(0, b"x")]),
    }
    for name, call in calls.items():
        with pytest.raises(ValueError):
            await call()


async def test_repeated_open_close_keeps_position(tmp_path):
    path = tmp_path / "churn.bin"
    path.write_bytes(b"abcdefghij")
    for _ in range(50):
        async with ayafileio.open(str(path), "rb") as handle:
            assert await handle.tell() == 0
            assert await handle.read(4) == b"abcd"
