# 故事集

[English](STORY.md)

这里收集 ayafileio 的真实使用故事——基准测试、意外发现、脑洞用法，
以及偶尔的"它不适合我的场景，原因如下"。统统欢迎。

在哪儿看到有人用 ayafileio 干了有趣的事？或者你自己用它玩出了
作者没想到的花样？开个 issue 或 PR 分享出来——精彩的故事会被
收录进本页。

## pread/pwrite：诞生于一个 RWKV 推理引擎

[SSD-RWKV-engine](https://github.com/Kaboomlolxd/SSD-RWKV-engine) 在 decode
时从 SSD 流式读取 RWKV 权重，每个 token 都要从随机偏移读张量。它的作者
评估 ayafileio 作为 IO 后端时（见其 [changelog](https://github.com/Kaboomlolxd/SSD-RWKV-engine/blob/main/CHANGELOG.md)
v0.5.2–v0.5.4）发现：Windows 上 IOCP 的 seek+read 会在文件指针上
串行化——改用位置读（pread 语义）后实测快了约 25 倍。正是这次评估，
让 ayafileio 把 `read_at` / `write_at` 做成了一等 API。

同一次评估也讲了反面的一课：对他们那种"反复读同一片热点区域的小随机读"，
朴素的 `mmap` 全面胜出，他们最终切换到了 mmap。很合理——ayafileio 的
主场是并发流式 IO 和没法（或不想）整个映射的大文件；如果你的负载是
"热点区域反复读"，mmap 才是正确答案。

这个故事的两半，都塑造了今天的 ayafileio。
