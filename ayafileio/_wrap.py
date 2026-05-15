"""wrap_file — 将任意文件包装为异步 I/O 对象"""

from .util import is_real_file
from ._ayafileio import AsyncFile as _AsyncFile
from ._async_file import AsyncFile
from .types import FileObj

def wrap_file(file: int | FileObj, mode: str = "rb", *, owns_fd: bool = False) -> AsyncFile[bytes]:
    """将现有**文件**包装为异步 I/O 对象。

    底层自动选择最优平台后端（io_uring / IOCP / Dispatch I/O），
    提供真正的内核级异步文件读写。

    .. warning::

       此函数**仅支持文件描述符**（常规文件、临时文件等）。
       请勿传入 socket、pipe、tty 等非文件描述符——这些应由
       事件循环（asyncio / trio / anyio）直接管理。

    Args:
        file: 文件或者描述符对象。
        mode: 打开模式，与内建 ``open()`` 兼容（``'rb'``, ``'wb'``, ``'ab'`` 等）。
            **仅支持二进制模式**。
        owns_fd: 是否接管文件描述符的所有权。

            - ``False``（默认）：``close()`` 后原始 fd 保持打开，用户需自行关闭。
            - ``True``：``close()`` 时自动关闭底层文件描述符。

    Raises:
        ValueError: 如果 mode 不合法 | 不是真实磁盘文件对象

    Example:

        基本用法（用户自行管理 fd）：::

        ```python
            import os, ayafileio

            fd = os.open('data.bin', os.O_RDWR | os.O_CREAT)

            aio = ayafileio.wrap_file(fd, 'wb')

            await aio.write(b'some data')
            await aio.seek(0)
            data = await aio.read()

            await aio.close()       # 释放 aio 资源，fd 仍打开
            os.close(fd)            # 用户自行关闭 fd
        ```

        接管 fd 所有权：::

        ```python
        import os, ayafileio

        fd = os.open('data.bin', os.O_RDWR | os.O_CREAT)
        aio = ayafileio.wrap_file(fd, 'wb', owns_fd=True)

        await aio.write(b'data')

        await aio.close() # 自动关闭底层 fd

        # 无需再调用 os.close(fd)
        ```
    """
    if "b" not in mode:
        raise ValueError("wrap_file() only supports binary mode (e.g., 'rb', 'wb')")
    
    if isinstance(file, FileObj):
        fd = file.fileno()
    else:
        fd = file
        
    if not is_real_file(fd):
        raise ValueError(
            f"wrap_file() only supports regular files, "
            f"got {type(file).__name__} (fd={fd})"
        )

    impl = _AsyncFile(fd, mode, owns_fd)
    return AsyncFile._from_impl(impl)
