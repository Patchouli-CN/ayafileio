"""minimal_test.py - 极简测试，排查 ayafileio 本身是否有问题"""
import asyncio
import time
from pathlib import Path

async def test_basic():
    """基础读写测试"""
    print("=" * 50)
    print("测试 1: 基础读写")
    print("=" * 50)
    
    test_path = Path("minimal_test.dat")
    
    # 创建测试文件
    print(f"\n创建测试文件: {test_path.absolute()}")
    with open(test_path, "wb") as f:
        f.write(b"0" * 1024 * 1024)  # 1MB
    print("✓ 文件创建成功")
    
    # 测试导入
    try:
        import ayafileio
        print("✓ ayafileio 导入成功")
    except ImportError as e:
        print(f"✗ 导入失败: {e}")
        return
    
    # 测试读取
    print("\n--- 测试读取 ---")
    try:
        async with ayafileio.open(str(test_path), "rb") as f:
            start = time.perf_counter()
            data = await f.read(1024)  # 读 1KB
            elapsed = time.perf_counter() - start
            print(f"✓ 读取成功: {len(data)} 字节, 耗时 {elapsed*1000:.2f}ms")
    except Exception as e:
        print(f"✗ 读取失败: {e}")
        import traceback
        traceback.print_exc()
    
    # 测试写入
    print("\n--- 测试写入 ---")
    write_path = Path("minimal_write.dat")
    try:
        async with ayafileio.open(str(write_path), "wb") as f:
            start = time.perf_counter()
            await f.write(b"X" * 1024)  # 写 1KB
            elapsed = time.perf_counter() - start
            print(f"✓ 写入成功: 1024 字节, 耗时 {elapsed*1000:.2f}ms")
    except Exception as e:
        print(f"✗ 写入失败: {e}")
        import traceback
        traceback.print_exc()
    
    # 测试 seek
    print("\n--- 测试 seek ---")
    try:
        async with ayafileio.open(str(test_path), "rb") as f:
            await f.seek(100)
            pos = await f.seek(0, 1)  # 当前位置
            print(f"✓ seek 成功: 当前位置 {pos}")
    except Exception as e:
        print(f"✗ seek 失败: {e}")
    
    # 清理
    test_path.unlink(missing_ok=True)
    write_path.unlink(missing_ok=True)
    print("\n✓ 清理完成")


async def test_concurrent_simple():
    """简单并发测试"""
    print("\n" + "=" * 50)
    print("测试 2: 简单并发 (10 个任务)")
    print("=" * 50)
    
    import ayafileio
    
    # 准备多个测试文件
    test_files = []
    for i in range(5):
        path = Path(f"concurrent_test_{i}.dat")
        with open(path, "wb") as f:
            f.write(os.urandom(100 * 1024))  # 100KB
        test_files.append(str(path))
    
    print(f"✓ 创建了 {len(test_files)} 个测试文件")
    
    async def worker(worker_id: int):
        """单个工作协程"""
        ops = 0
        try:
            for _ in range(10):  # 每个 worker 做 10 次操作
                file_path = test_files[worker_id % len(test_files)]
                async with ayafileio.open(file_path, "rb") as f:
                    await f.seek(0)
                    data = await f.read(4096)
                    ops += 1
            return ops
        except Exception as e:
            print(f"Worker {worker_id} 错误: {e}")
            return 0
    
    print("\n开始并发测试...")
    start = time.perf_counter()
    
    # 10 个并发任务
    tasks = [worker(i) for i in range(10)]
    results = await asyncio.gather(*tasks)
    
    elapsed = time.perf_counter() - start
    total_ops = sum(results)
    
    print(f"✓ 并发完成: {total_ops} 次操作, 耗时 {elapsed:.2f}秒")
    print(f"✓ 平均: {total_ops/elapsed:.1f} ops/s")
    
    # 清理
    for path in test_files:
        Path(path).unlink(missing_ok=True)
    print("✓ 清理完成")


async def test_timeout():
    """测试 I/O 是否会卡住"""
    print("\n" + "=" * 50)
    print("测试 3: 超时测试 (确保 I/O 不会永久卡住)")
    print("=" * 50)
    
    import ayafileio
    
    test_path = Path("timeout_test.dat")
    with open(test_path, "wb") as f:
        f.write(b"0" * 10 * 1024 * 1024)  # 10MB
    
    print("✓ 创建 10MB 测试文件")
    
    async def read_with_timeout():
        try:
            async with ayafileio.open(str(test_path), "rb") as f:
                # 设置 3 秒超时
                return await asyncio.wait_for(f.read(10*1024*1024), timeout=3.0)
        except asyncio.TimeoutError:
            return "TIMEOUT"
        except Exception as e:
            return f"ERROR: {e}"
    
    print("\n开始读取 (3秒超时)...")
    start = time.perf_counter()
    result = await read_with_timeout()
    elapsed = time.perf_counter() - start
    
    if result == "TIMEOUT":
        print(f"⚠️ 读取超时 ({elapsed:.2f}秒) - 可能 I/O 卡住了")
    elif isinstance(result, bytes):
        print(f"✓ 读取成功: {len(result)} 字节, 耗时 {elapsed:.2f}秒")
    else:
        print(f"✗ {result}")
    
    test_path.unlink(missing_ok=True)


async def main():
    print("ayafileio 极简测试")
    print(f"Python 版本: {__import__('sys').version}")
    print(f"平台: {__import__('platform').platform()}")
    
    await test_basic()
    await test_concurrent_simple()
    await test_timeout()
    
    print("\n" + "=" * 50)
    print("所有测试完成!")

if __name__ == "__main__":
    import os
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n中断")