"""batcher 在飞 op 记账回归测试。

Windows skipCPOnSuccess 同步完成的 op 不投完成包，提交线程收尾时必须
配平 op_completed()，否则 m_outstanding 只增不减，"最后一个在飞 op
完成立即 flush" 永久失效，异步完成的 op 全部退化到等 5ms 空闲清扫
周期（CI windows-2022 实测 x1 随机读因此只有 144 ops/s）。

本测试在所有平台上断言：一批读写完成后，记账必须归零。

注意：是否触发依赖机器的存储栈行为——只有在 ReadFile/WriteFile 走
"同步完成 + skipCPOnSuccess" 路径的机器上（如 CI 的 windows-2022）
才能覆盖到泄漏点；同步完成不发生（如本地 Win10）的机器上本测试
恒过，不代表覆盖无效。
"""

import asyncio
import os
import tempfile
import unittest

import ayafileio
from ayafileio._ayafileio import debug_batcher_outstanding


class TestBatcherAccounting(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="aya_acct_")
        cls.path = os.path.join(cls.dir, "f.bin")
        with open(cls.path, "wb") as fp:
            fp.write(os.urandom(1024 * 1024))

    @classmethod
    def tearDownClass(cls):
        try:
            os.unlink(cls.path)
            os.rmdir(cls.dir)
        except OSError:
            pass

    def test_outstanding_returns_to_zero(self):
        """200 次随机读 + 100 次写后，在飞记账必须归零"""
        async def run():
            async with ayafileio.open(self.path, "rb") as f:
                for i in range(200):
                    await f.read_at((i * 4096) % (1024 * 1024 - 4096), 4096)
            wpath = os.path.join(self.dir, "w.bin")
            async with ayafileio.open(wpath, "wb") as f:
                for _ in range(100):
                    await f.write(b"x" * 4096)
            await asyncio.sleep(0.1)  # 异步完成的 op 排水窗口
            return debug_batcher_outstanding()

        try:
            self.assertEqual(asyncio.run(run()), 0)
        finally:
            wp = os.path.join(self.dir, "w.bin")
            if os.path.exists(wp):
                os.unlink(wp)


if __name__ == "__main__":
    unittest.main()
