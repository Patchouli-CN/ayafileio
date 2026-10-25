"""Native positioned-write regression tests: python -m unittest discover -s tests -p test_write_at.py."""
import asyncio
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

try:
    import ayafileio
except ModuleNotFoundError as exc:
    if exc.name != "ayafileio._ayafileio":
        raise
    ayafileio = None


@unittest.skipIf(ayafileio is None, "Build the native extension first")
class FileLifecycleTests(unittest.TestCase):
    def test_destroy_closed_files_after_loop_shutdown(self):
        # A leaked C API exception can surface on an unrelated Python opcode
        # or abort unittest itself, so exercise destruction in a subprocess.
        script = textwrap.dedent("""
            import asyncio
            import gc
            import sys
            import tempfile
            from pathlib import Path
            import ayafileio

            unraisable = []
            sys.unraisablehook = unraisable.append

            async def open_and_close(path):
                f = ayafileio.open(path, "w+b")
                await f.write_at(0, b"data")
                await f.close()
                await f.close()
                return f

            async def open_and_force_close(path):
                f = ayafileio.open(path, "w+b")
                f._close_impl()
                return f

            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "lifecycle.bin"
                for operation, repeat_close in (
                    (open_and_close, False),
                    (open_and_close, True),
                    (open_and_force_close, True),
                ):
                    f = asyncio.run(operation(path))
                    # No running event loop from here onwards.
                    if repeat_close:
                        f._close_impl()
                    del f
                    gc.collect()
                    assert path.read_bytes() in (b"data", b"")
                assert not unraisable, unraisable
            print("cleanup completed")
        """)
        result = subprocess.run(
            [sys.executable, "-c", script], capture_output=True, text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        # 线程池回退构建（例如没编 io_uring 的 Linux）在 import 时就会发
        # 后端提示——那是构建事实的提示，与这里要守的「析构路径不能抛
        # unraisable / 不能往 stderr 漏东西」无关，所以按后端状态放行。
        stderr = result.stderr
        if ayafileio.get_backend_info()["backend"] == "thread_pool":
            import re
            stderr = re.sub(r"^.*UserWarning: ayafileio:.*$", "", stderr, flags=re.M)

        self.assertEqual(stderr.strip(), "")
        self.assertIn("cleanup completed", result.stdout)


@unittest.skipIf(ayafileio is None, "Build the native extension first")
class PositionedWriteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "data.bin"
        self.file = ayafileio.open(self.path, "w+b")

    async def asyncTearDown(self):
        await self.file.close()
        self.tmp.cleanup()

    async def test_position_and_extension(self):
        f = self.file
        await f.write(b"0123456789")
        await f.seek(3)
        self.assertEqual(await f.write_at(5, b"XY"), 2)
        self.assertEqual(await f.tell(), 3)
        self.assertEqual(await f.read_at(0), b"01234XY789")
        self.assertEqual(await f.write_at(20, b"z"), 1)
        self.assertEqual(await f.tell(), 3)
        self.assertEqual(await f.seek(0, 2), 21)
        self.assertEqual(await f.read_at(10), b"\0" * 10 + b"z")

    async def test_many_and_buffers(self):
        chunks = [bytes([i]) * 257 for i in range(128)]
        counts = await self.file.write_many((i * 257, bytearray(c)) for i, c in enumerate(chunks))
        self.assertEqual(counts, [257] * 128)
        self.assertEqual(await self.file.tell(), 0)
        self.assertEqual(await self.file.read(), b"".join(chunks))
        self.assertEqual(await self.file.write_at(0, memoryview(b"abc")), 3)
        self.assertEqual(await self.file.write_many([]), [])
        self.assertEqual(await self.file.write_at(0, b""), 0)

    async def test_readahead_invalidation(self):
        await self.file.write(b"first\nold\n")
        await self.file.seek(0)
        self.assertEqual(await self.file.readline(), b"first\n")
        await self.file.write_at(6, b"new")
        self.assertEqual(await self.file.tell(), 6)
        self.assertEqual(await self.file.readline(), b"new\n")

    async def test_invalid_arguments_and_partial_batch(self):
        for offset, data, error in [(-1, b"x", ValueError), (2**63, b"x", (TypeError, OverflowError)),
                                    (2**63 - 1, b"x", OverflowError), (0, "x", TypeError),
                                    (0, memoryview(b"abc")[::2], BufferError)]:
            with self.assertRaises(error):
                await self.file.write_at(offset, data)
        with self.assertRaises(ValueError):
            await self.file.write_many([(0, b"ok"), (-1, b"bad")])
        self.assertEqual(await self.file.read_at(0), b"ok")

    async def test_modes_and_closed(self):
        for mode, error in [("ab", ValueError), ("rb", OSError), ("r+", ValueError)]:
            async with ayafileio.open(self.path, mode) as f:
                with self.assertRaises(error):
                    await f.write_at(0, b"x")
                with self.assertRaises(error):
                    await f.write_many([])
        await self.file.close()
        with self.assertRaises(ValueError):
            await self.file.write_at(0, b"")
        with self.assertRaises(ValueError):
            await self.file.write_many([])

    async def test_concurrent_positioned_and_sequential(self):
        await self.file.write(b"_" * 2048)
        await self.file.seek(1024)
        await asyncio.gather(self.file.write_many((i, b"x") for i in range(128)),
                             self.file.write(b"sequential"))
        self.assertEqual(await self.file.tell(), 1034)
        self.assertEqual(await self.file.read_at(0, 128), b"x" * 128)
        self.assertEqual(await self.file.read_at(1024, 10), b"sequential")


if __name__ == "__main__":
    unittest.main()
