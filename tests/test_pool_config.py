"""GlobalThreadPool 的 worker 数必须跟着 configure 走。

回归背景：`ensure_started()` 是 start-once（第一个调用者传的规模永久生效），
于是 `configure({"io_worker_count": N})` 在池已经启动之后是**静默无效**的：
`get_config()` 报 N，实际干活的还是老数量。实测（进程线程数采样）改动前配置从 2
改成 16，线程数全程钉在 9 不动。

现在 `configure()` / `reset_config()` 会把池热同步到 `effective_io_worker_count()`
（多就补线程、少就让多余 worker 在取下一个任务前自行退出）。
"""

import asyncio
import os
import time

import ayafileio


def _pool_workers() -> int:
    return ayafileio._ayafileio.debug_pool_workers()  # noqa: SLF001


def _auto_workers() -> int:
    hc = os.cpu_count() or 1
    return max(1, min(hc * 2, 16))


async def _wait_workers(expected: int, timeout: float = 5.0) -> bool:
    """worker 退出是异步的（取下一个任务前才自查），所以轮询等它收敛。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _pool_workers() == expected:
            return True
        await asyncio.sleep(0.01)
    return False


async def test_configure_resizes_live_pool(tmp_path):
    path = tmp_path / "pool.bin"
    path.write_bytes(b"x" * 4096)

    try:
        ayafileio.configure({"io_worker_count": 2})
        handle = await ayafileio.aopen(str(path), "rb")   # 触发池首次启动
        await handle.close()
        assert await _wait_workers(2), f"池应以 2 起，实际 {_pool_workers()}"

        ayafileio.configure({"io_worker_count": 8})
        assert await _wait_workers(8), f"扩容没生效，实际 {_pool_workers()}"

        ayafileio.configure({"io_worker_count": 3})
        assert await _wait_workers(3), f"缩容没生效，实际 {_pool_workers()}"
    finally:
        ayafileio.reset_config()
        await _wait_workers(_auto_workers())


async def test_reset_config_restores_auto_pool_size(tmp_path):
    path = tmp_path / "pool2.bin"
    path.write_bytes(b"y" * 64)

    try:
        ayafileio.configure({"io_worker_count": 4})
        handle = await ayafileio.aopen(str(path), "rb")
        await handle.close()
        assert await _wait_workers(4), f"池应以 4 起，实际 {_pool_workers()}"

        ayafileio.reset_config()
        expected = _auto_workers()
        assert await _wait_workers(expected), (
            f"reset_config 后应回到 auto={expected}，实际 {_pool_workers()}"
        )
    finally:
        ayafileio.reset_config()
