"""原生资源记账的回归测试：fileno 的所有权、事件循环引用、文件大小缓存。

这三条都属于「功能看着全对、只在长时间运行或特定调用顺序下才炸」的问题，
所以只能靠**可观测的资源不变量**来守，而不是靠返回值断言：

* ``fileno()`` 曾经每次调用都对会话自己的 HANDLE 做 ``_open_osfhandle`` ——
  每调一次泄漏一个 CRT 描述符；而且调用方 ``os.close(f.fileno())`` 会把文件
  对象的句柄一起关掉，之后所有 I/O 全部 EBADF。现在暴露的是**复制**出来的
  句柄，并且只创建一次（同一个对象返回稳定的 fd，对齐 ``io.FileIO.fileno()``）。
* 每次 ``open()`` 都白送事件循环对象一个引用且从不归还，``loop.close()`` 之后
  那个对象依然回收不了。``aopen()`` 那条路径还多漏一个。
* 文件大小在 open 时快照，别的句柄追加的数据 ``read()`` 看不见。现在到 EOF
  判决点（或发生短读）时会问一次内核，热路径 syscall 数不变。
"""

import asyncio
import gc
import os
import subprocess
import sys
import textwrap

import pytest

import ayafileio

# fileno 的「复制句柄」语义是 Windows IOCP 专属：POSIX 后端直接把真实 fd
# 交给调用方（与标准库一致，关掉它就等于关掉文件），没有可复制的东西。
WIN_ONLY = pytest.mark.skipif(
    sys.platform != "win32", reason="fileno() 复制句柄只存在于 Windows IOCP 后端"
)


# ════════════════════════════════════════════════════════════════════════════
# fileno()：稳定 + 句柄独立
# ════════════════════════════════════════════════════════════════════════════


@WIN_ONLY
async def test_fileno_is_stable_and_not_leaked(tmp_path):
    """反复调用 fileno() 返回同一个 fd（不再每次新建一个 CRT 描述符）。"""
    path = tmp_path / "stable.bin"
    path.write_bytes(b"x" * 64)

    async with ayafileio.open(str(path), "rb") as handle:
        first = handle.fileno()
        assert first >= 0
        for _ in range(200):
            assert handle.fileno() == first, "fileno() 每次都应返回同一个描述符"

        # 描述符必须真的指向这个文件（而不是别处复用来的槽位）
        assert os.fstat(first).st_size == 64


@WIN_ONLY
async def test_fileno_survives_callers_close(tmp_path):
    """调用方 close 掉 fileno() 的返回值，不该把文件对象一起弄死。

    这是修复前最直接的伤害：那个描述符拥有的是会话自己的 HANDLE，
    ``os.close`` 之后 ``await f.read()`` 立刻 EBADF。现在两者互相独立。
    """
    path = tmp_path / "independent.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        fd = handle.fileno()
        os.close(fd)  # 标准库里属于未定义用法，但至少不能连累文件对象

        assert await handle.read(4) == b"0123"
        await handle.seek(0)
        assert await handle.read() == b"0123456789"

    # 关闭的对象走到这里没有崩，且 close() 正常返回即为通过
    assert handle.closed


@WIN_ONLY
def test_fileno_close_does_not_touch_reused_fd(tmp_path):
    """调用方关掉描述符后，它的号码被别的描述符复用 —— 关闭文件不能误伤。

    修复过程中的一次尝试只判断「fd 还有效吗」，结果把复用了同一号码的
    asyncio self-pipe socket 当成自己的关掉了，进程在事件循环收尾时 fastfail。
    现在比对的是**句柄身份**，号码被复用也只会跳过。

    用子进程跑：真要回归的话表现是整进程 0xC0000409，而不是异常。
    """
    script = textwrap.dedent(
        f"""
        import asyncio, os, ayafileio

        keep = []   # 全程活着，保证被抢占的号码一直没被释放

        async def job():
            async with ayafileio.open({str(tmp_path / "guard.bin")!r}, "rb") as f:
                fd = f.fileno()
                os.close(fd)
                decoy = open({str(tmp_path / "decoy.bin")!r}, "wb")
                if decoy.fileno() != fd:
                    # 没抢到同一个号码 → 这个场景没法构造，交给调用方跳过
                    return "skip"
                keep.append(decoy)
            return "ok"

        print(asyncio.run(job()))
        """
    )
    (tmp_path / "guard.bin").write_bytes(b"x" * 32)
    (tmp_path / "decoy.bin").write_bytes(b"")

    result = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, timeout=120
    )
    assert result.returncode == 0, f"子进程异常退出 {result.returncode}: {result.stderr}"
    assert "ok" in result.stdout or "skip" in result.stdout
    if "skip" in result.stdout:
        pytest.skip("该环境下无法把同一个 fd 号分配回来，构造不出「号码被复用」")


# ════════════════════════════════════════════════════════════════════════════
# 事件循环引用
# ════════════════════════════════════════════════════════════════════════════


# 下面这条守的是「每次 open 泄漏一个引用」；再下面那条守的是**按 loop** 的
# 滞留（ResultBatcher 曾经缓存 `loop.call_soon_threadsafe` 这个绑定方法，
# 把任何做过一次 I/O 的 loop 钉到进程退出）。两者是独立的泄漏源。


@pytest.mark.parametrize("opener", ["open", "aopen"])
async def test_open_does_not_grow_loop_refcount(tmp_path, opener):
    """200 次 open/close 之后，当前 loop 的引用计数不应增长。

    ``aopen()`` 单独覆盖：它走 ``open_async`` 的工作线程回投路径，那边曾经
    也把 loop 引用当成「Session 的续命来源」留着不放，比同步路径多漏一个。

    只 open/close、不做 I/O，避开 quarantine 的合法滞留。
    """
    path = tmp_path / "refcount.bin"
    path.write_bytes(b"x" * 64)

    loop = asyncio.get_running_loop()
    gc.collect()
    before = sys.getrefcount(loop)

    for _ in range(200):
        async with getattr(ayafileio, opener)(str(path), "rb"):
            pass

    gc.collect()
    grew = sys.getrefcount(loop) - before
    # 20 是很宽松的上界：真回归是 200（每次一个）。留这么大是为了容忍自由
    # 线程构建上 biased refcount 的抖动（CI 矩阵里有 3.14t / 3.15t）。
    assert grew <= 20, f"200 次 {opener} 让 loop 引用计数增长了 {grew}"


def test_event_loop_is_released_after_open(tmp_path):
    """做过 I/O 的事件循环，``close()`` + ``del`` 之后必须能被回收。

    `ResultBatcher` 按 loop 缓存，构造时缓存了 ``loop.call_soon_threadsafe``
    这个**绑定方法**——那是个强引用，于是任何跑过一次 I/O 的 loop 都会被
    ``IOCPContext::m_batchers`` 持有到进程退出。``asyncio.run()`` 包在每个请求
    里的代码（gunicorn 同步栈里很常见）就会每请求留下一个 loop 对象。
    现在 batcher 只持弱引用，并在 flush 时按弱引用取回 loop。

    用子进程跑：pytest 的 frame / 断言改写会额外持有 loop，对照组都过不了。
    刻意不做 I/O：quarantine 会合法地暂扣 future（那是设计内的延迟释放）。
    """
    path = tmp_path / "loop.bin"
    path.write_bytes(b"x" * 64)

    script = textwrap.dedent(
        f"""
        import asyncio, gc, weakref
        import ayafileio

        PATH = {str(path)!r}

        def collected(*, use_open, use_aopen):
            dead = []
            loop = asyncio.new_event_loop()
            # weakref 对象本身要留在局部变量里，否则它先被回收、回调永不触发
            watch = weakref.ref(loop, lambda _: dead.append(True))

            async def job():
                if use_open:
                    async with ayafileio.open(PATH, "rb"):
                        pass
                if use_aopen:
                    async with ayafileio.aopen(PATH, "rb"):
                        pass

            try:
                loop.run_until_complete(job())
                loop.close()
            finally:
                asyncio.set_event_loop(None)
            del loop
            gc.collect()
            assert watch() is None or dead, "weakref 状态异常"
            return bool(dead)

        print(
            collected(use_open=False, use_aopen=False),
            collected(use_open=True,  use_aopen=False),
            collected(use_open=False, use_aopen=True),
        )
        """
    )

    result = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, timeout=180
    )
    assert result.returncode == 0, f"子进程异常退出: {result.stderr}"

    control, plain, async_open = result.stdout.split()
    assert control == "True", "对照组失效：不碰 ayafileio 时 loop 也回收不了"
    assert plain == "True", "open() 之后事件循环对象回收不了（batcher 强引用？）"
    assert async_open == "True", "aopen() 之后事件循环对象回收不了（batcher 强引用？）"


# ════════════════════════════════════════════════════════════════════════════
# 文件大小缓存
# ════════════════════════════════════════════════════════════════════════════

# 大小缓存（open 时快照）目前只在 Windows IOCP 后端做了「到 EOF 判决点就
# 问内核」的刷新。macOS 的 dispatch_io 后端与 POSIX 线程池后端有同一套缓存，
# 尚未跟进——那两个后端的修复留待能在对应平台上验证时再做，这里先不假装
# 跨平台通过。
WIN_ONLY_SIZE_CACHE = WIN_ONLY


@WIN_ONLY_SIZE_CACHE
async def test_read_sees_external_append(tmp_path):
    """别的句柄追加数据后，read_at / seek 必须看到新长度。

    修复前 read_at 直接用 open 时缓存的长度做 EOF 截断，越界的 offset 永远
    返回 b""，对着一个正在被写的文件读就是静默少读。
    """
    path = tmp_path / "grow.bin"
    path.write_bytes(b"0123456789")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.read() == b"0123456789"

        with open(path, "ab") as producer:
            producer.write(b"APPENDED")

        assert await handle.read_at(10, -1) == b"APPENDED"
        assert await handle.read_at(0, -1) == b"0123456789APPENDED"
        assert await handle.seek(0, 2) == 18   # 10 + len("APPENDED")


@WIN_ONLY_SIZE_CACHE
async def test_read_continues_after_external_append(tmp_path):
    """顺序读到 EOF 之后再来一次 read()，应该看到外部追加的内容。"""
    path = tmp_path / "tail.bin"
    path.write_bytes(b"first\n")

    async with ayafileio.open(str(path), "rb") as handle:
        assert await handle.read() == b"first\n"
        with open(path, "ab") as producer:
            producer.write(b"second\n")
        assert await handle.read() == b"second\n"
        assert await handle.read() == b""


@WIN_ONLY_SIZE_CACHE
async def test_truncated_file_does_not_return_stale_garbage(tmp_path):
    """文件被外部截短后，read() 不能返回超出新长度的内容，且长度要收敛。"""
    path = tmp_path / "shrink.bin"
    path.write_bytes(b"A" * 64)

    async with ayafileio.open(str(path), "rb") as handle:
        assert len(await handle.read(16)) == 16
        os.truncate(path, 20)  # 截到 20 字节

        rest = await handle.read()  # 从 16 读到真 EOF
        assert rest == b"A" * 4
        await handle.seek(0, 2)
        assert await handle.tell() == 20


@WIN_ONLY_SIZE_CACHE
async def test_readinto_sees_external_append(tmp_path):
    """readinto 走同一条缓存长度判决，短读后必须刷新。"""
    path = tmp_path / "into.bin"
    path.write_bytes(b"abc")

    async with ayafileio.open(str(path), "rb") as handle:
        buf = bytearray(8)
        assert await handle.readinto(buf) == 3

        with open(path, "ab") as producer:
            producer.write(b"defgh")

        await handle.seek(0, 2)
        assert await handle.tell() == 8
        await handle.seek(3)
        assert await handle.readinto(buf) == 5
        assert bytes(buf[:5]) == b"defgh"


# ════════════════════════════════════════════════════════════════════════════
# quarantine：空闲时也要排空
# ════════════════════════════════════════════════════════════════════════════


def _quarantine() -> dict:
    return ayafileio._ayafileio.debug_quarantine_stats()  # noqa: SLF001


@WIN_ONLY
async def test_quarantine_drains_when_idle(tmp_path):
    """保留期满了、而且没有新流量时，也得把滞留的请求放掉。

    此前 quarantine 只在两种时机 drain：本线程 push 到 256 条时，以及 worker
    收到一批完成包之后。于是**最后一批**完成的请求会永远挂在队里——每个
    IORequest 还牵着它的 future（以及 future 里的 loop 引用），内存停在上一次
    峰值上不回落。现在 worker 的等待超时把队首到期时间算进去了，空闲也会醒。
    """
    path = tmp_path / "idle.bin"
    path.write_bytes(b"x" * 64)

    before = _quarantine()["live"]

    async with ayafileio.open(str(path), "rb") as handle:
        for _ in range(300):
            await handle.seek(0)
            await handle.read(4)

    during = _quarantine()["live"]
    if during <= before:
        pytest.skip("本次 burst 没有走 IOCP 完成包路径，quarantine 里没有东西可测")

    await asyncio.sleep(1.6)  # QUARANTINE_MS = 1000
    after = _quarantine()["live"]
    assert after - before <= 8, (
        f"空闲 1.6s 后仍有 {after - before} 个请求滞留："
        "quarantine 只在有流量时排空"
    )
