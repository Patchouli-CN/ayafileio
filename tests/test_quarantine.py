"""quarantine（IOCP 延迟释放队列）与 configure() 热更新的回归测试。

这两条都是「静默劣化」型 bug：功能全对、只是慢，所以只能靠计数器/状态断言来守。

* quarantine 一次 O(n) 的 drain 会让每个完成的 op 去扫整个队列（曾经 15k–75k 条），
  而队列深度 = 1 秒流量，于是吞吐越高越慢。摊销 O(1) 的不变量是
  ``scanned <= 每 op 常数``，这条断言不依赖机器快慢。
* ``configure()`` 曾对已存在的 batcher 静默无效（构造时缓存），
  所以只能读 ``debug_batcher_state()`` 才能证明值真的推进去了。
"""

import asyncio
import os
import sys
import tempfile
from pathlib import Path

import pytest

import ayafileio

WIN_ONLY = pytest.mark.skipif(
    sys.platform != "win32", reason="IOCP deferred-free quarantine is Windows-only"
)

BURST = 20_000
MESSAGE = "x" * 128 + "\n"


async def _burst(path: Path, count: int, workers: int = 50) -> None:
    async with ayafileio.open(str(path), "a", encoding="utf-8") as handle:
        async def worker(worker_index: int) -> None:
            for index in range(worker_index, count, workers):
                await handle.write(MESSAGE)

        await asyncio.gather(*(worker(i) for i in range(workers)))


@WIN_ONLY
async def test_quarantine_drain_is_amortised() -> None:
    """每个完成最多扫常数个条目——不是 O(队列深度)。"""
    stats = ayafileio._ayafileio.debug_quarantine_stats  # noqa: SLF001
    before = stats()

    with tempfile.TemporaryDirectory(prefix="quarantine_") as root:
        await _burst(Path(root) / "burst.log", BURST)

    after = stats()
    pushed = after["pushes"] - before["pushes"]
    scanned = after["scanned"] - before["scanned"]

    assert pushed >= BURST, f"only {pushed} completions recorded"
    # O(n) 的实现这里会是「队列深度 × 完成数」，量级 1e8 起。
    assert scanned <= 2 * pushed + 4096, (
        f"drain inspected {scanned} entries for {pushed} completions — "
        "quarantine drain is not amortised O(1)"
    )
    # 队列深度不可能超过「一秒内完成的 op 数」这个上界。
    assert after["live"] <= pushed + 1024, f"live={after['live']} pushes={pushed}"


@WIN_ONLY
async def test_quarantine_releases_after_window() -> None:
    """过了保留期再有一次 I/O，旧请求就会被真正释放（live 下降）。"""
    stats = ayafileio._ayafileio.debug_quarantine_stats  # noqa: SLF001
    with tempfile.TemporaryDirectory(prefix="quarantine_window_") as root:
        path = Path(root) / "window.log"
        await _burst(path, 2_000)
        held = stats()["live"]
        assert held > 0, "burst should leave requests quarantined"

        await asyncio.sleep(1.5)  # > QUARANTINE_MS
        await _burst(path, 1_000)
        assert stats()["live"] < held, "expired requests were not released"


async def test_configure_hot_updates_live_batcher() -> None:
    """configure() / reset_config() 必须作用到已经存在的 batcher。"""
    state = ayafileio._ayafileio.debug_batcher_state  # noqa: SLF001
    with tempfile.TemporaryDirectory(prefix="batcher_state_") as root:
        path = Path(root) / "state.txt"
        path.write_text("hello", encoding="utf-8")
        async with ayafileio.open(str(path), "rb") as handle:
            await handle.read(4)  # 让 batcher 先存在

        baseline = state()
        assert baseline, "debug_batcher_state() returned nothing"
        assert baseline["threshold_max"] == ayafileio.get_config()["iocp_batch_size"]

        try:
            ayafileio.configure(
                {
                    "iocp_batch_size": 8,
                    "adaptive_batch": False,
                    "adaptive_target_latency_us": 3000,
                }
            )
            live = state()
            assert live["threshold_max"] == 8, live
            assert live["threshold"] <= 8, live
            assert live["adaptive"] is False, live
            assert live["target_latency_us"] == 3000, live
        finally:
            ayafileio.reset_config()

    restored = state()
    assert restored["threshold_max"] == 64, restored
    assert restored["adaptive"] is True, restored
    assert restored["target_latency_us"] == 1000, restored
