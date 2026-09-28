# Stories

[简体中文](STORY_CN.md)

Real-world stories from people using ayafileio — benchmarks, surprises,
creative uses, and the occasional "it didn't fit, and here's why". All of it
is welcome here.

Found ayafileio in the wild doing something interesting? Using it in a way the
author never imagined? Open an issue or a PR and share it — the best entries
get added to this page.

## pread/pwrite: born from an RWKV inference engine

[SSD-RWKV-engine](https://github.com/Kaboomlolxd/SSD-RWKV-engine) streams RWKV
model weights from SSD at decode time, reading tensors from random offsets on
every token. When its author evaluated ayafileio as an I/O backend (v0.5.2–
v0.5.4 in their [changelog](https://github.com/Kaboomlolxd/SSD-RWKV-engine/blob/main/CHANGELOG.md)),
they found that on Windows, seek+read over IOCP serializes on the file
pointer — routing reads through positioned reads instead measured ~25× faster.
That evaluation is why ayafileio ships `read_at` / `write_at` (pread/pwrite
semantics) as first-class APIs.

The same evaluation also taught the opposite lesson: for their workload — hot,
repeated, small random reads of the same regions — plain `mmap` beat async
I/O entirely, and they eventually switched to it. Fair. ayafileio's home turf
is concurrent streaming I/O and files you can't (or don't want to) map
wholesale; if your workload is "re-read the same hot region forever", mmap is
the right tool.

Both halves of that story shaped the library.
