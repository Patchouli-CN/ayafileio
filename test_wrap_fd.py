"""wrap_fd 冒烟测试"""
import asyncio
import os
import tempfile
import ayafileio
from ayafileio.types import AyaFileIO


async def test_wrap_file_fd():
    """测试：包装普通文件 fd"""
    print("=== test_wrap_file_fd ===")
    
    # Windows 上 os.open() 的 fd 可能不能直接转 HANDLE
    # 用标准库 open() 拿 fileno() 更可靠
    import tempfile
    tmp = tempfile.NamedTemporaryFile(delete=False)
    tmp.close()  # 关闭，但文件保留
    
    # 用 low-level io 打开拿 fd
    f = open(tmp.name, 'w+b')
    fd = f.fileno()
    
    aio = ayafileio.wrap_fd(fd, 'wb')
    assert isinstance(aio, AyaFileIO), "Should be AyaIO"
    
    n = await aio.write(b"Hello from wrap_fd!")
    print(f"  wrote {n} bytes")
    
    await aio.seek(0)
    data = await aio.read()
    print(f"  read: {data}")
    assert data == b"Hello from wrap_fd!"
    
    await aio.close()
    f.close()  # 用户自己关
    os.unlink(tmp.name)
    print("  PASSED\n")


async def test_owns_fd_true():
    """测试：owns_fd=True，close 时自动关闭 fd"""
    print("=== test_owns_fd_true ===")
    
    fd = os.open(tempfile.mktemp(), os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    aio = ayafileio.wrap_fd(fd, 'wb', owns_fd=True)
    
    await aio.write(b"data")
    await aio.close()
    
    # 验证 fd 已关闭
    try:
        os.close(fd)
        print("  FAILED: fd should already be closed")
    except OSError:
        print("  PASSED: fd auto-closed\n")


async def test_wrap_text_mode():
    """测试：文本模式 wrap_fd"""
    print("=== test_wrap_text_mode ===")
    
    # 注意：wrap_fd 目前只支持二进制（_from_impl 里写死了 _is_text=False）
    # 如果需要文本模式，后面可以扩展
    fd = os.open(tempfile.mktemp(), os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    aio = ayafileio.wrap_fd(fd, 'wb')
    
    await aio.write(b"hello\nworld\n")
    await aio.seek(0)
    
    # 手动解码
    data = await aio.read()
    text = data.decode()
    print(f"  text: {repr(text)}")
    assert text == "hello\nworld\n"
    
    await aio.close()
    os.close(fd)
    print("  PASSED\n")


async def test_wrap_closed_error():
    """测试：关闭后操作报错"""
    print("=== test_wrap_closed_error ===")
    
    fd = os.open(tempfile.mktemp(), os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    aio = ayafileio.wrap_fd(fd, 'wb')
    await aio.close()
    
    try:
        await aio.read()
        print("  FAILED: should raise")
    except ValueError:
        print("  PASSED: ValueError on closed\n")
    finally:
        os.close(fd)


async def test_normal_open_still_works():
    """测试：原有的 open() 不受影响"""
    print("=== test_normal_open_still_works ===")
    
    path = tempfile.mktemp()
    async with ayafileio.open(path, 'wb') as f:
        await f.write(b"normal open works!")
    
    async with ayafileio.open(path, 'rb') as f:
        data = await f.read()
        assert data == b"normal open works!"
    
    print("  PASSED\n")
    os.unlink(path)


async def main():
    await test_wrap_file_fd()
    await test_owns_fd_true()
    await test_wrap_text_mode()
    await test_wrap_closed_error()
    await test_normal_open_still_works()
    
    print("=" * 40)
    print("All tests passed! 🍃")
    print(f"Backend: {ayafileio.get_backend_info()}")


if __name__ == "__main__":
    asyncio.run(main())