"""aopen 异步打开 + 批量整文件读取的正确性测试。

aopen 把 OS open 下沉到 C++ 线程池（工作线程持 GIL 构造，构造内部的
阻塞系统调用经 GilRelease 释放 GIL，多 worker 真正并行），结果经
ResultBatcher 投递。read_bytes_many/read_text_many 在此基础上提供
gather 批量形态。本测试在所有平台上验证两条路径的正确性。
"""

import asyncio
import os
import tempfile
import unittest

import ayafileio


class TestAsyncOpen(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="aya_aopen_")
        cls.files = []
        for i in range(32):
            p = os.path.join(cls.dir, f"f{i:02d}.bin")
            with open(p, "wb") as fp:
                fp.write(f"content-{i}-".encode() * (i + 1))
            cls.files.append(p)

    @classmethod
    def tearDownClass(cls):
        for p in cls.files:
            try:
                os.unlink(p)
            except OSError:
                pass
        try:
            os.rmdir(cls.dir)
        except OSError:
            pass

    def _expected(self, p):
        with open(p, "rb") as fp:
            return fp.read()

    def test_aopen_read_binary(self):
        """aopen 二进制读：内容与同步 open 一致"""
        async def run():
            async with await ayafileio.aopen(self.files[3], "rb") as f:
                return await f.read()
        self.assertEqual(asyncio.run(run()), self._expected(self.files[3]))

    def test_aopen_lazy_context_manager(self):
        """惰性句柄：async with 免 await（aiofiles 同款用法）"""
        async def run():
            async with ayafileio.aopen(self.files[5], "rb") as f:
                return await f.read()
        self.assertEqual(asyncio.run(run()), self._expected(self.files[5]))

    def test_aopen_lazy_await(self):
        """惰性句柄：显式 await 拿到 AsyncFile"""
        async def run():
            f = await ayafileio.aopen(self.files[6], "rb")
            try:
                return await f.read()
            finally:
                await f.close()
        self.assertEqual(asyncio.run(run()), self._expected(self.files[6]))

    def test_aopen_lazy_exception_propagates(self):
        """惰性句柄：__aenter__ 阶段的打开失败照常抛出"""
        async def run():
            async with ayafileio.aopen(os.path.join(self.dir, "nope2.bin"), "rb"):
                pass
        with self.assertRaises(FileNotFoundError):
            asyncio.run(run())

    def test_aopen_text_mode(self):
        """aopen 文本模式（"r"）：模式清洗与解码正确"""
        p = os.path.join(self.dir, "text.txt")
        with open(p, "w", encoding="utf-8") as fp:
            fp.write("你好，ayafileio")
        try:
            async def run():
                async with await ayafileio.aopen(p, "r", encoding="utf-8") as f:
                    return await f.read()
            self.assertEqual(asyncio.run(run()), "你好，ayafileio")
        finally:
            os.unlink(p)

    def test_aopen_write(self):
        """aopen 写模式：round-trip 一致"""
        p = os.path.join(self.dir, "w.bin")
        async def run():
            async with await ayafileio.aopen(p, "wb") as f:
                await f.write(b"async-open-write")
            async with await ayafileio.aopen(p, "rb") as f:
                return await f.read()
        try:
            self.assertEqual(asyncio.run(run()), b"async-open-write")
        finally:
            os.unlink(p)

    def test_aopen_not_found(self):
        """aopen 打开不存在的文件：FileNotFoundError 经 future 投递"""
        async def run():
            await ayafileio.aopen(os.path.join(self.dir, "nope.bin"), "rb")
        with self.assertRaises(FileNotFoundError):
            asyncio.run(run())

    def test_aopen_invalid_mode(self):
        """模式校验与 open() 一致：非法模式抛 ValueError"""
        async def run():
            await ayafileio.aopen(self.files[0], "q")
        with self.assertRaises(ValueError):
            asyncio.run(run())

    def test_aopen_concurrent(self):
        """多个 aopen 并发：全部成功且内容正确"""
        async def run():
            async def one(p):
                async with await ayafileio.aopen(p, "rb") as f:
                    return await f.read()
            return await asyncio.gather(*(one(p) for p in self.files))
        results = asyncio.run(run())
        for p, got in zip(self.files, results):
            self.assertEqual(got, self._expected(p))

    def test_read_bytes_many(self):
        """批量读：顺序与输入一致，内容逐字节正确"""
        results = asyncio.run(ayafileio.read_bytes_many(self.files))
        self.assertEqual(len(results), len(self.files))
        for p, got in zip(self.files, results):
            self.assertEqual(got, self._expected(p))

    def test_read_bytes_many_limited(self):
        """限流不影响正确性：max_concurrency=1 退化为串行"""
        results = asyncio.run(
            ayafileio.read_bytes_many(self.files[:8], max_concurrency=1))
        for p, got in zip(self.files[:8], results):
            self.assertEqual(got, self._expected(p))

    def test_read_bytes_many_empty(self):
        """空输入返回空列表"""
        self.assertEqual(asyncio.run(ayafileio.read_bytes_many([])), [])

    def test_read_bytes_many_propagates_error(self):
        """批量中任一失败整体抛出（gather 默认语义）"""
        paths = self.files[:4] + [os.path.join(self.dir, "missing.bin")]
        with self.assertRaises(FileNotFoundError):
            asyncio.run(ayafileio.read_bytes_many(paths))

    def test_read_text_many(self):
        """批量文本读：编码与顺序正确"""
        paths = []
        for i in range(4):
            p = os.path.join(self.dir, f"t{i}.txt")
            with open(p, "w", encoding="utf-8") as fp:
                fp.write(f"文本-{i}")
            paths.append(p)
        try:
            results = asyncio.run(ayafileio.read_text_many(paths, encoding="utf-8"))
            self.assertEqual(results, [f"文本-{i}" for i in range(4)])
        finally:
            for p in paths:
                os.unlink(p)


if __name__ == "__main__":
    unittest.main()
