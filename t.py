# test_ayafileio.py
import asyncio
from pathlib import Path
import ayafileio

async def test_chinese_path():
    # 创建测试目录和文件
    test_dir = Path("sessions/西行寺幽幽子")
    test_dir.mkdir(parents=True, exist_ok=True)
    
    test_file = test_dir / "test.json"
    
    # 写入测试数据
    print(f"测试文件路径: {test_file.absolute()}")
    print(f"文件存在? {test_file.exists()}")
    
    # 测试 1: 直接用字符串
    print("\n测试 1: str 路径")
    try:
        async with ayafileio.open(str(test_file), "w", encoding="utf-8") as f:
            await f.write('{"test": "hello"}')
        print("✓ 写入成功")
        
        async with ayafileio.open(str(test_file), "r", encoding="utf-8") as f:
            content = await f.read()
            print(f"✓ 读取成功: {content}")
    except Exception as e:
        print(f"✗ 失败: {type(e).__name__}: {e}")
    
    # 测试 2: Path 对象
    print("\n测试 2: Path 对象")
    try:
        async with ayafileio.open(test_file, "r", encoding="utf-8") as f:
            content = await f.read()
            print(f"✓ 读取成功: {content}")
    except Exception as e:
        print(f"✗ 失败: {type(e).__name__}: {e}")
    
    # 测试 3: 相对路径
    print("\n测试 3: 相对路径字符串")
    rel_path = "sessions/西行寺幽幽子/test.json"
    try:
        async with ayafileio.open(rel_path, "r", encoding="utf-8") as f:
            content = await f.read()
            print(f"✓ 读取成功: {content}")
    except Exception as e:
        print(f"✗ 失败: {e}")
    
    # 测试 4: 并发读写
    print("\n测试 4: 并发操作")
    async def write_file(i):
        fname = test_dir / f"concurrent_{i}.json"
        async with ayafileio.open(str(fname), "w", encoding="utf-8") as f:
            await f.write(f'{{"id": {i}}}')
        return i
    
    try:
        tasks = [write_file(i) for i in range(10)]
        results = await asyncio.gather(*tasks, return_exceptions=True)
        success = sum(1 for r in results if not isinstance(r, Exception))
        print(f"✓ 并发完成: {success}/10")
        
        # 清理
        for i in range(10):
            (test_dir / f"concurrent_{i}.json").unlink(missing_ok=True)
    except Exception as e:
        print(f"✗ 失败: {type(e).__name__}: {e}")
    
    # 清理测试文件
    test_file.unlink(missing_ok=True)

if __name__ == "__main__":
    asyncio.run(test_chinese_path())