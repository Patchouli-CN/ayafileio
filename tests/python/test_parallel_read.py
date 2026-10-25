"""大读并行分块填充 + 整文件操作的正确性测试。

在 macOS 上，>= 1 MiB 的读走 submit_read_parallel（多 worker 并发
pread 进同一缓冲）；其它平台走各自的快速路。本测试在所有平台上
验证数据正确性与整文件 API 的 round-trip 语义。
"""

import asyncio
import os
import tempfile
import unittest

import ayafileio


def _make_pattern(size: int) -> bytes:
    """确定性伪随机内容（避免全零/压缩文件系统的巧合命中）。"""
    out = bytearray(size)
    x = 0x12345678
    for i in range(size):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


PARALLEL_SIZE = 3 * 1024 * 1024 + 12345  # > 1 MiB 阈值，非分块整数倍（短尾块）


class TestParallelRead(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="aya_par_")
        cls.path = os.path.join(cls.dir, "big.bin")
        cls.data = _make_pattern(PARALLEL_SIZE)
        with open(cls.path, "wb") as f:
            f.write(cls.data)

    @classmethod
    def tearDownClass(cls):
        try:
            os.unlink(cls.path)
            os.rmdir(cls.dir)
        except OSError:
            pass

    def test_read_whole(self):
        """整文件 read() 走并行路径，内容必须逐字节一致"""
        async def run():
            async with ayafileio.open(self.path, "rb") as f:
                return await f.read()
        got = asyncio.run(run())
        self.assertEqual(got, self.data)

    def test_read_slice(self):
        """read(n) 小于阈值；并发 gather 多个大 read_at 交叉验证"""
        async def run():
            spans = [(0, 1024 * 1024), (1024 * 1024, 1024 * 1024),
                     (2 * 1024 * 1024, PARALLEL_SIZE - 2 * 1024 * 1024)]
            async with ayafileio.open(self.path, "rb") as f:
                return await asyncio.gather(
                    *[f.read_at(off, n) for off, n in spans])
        chunks = asyncio.run(run())
        self.assertEqual(chunks[0], self.data[:1024 * 1024])
        self.assertEqual(chunks[1], self.data[1024 * 1024:2 * 1024 * 1024])
        self.assertEqual(chunks[2], self.data[2 * 1024 * 1024:])

    def test_readinto_large(self):
        """readinto 大缓冲同样验证"""
        async def run():
            buf = bytearray(PARALLEL_SIZE)
            async with ayafileio.open(self.path, "rb") as f:
                n = await f.readinto(buf)
            return n, bytes(buf)
        n, buf = asyncio.run(run())
        self.assertEqual(n, PARALLEL_SIZE)
        self.assertEqual(buf, self.data)


class TestWholeFileAPI(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="aya_whole_")

    def tearDown(self):
        for name in os.listdir(self.dir):
            os.unlink(os.path.join(self.dir, name))
        os.rmdir(self.dir)

    def test_read_write_bytes_roundtrip(self):
        path = os.path.join(self.dir, "x.bin")
        payload = os.urandom(2 * 1024 * 1024 + 7)
        async def run():
            n = await ayafileio.write_bytes(path, payload)
            back = await ayafileio.read_bytes(path)
            return n, back
        n, back = asyncio.run(run())
        self.assertEqual(n, len(payload))
        self.assertEqual(back, payload)

    def test_read_write_text_roundtrip(self):
        path = os.path.join(self.dir, "y.txt")
        text = "幽々子\n测试 utf-8 往返\n" * 1000
        async def run():
            await ayafileio.write_text(path, text, encoding="utf-8")
            return await ayafileio.read_text(path, encoding="utf-8")
        self.assertEqual(asyncio.run(run()), text)

    def test_gather_many_files(self):
        """asyncio.gather 批量整文件读（read_bytes 的目标形态）"""
        paths = []
        expect = []
        for i in range(8):
            p = os.path.join(self.dir, f"f{i}.bin")
            payload = _make_pattern(64 * 1024 + i)
            with open(p, "wb") as f:
                f.write(payload)
            paths.append(p)
            expect.append(payload)
        async def run():
            return await asyncio.gather(*[ayafileio.read_bytes(p) for p in paths])
        got = asyncio.run(run())
        self.assertEqual(got, expect)


if __name__ == "__main__":
    unittest.main(verbosity=2)
