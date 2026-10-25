"""原生 Future 的批量聚合。"""

import asyncio


async def _gather_io(requests):
    """Submit native Futures directly; drain submitted I/O before reporting errors."""
    pending = []
    try:
        for request in requests:
            pending.append(request)
    except BaseException:
        if pending:
            await asyncio.gather(*pending, return_exceptions=True)
        raise
    results = await asyncio.gather(*pending, return_exceptions=True)
    for result in results:
        if isinstance(result, BaseException):
            raise result
    return results
