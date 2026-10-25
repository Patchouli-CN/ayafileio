"""Wrapper regressions runnable without a compiler or native extension."""
import asyncio
import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch


def load_wrapper():
    root = Path(__file__).resolve().parents[2]
    package = types.ModuleType("_aya_wrapper_test")
    package.__path__ = [str(root / "python" / "ayafileio")]
    native = types.ModuleType("_aya_wrapper_test._ayafileio")
    native.AsyncFile = object
    spec = importlib.util.spec_from_file_location(
        "_aya_wrapper_test._async_file",
        root / "python" / "ayafileio" / "_async_file.py")
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {package.__name__: package, native.__name__: native}):
        spec.loader.exec_module(module)
    return module


wrapper = load_wrapper()


class MemoryBackend:
    def __init__(self, content):
        self.stream = io.BytesIO(content)

    def result(self, value):
        future = asyncio.get_running_loop().create_future()
        future.set_result(value)
        return future

    def read(self, size):
        return self.result(self.stream.read(size))

    def read_at(self, offset, size):
        position = self.stream.tell()
        self.stream.seek(offset)
        data = self.stream.read(size)
        self.stream.seek(position)
        return self.result(data)

    def write_at(self, offset, data):
        if offset < 0:
            raise ValueError("negative offset")
        position = self.stream.tell()
        self.stream.seek(offset)
        count = self.stream.write(data)
        self.stream.seek(position)
        return self.result(count)

    def tell(self):
        return self.result(self.stream.tell())

    def seek(self, offset, whence=0):
        return self.result(self.stream.seek(offset, whence))


class WrapperTests(unittest.IsolatedAsyncioTestCase):
    def file(self, data=b""):
        return wrapper.AsyncFile._from_impl(MemoryBackend(data), "r+b")

    async def test_positioned_writes_invalidate_readahead(self):
        f = self.file(b"one\nold\n")
        self.assertEqual(await f.readline(), b"one\n")
        self.assertEqual(await f.write_at(4, bytearray(b"new")), 3)
        self.assertEqual(await f.tell(), 4)
        self.assertEqual(await f.readline(), b"new\n")
        self.assertEqual(await f.write_many([(4, b"NEW"), (0, memoryview(b"ONE"))]), [3, 3])
        self.assertEqual(await f.tell(), 8)
        self.assertEqual(await f.read_many([(4, 3), (0, 3)]), [b"NEW", b"ONE"])

    async def test_batch_drains_on_submission_error(self):
        done = asyncio.get_running_loop().create_future()
        def requests():
            yield done
            asyncio.get_running_loop().call_soon(done.set_result, 7)
            raise ValueError("submission")
        with self.assertRaisesRegex(ValueError, "submission"):
            await wrapper._gather_io(requests())
        self.assertTrue(done.done())

    async def test_batch_drains_on_completion_error(self):
        loop = asyncio.get_running_loop()
        failed, done = loop.create_future(), loop.create_future()
        failed.set_exception(OSError("write failed"))
        loop.call_soon(done.set_result, 3)
        with self.assertRaises(OSError):
            await wrapper._gather_io(iter([failed, done]))
        self.assertEqual(done.result(), 3)
        self.assertEqual(await wrapper._gather_io([]), [])

    async def test_long_line_scans_only_new_bytes(self):
        data = b"a" * (8 * 65536) + b"\r\nlast\r"
        f = self.file(data)
        with patch.object(wrapper, "_find_line_end", wraps=wrapper._find_line_end) as scan:
            self.assertEqual(await f.readline(), data[:-5])
            starts = [call.args[1] for call in scan.call_args_list]
            self.assertEqual(starts[:4], [0, 0, 65535, 131071])
        self.assertEqual(await f.readline(), b"last\r")
        self.assertEqual(await f.readline(), b"")

    async def test_text_crlf_split_and_compaction(self):
        f = self.file(b"short\n" + b"a" * (65536 - 7) + b"\r\ntail")
        f._is_text = True
        f._encoding = "utf-8"
        self.assertEqual(await f.readline(), "short\n")
        self.assertEqual(await f.readline(), "a" * (65536 - 7) + "\n")
        self.assertEqual(await f.readline(), "tail")

    async def test_binary_cr_is_not_a_separator(self):
        f = self.file(b"a\rb\r\nc\r")
        self.assertEqual(await f.readline(), b"a\rb\r\n")
        self.assertEqual(await f.readline(), b"c\r")


if __name__ == "__main__":
    unittest.main()
