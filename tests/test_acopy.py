"""acopy 异步整文件复制测试: python -m unittest discover -s tests -p test_acopy.py"""
import os
import random
import shutil
import tempfile
import unittest
from pathlib import Path

try:
    import ayafileio
    from ayafileio import _copy
except ModuleNotFoundError as exc:
    if exc.name != "ayafileio._ayafileio":
        raise
    ayafileio = None


@unittest.skipIf(ayafileio is None, "Build the native extension first")
class AcopyTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _make_file(self, name, size, seed=0):
        data = random.Random(seed).randbytes(size)
        path = self.dir / name
        path.write_bytes(data)
        return path, data

    async def test_small_file(self):
        src, data = self._make_file("small.bin", 1234)
        dst = self.dir / "small.out"
        n = await ayafileio.acopy(src, dst, chunk_size=4096)
        self.assertEqual(n, len(data))
        self.assertEqual(dst.read_bytes(), data)

    async def test_multi_chunk(self):
        # 3 MiB + 7 B, 64 KiB 块 → 49 块, 尾块不满
        src, data = self._make_file("multi.bin", 3 * 1024 * 1024 + 7, seed=1)
        dst = self.dir / "multi.out"
        n = await ayafileio.acopy(src, dst, chunk_size=64 * 1024, concurrency=8)
        self.assertEqual(n, len(data))
        self.assertEqual(dst.read_bytes(), data)

    async def test_empty_file(self):
        src, data = self._make_file("empty.bin", 0)
        dst = self.dir / "empty.out"
        n = await ayafileio.acopy(src, dst)
        self.assertEqual(n, 0)
        self.assertEqual(dst.read_bytes(), b"")

    async def test_overwrite_longer_dst(self):
        src, data = self._make_file("short.bin", 100)
        dst = self.dir / "short.out"
        dst.write_bytes(b"\xff" * 10000)  # 目标比源长, 必须被截断到源大小
        n = await ayafileio.acopy(src, dst)
        self.assertEqual(n, 100)
        self.assertEqual(dst.read_bytes(), data)

    async def test_chunk_boundaries(self):
        for i, delta in enumerate((-1, 0, 1)):
            cs = 32 * 1024
            src, data = self._make_file(f"edge{i}.bin", cs + delta, seed=10 + i)
            dst = self.dir / f"edge{i}.out"
            n = await ayafileio.acopy(src, dst, chunk_size=cs)
            self.assertEqual(n, len(data))
            self.assertEqual(dst.read_bytes(), data)

    async def test_same_file_raises(self):
        src, _ = self._make_file("same.bin", 64)
        with self.assertRaises(shutil.SameFileError):
            await ayafileio.acopy(src, src)

    async def test_pipeline_path(self):
        # 强制走 read_at/write_at 流水线（默认是 OS 级快车道）
        saved = _copy._os_copy
        _copy._os_copy = None
        try:
            src, data = self._make_file("pipe.bin", 1024 * 1024 + 3, seed=2)
            dst = self.dir / "pipe.out"
            n = await ayafileio.acopy(src, dst, chunk_size=128 * 1024, concurrency=4)
            self.assertEqual(n, len(data))
            self.assertEqual(dst.read_bytes(), data)
        finally:
            _copy._os_copy = saved

    async def test_copy_stat(self):
        src, data = self._make_file("stat.bin", 256)
        os.utime(src, (1000000000, 1000000000))
        dst = self.dir / "stat.out"
        await ayafileio.acopy(src, dst, copy_stat=True)
        self.assertEqual(dst.read_bytes(), data)
        self.assertAlmostEqual(
            os.stat(src).st_mtime, os.stat(dst).st_mtime, delta=2.0
        )

    async def test_concurrent_copies(self):
        import asyncio

        specs = [self._make_file(f"c{i}.bin", 100 * 1024 + i, seed=20 + i) for i in range(4)]
        results = await asyncio.gather(
            *(ayafileio.acopy(src, self.dir / f"c{i}.out", chunk_size=16 * 1024)
              for i, (src, _) in enumerate(specs))
        )
        for (src, data), n in zip(specs, results):
            self.assertEqual(n, len(data))
        for i, (_, data) in enumerate(specs):
            self.assertEqual((self.dir / f"c{i}.out").read_bytes(), data)

    async def test_bad_params(self):
        src, _ = self._make_file("p.bin", 8)
        with self.assertRaises(ValueError):
            await ayafileio.acopy(src, self.dir / "p.out", chunk_size=0)
        with self.assertRaises(ValueError):
            await ayafileio.acopy(src, self.dir / "p.out", concurrency=0)

    async def test_missing_src_raises(self):
        with self.assertRaises(OSError):
            await ayafileio.acopy(self.dir / "nope.bin", self.dir / "nope.out")


if __name__ == "__main__":
    unittest.main()
