import asyncio, tempfile, sys
from pathlib import Path
import ayafileio

MESSAGE = "x" * 128 + "\n"
stats = ayafileio._ayafileio.debug_quarantine_stats

async def burst(path, count, workers=50):
    async with ayafileio.open(str(path), "a", encoding="utf-8") as h:
        async def w(i):
            for idx in range(i, count, workers):
                await h.write(MESSAGE)
        await asyncio.gather(*(w(i) for i in range(workers)))

async def main():
    before = stats()
    with tempfile.TemporaryDirectory() as root:
        await burst(Path(root) / "b.log", 20000)
    after = stats()
    print("pushes delta:", after["pushes"] - before["pushes"])
    print("scanned delta:", after["scanned"] - before["scanned"])
    print("live:", after["live"])

asyncio.run(main())
