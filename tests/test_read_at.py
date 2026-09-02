"""read_at / read_many 位置读测试（脚本式，直接 python tests/test_read_at.py 运行）"""

import asyncio
import os
import random
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

# 添加项目根目录到路径
sys.path.insert(0, str(Path(__file__).parent))

if sys.platform == "win32":
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import ayafileio


# ════════════════════════════════════════════════════════════════════════════
# 简易 watchdog — 单个测试卡住时打印堆栈并退出
# ════════════════════════════════════════════════════════════════════════════

_TEST_TIMEOUT = 30
_current_test = {"name": None, "start": 0.0}


def _watchdog():
    while True:
        time.sleep(1)
        name, start = _current_test["name"], _current_test["start"]
        if name and time.time() - start > _TEST_TIMEOUT:
            print(f"\n⚠️  TIMEOUT: '{name}' running for {time.time() - start:.1f}s")
            for tid, frame in sys._current_frames().items():
                print(f"\n[Thread {tid}]")
                traceback.print_stack(frame)
            os._exit(1)


threading.Thread(target=_watchdog, daemon=True).start()


# ════════════════════════════════════════════════════════════════════════════
# 测试运行器
# ════════════════════════════════════════════════════════════════════════════

passed = 0
failed = 0
failures = []


def run_async(name, coro_func):
    global passed, failed
    _current_test["name"] = name
    _current_test["start"] = time.time()
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(coro_func())
        passed += 1
        print(f"  ✅ {name}")
    except AssertionError as e:
        failed += 1
        failures.append((name, str(e)))
        print(f"  ❌ {name}: {e}")
    except Exception as e:
        failed += 1
        failures.append((name, f"{type(e).__name__}: {e}"))
        print(f"  💥 {name}: {type(e).__name__}: {e}")
    finally:
        try:
            pending = asyncio.all_tasks(loop)
            if pending:
                loop.run_until_complete(
                    asyncio.gather(*pending, return_exceptions=True)
                )
        except Exception:
            pass
        loop.close()
        asyncio.set_event_loop(None)
        _current_test["name"] = None


def get_temp_path(suffix: str = "") -> Path:
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as f:
        return Path(f.name)


def make_content(n: int = 65536) -> bytes:
    """确定性伪随机内容，便于逐字节对照。"""
    rng = random.Random(42)
    return bytes(rng.randrange(256) for _ in range(n))


# ════════════════════════════════════════════════════════════════════════════
# 测试用例
# ════════════════════════════════════════════════════════════════════════════


async def test_read_at_basic():
    """基本位置读"""
    content = make_content()
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            data = await f.read_at(100, 50)
            assert data == content[100:150], "read_at(100, 50) 内容不符"
            data = await f.read_at(0, 16)
            assert data == content[:16], "read_at(0, 16) 内容不符"
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_does_not_move_position():
    """read_at 不改变 tell() 与后续 read() 的位置"""
    content = make_content()
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            await f.seek(1000)
            pos_before = await f.tell()
            await f.read_at(5000, 128)
            pos_after = await f.tell()
            assert pos_before == pos_after == 1000, (
                f"tell() 被 read_at 改变: {pos_before} -> {pos_after}"
            )
            # 后续 read() 仍从逻辑位置继续
            data = await f.read(64)
            assert data == content[1000:1064], "read_at 后 read() 位置错乱"
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_eof_and_beyond():
    """EOF 截断与 offset 越界"""
    content = make_content(1000)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            # 读到 EOF 截断为短数据
            data = await f.read_at(990, 100)
            assert data == content[990:], f"EOF 截断错误: len={len(data)}"
            # offset == 文件大小 → b""
            assert await f.read_at(1000, 10) == b"", "offset == size 应返回空"
            # offset > 文件大小 → b""
            assert await f.read_at(99999, 10) == b"", "offset 越界应返回空"
            # size = 0 → b""
            assert await f.read_at(0, 0) == b"", "size=0 应返回空"
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_size_minus_one():
    """size=-1 从 offset 读到 EOF"""
    content = make_content(4096)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            data = await f.read_at(100)
            assert data == content[100:], "size=-1 应读到 EOF"
            data = await f.read_at(0)
            assert data == content, "size=-1 offset=0 应读全文件"
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_negative_offset():
    """负 offset 抛 ValueError"""
    content = make_content(256)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            try:
                await f.read_at(-1, 10)
                assert False, "负 offset 应抛 ValueError"
            except ValueError:
                pass
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_equivalent_to_seek_read():
    """read_at 与 seek+read 结果等价"""
    content = make_content()
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            rng = random.Random(7)
            for _ in range(50):
                off = rng.randrange(0, len(content) + 100)
                size = rng.choice([-1, 1, 63, 4096, len(content)])
                got = await f.read_at(off, size)
                end = None if size < 0 else off + size
                expect = content[off:end] if off < len(content) else b""
                assert got == expect, (
                    f"read_at({off}, {size}) 与切片不符: "
                    f"len(got)={len(got)} len(expect)={len(expect)}"
                )
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_concurrent_shared_handle():
    """共享句柄 100 个并发 read_at 随机偏移对照文件内容"""
    content = make_content(128 * 1024)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            rng = random.Random(1234)
            spans = [(rng.randrange(0, len(content)), rng.randrange(1, 4096))
                     for _ in range(100)]
            results = await asyncio.gather(*(f.read_at(o, s) for o, s in spans))
            for i, ((off, size), got) in enumerate(zip(spans, results)):
                expect = content[off:off + size]
                assert got == expect, f"并发第 {i} 个 read_at({off}, {size}) 内容不符"
            # 并发位置读后逻辑位置仍未被污染
            assert await f.tell() == 0, "并发 read_at 后 tell() 应为 0"
    finally:
        path.unlink(missing_ok=True)


async def test_read_many_order_and_content():
    """read_many 保序且内容正确"""
    content = make_content()
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            # 故意乱序 + 混合 size=-1 / 越界
            spans = [(5000, 100), (0, 32), (60000, -1), (len(content) + 10, 5), (123, 1)]
            results = await f.read_many(spans)
            assert isinstance(results, list) and len(results) == len(spans), "结果数量不符"
            assert results[0] == content[5000:5100]
            assert results[1] == content[0:32]
            assert results[2] == content[60000:]
            assert results[3] == b""
            assert results[4] == content[123:124]
            # read_many 同样不动逻辑位置
            assert await f.tell() == 0, "read_many 后 tell() 应为 0"
    finally:
        path.unlink(missing_ok=True)


async def test_read_many_empty_spans():
    """read_many 空列表"""
    content = make_content(64)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        async with ayafileio.open(path, "rb") as f:
            assert await f.read_many([]) == [], "空 spans 应返回空列表"
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_text_mode_raises():
    """文本模式下 read_at / read_many 抛 ValueError"""
    path = get_temp_path(".txt")
    try:
        path.write_text("hello world\n" * 100, encoding="utf-8")
        async with ayafileio.open(path, "r", encoding="utf-8") as f:
            try:
                await f.read_at(0, 10)
                assert False, "文本模式 read_at 应抛 ValueError"
            except ValueError:
                pass
            try:
                await f.read_many([(0, 10)])
                assert False, "文本模式 read_many 应抛 ValueError"
            except ValueError:
                pass
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_closed_file():
    """已关闭文件 read_at 抛 ValueError"""
    content = make_content(64)
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)
        f = ayafileio.open(path, "rb")
        await f.close()
        try:
            await f.read_at(0, 10)
            assert False, "已关闭文件 read_at 应抛 ValueError"
        except ValueError:
            pass
    finally:
        path.unlink(missing_ok=True)


async def test_read_at_mixed_with_concurrent_reads():
    """read_at 与并发顺序 read() 互不干扰"""
    content = make_content()
    path = get_temp_path(".bin")
    try:
        path.write_bytes(content)

        async with ayafileio.open(path, "rb") as f:
            # 一个协程顺序读，另一个协程并发 read_at；两者应各自拿到正确数据
            seq_result = []

            async def sequential_reader():
                while True:
                    chunk = await f.read(8192)
                    if not chunk:
                        break
                    seq_result.append(chunk)

            async def random_reader():
                rng = random.Random(99)
                out = []
                for _ in range(30):
                    off = rng.randrange(0, len(content))
                    size = rng.randrange(1, 2048)
                    out.append((off, size, await f.read_at(off, size)))
                return out

            _, rand_out = await asyncio.gather(sequential_reader(), random_reader())

            assert b"".join(seq_result) == content, "并发 read_at 干扰了顺序 read"
            for off, size, got in rand_out:
                assert got == content[off:off + size], f"read_at({off}, {size}) 内容不符"
    finally:
        path.unlink(missing_ok=True)


# ════════════════════════════════════════════════════════════════════════════
# 主函数
# ════════════════════════════════════════════════════════════════════════════


def main():
    print("=" * 60)
    print("ayafileio read_at / read_many 测试")
    print("=" * 60)
    print(f"\nPython: {sys.version}")
    print(f"Platform: {sys.platform}")
    info = ayafileio.get_backend_info()
    print(f"Backend: {info['backend']} (truly_async: {info['is_truly_async']})")

    start = time.time()

    print("\n📋 read_at 基本语义:")
    run_async("基本位置读", test_read_at_basic)
    run_async("read_at 不改变文件位置", test_read_at_does_not_move_position)
    run_async("EOF 截断与 offset 越界", test_read_at_eof_and_beyond)
    run_async("size=-1 读到 EOF", test_read_at_size_minus_one)
    run_async("负 offset 抛 ValueError", test_read_at_negative_offset)
    run_async("与 seek+read 结果等价", test_read_at_equivalent_to_seek_read)
    run_async("已关闭文件抛 ValueError", test_read_at_closed_file)

    print("\n📋 并发测试:")
    run_async("共享句柄 100 并发 read_at", test_read_at_concurrent_shared_handle)
    run_async("read_at 与并发顺序 read 互不干扰", test_read_at_mixed_with_concurrent_reads)

    print("\n📋 read_many 测试:")
    run_async("read_many 保序与正确性", test_read_many_order_and_content)
    run_async("read_many 空列表", test_read_many_empty_spans)

    print("\n📋 模式限制:")
    run_async("文本模式 read_at/read_many 抛错", test_read_at_text_mode_raises)

    print("\n" + "=" * 60)
    total = passed + failed
    print(f"测试完成: {total} 个测试, {passed} 通过, {failed} 失败")
    print(f"耗时: {time.time() - start:.2f}s")
    if failures:
        print("\n失败的测试:")
        for name, error in failures:
            print(f"  - {name}: {error}")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
