"""configure({"backend": ...}) 的测试。

这个开关存在的两个理由，正好就是这里要守的：

1. **让回退后端有 CI 覆盖**。`ThreadIOBackend` 是「io_uring 不可用」时的唯一
   路径，但正常 CI 里永远跑不到它（Linux 装 liburing 走 io_uring、macOS 走
   GCD、Windows 走 IOCP）。CI 通过 ``AYAFILEIO_TEST_BACKEND=thread_pool``
   让整套用例切过去（见 tests/conftest.py）。
2. **让「我到底在用哪个后端」可回答且诚实**。所以这里顺带验证
   ``get_backend_info()`` / ``get_capabilities()`` 跟着走，而不是永远报平台默认。

不可用的后端（比如 Windows 上的 ``thread_pool``——线程池后端是 POSIX 实现）
必须在 configure 时当场报错，而不是留到第一次 open。
"""

import os

import pytest

import ayafileio

ALL_BACKENDS = ("auto", "iocp", "io_uring", "dispatch_io", "thread_pool")
FIXED_BACKENDS = tuple(b for b in ALL_BACKENDS if b != "auto")


@pytest.fixture(autouse=True)
def _restore_backend():
    """每个用例结束后把后端恢复原状（CI 可能已经把整体切到 thread_pool）。"""
    before = ayafileio.get_config()["backend"]
    yield
    ayafileio.configure({"backend": before})


def _is_available(name: str) -> bool:
    """本构建/本机器上能否强制选中该后端（临时改配置，由 fixture 复原）。"""
    before = ayafileio.get_config()["backend"]
    try:
        ayafileio.configure({"backend": name})
        return ayafileio.get_config()["backend"] == name
    except ValueError:
        return False
    finally:
        ayafileio.configure({"backend": before})


def _available_fixed():
    return [name for name in FIXED_BACKENDS if _is_available(name)]


# ════════════════════════════════════════════════════════════════════════════
# 配置面
# ════════════════════════════════════════════════════════════════════════════


def test_backend_key_is_exposed():
    cfg = ayafileio.get_config()
    assert "backend" in cfg, "get_config() 必须暴露 backend"
    assert cfg["backend"] in ALL_BACKENDS


def test_unknown_backend_is_rejected():
    for bogus in ("epoll", "", "IOCP", "iocp ", "threadpool"):
        with pytest.raises(ValueError):
            ayafileio.configure({"backend": bogus})


def test_unavailable_backend_is_rejected():
    """本平台没有的后端必须当场拒绝，而不是留到 open。"""
    available = set(_available_fixed())
    unavailable = [n for n in FIXED_BACKENDS if n not in available]
    if not unavailable:
        pytest.skip("本平台所有后端都可用，构造不出「不可用」的场景")

    for name in unavailable:
        with pytest.raises(ValueError, match="not available"):
            ayafileio.configure({"backend": name})


def test_reset_restores_auto():
    for name in _available_fixed():
        ayafileio.configure({"backend": name})
        ayafileio.reset_config()
        assert ayafileio.get_config()["backend"] == "auto"


def test_env_forced_backend_is_active():
    """CI 的 ``AYAFILEIO_TEST_BACKEND`` 必须真的生效。

    没有这一条，conftest 里的切后端一旦静默失效，整轮「回退后端」CI 就会在
    默认后端上跑绿——比不跑更糟。
    """
    forced = os.environ.get("AYAFILEIO_TEST_BACKEND", "").strip()
    if not forced:
        pytest.skip("没有设置 AYAFILEIO_TEST_BACKEND")

    assert ayafileio.get_config()["backend"] == forced
    assert ayafileio.get_backend_info()["backend"] == forced


# ════════════════════════════════════════════════════════════════════════════
# 生效性与如实上报
# ════════════════════════════════════════════════════════════════════════════


def test_backend_info_reflects_choice():
    for name in _available_fixed():
        ayafileio.configure({"backend": name})
        info = ayafileio.get_backend_info()
        assert info["backend"] == name, f"backend={name} 但 get_backend_info 报 {info['backend']}"
        assert "is_truly_async" not in info


def test_capabilities_follow_choice():
    """能力矩阵必须跟着有效后端走，否则上报会与实际配置不一致。"""
    if "thread_pool" not in _available_fixed():
        pytest.skip("本平台没有线程池后端")

    ayafileio.configure({"backend": "thread_pool"})
    caps = ayafileio.get_capabilities()
    assert caps["backend"] == "thread_pool"
    assert "is_truly_async" not in caps
    assert caps["backend_detail"]["completion_model"] is False


async def test_forced_backend_serves_io(tmp_path):
    """强制后端之后，打开 / 读 / 写 / 位置读写照常工作。"""
    path = tmp_path / "forced.bin"
    path.write_bytes(b"hello world")

    for name in _available_fixed():
        ayafileio.configure({"backend": name})

        async with ayafileio.open(str(path), "rb") as handle:
            assert await handle.read() == b"hello world", f"backend={name} 读失败"
            assert await handle.read_at(6, 5) == b"world"

        out = tmp_path / f"out_{name}.bin"
        async with ayafileio.open(str(out), "wb") as handle:
            assert await handle.write(b"abcdef") == 6
            assert await handle.write_at(2, b"XY") == 2
        assert out.read_bytes() == b"abXYef"


async def test_switching_backend_affects_new_handles_only(tmp_path):
    """已经打开的句柄不受后续 configure 影响（只影响之后 new 出来的）。"""
    path = tmp_path / "switch.bin"
    path.write_bytes(b"0123456789")
    available = _available_fixed()
    if len(available) < 2:
        pytest.skip("本平台可用的后端不足两个，无法构造切换场景")

    async with ayafileio.open(str(path), "rb") as handle:
        first = await handle.read(4)
        ayafileio.configure({"backend": available[-1]})
        second = await handle.read(4)
    assert (first, second) == (b"0123", b"4567")
