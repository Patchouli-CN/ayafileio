#!/usr/bin/env python3
"""
ayafileio vs aiofiles 专业基准测试
包含延迟分布、抖动分析、配置调优对比、事件循环基准校准
"""

import asyncio
import tempfile
import time
import json
import os
import io
import sys
import statistics
from pathlib import Path
from dataclasses import dataclass, field
from typing import Callable, Awaitable

# 设置 Windows 控制台编码
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# Rich（可选）
RICH_AVAILABLE = False
console = None
try:
    from rich.console import Console
    from rich.panel import Panel

    RICH_AVAILABLE = True
    console = Console()
except ImportError:
    pass

# 检查依赖
try:
    import aiofiles
except ImportError:
    print("错误：请先安装 aiofiles: pip install aiofiles")
    sys.exit(1)

try:
    import ayafileio
except ImportError:
    print("错误：请先安装 ayafileio")
    sys.exit(1)


# ════════════════════════════════════════════════════════════════════════════
# 配置与统计
# ════════════════════════════════════════════════════════════════════════════


@dataclass
class Config:
    screenshot_size: int = 512 * 1024
    screenshot_count: int = 100
    item_size: int = 1024
    item_count: int = 5000
    concurrent_limit: int = 50
    warmup_rounds: int = 2
    test_rounds: int = 5
    enable_tuning: bool = True
    tuning_mode: str = "balanced"


@dataclass
class BenchmarkStats:
    name: str
    values: list[float] = field(default_factory=list)

    @property
    def count(self) -> int:
        return len(self.values)

    @property
    def mean(self) -> float:
        return statistics.mean(self.values) if self.values else 0

    @property
    def median(self) -> float:
        return statistics.median(self.values) if self.values else 0

    @property
    def stdev(self) -> float:
        return statistics.stdev(self.values) if len(self.values) > 1 else 0

    @property
    def p95(self) -> float:
        if not self.values:
            return 0
        return sorted(self.values)[int(len(self.values) * 0.95)]

    @property
    def p99(self) -> float:
        if not self.values:
            return 0
        return sorted(self.values)[int(len(self.values) * 0.99)]

    @property
    def min_val(self) -> float:
        return min(self.values) if self.values else 0

    @property
    def max_val(self) -> float:
        return max(self.values) if self.values else 0

    @property
    def jitter(self) -> float:
        return (self.stdev / self.mean * 100) if self.mean > 0 else 0

    @property
    def range_ratio(self) -> float:
        return (self.max_val / self.min_val) if self.min_val > 0 else 0

    def to_dict(self) -> dict:
        return {
            "count": self.count,
            "mean": self.mean,
            "median": self.median,
            "stdev": self.stdev,
            "p95": self.p95,
            "p99": self.p99,
            "min": self.min_val,
            "max": self.max_val,
            "jitter_percent": self.jitter,
            "range_ratio": self.range_ratio,
        }


# ════════════════════════════════════════════════════════════════════════════
# 工具函数
# ════════════════════════════════════════════════════════════════════════════


def println(msg: str) -> None:
    if RICH_AVAILABLE and console:
        console.print(msg, markup=False)
    else:
        print(msg)


def format_duration(seconds: float) -> str:
    if seconds < 0.000001:
        return f"{seconds * 1_000_000_000:.0f}ns"
    elif seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}μs"
    elif seconds < 1:
        return f"{seconds * 1000:.1f}ms"
    return f"{seconds:.3f}s"


# ════════════════════════════════════════════════════════════════════════════
# 通用测试框架
# ════════════════════════════════════════════════════════════════════════════


async def run_benchmark_rounds(
    name: str, lib: str, bench_fn: Callable[[], Awaitable[float]], config: Config
) -> BenchmarkStats:
    times = []
    for rnd in range(config.test_rounds + config.warmup_rounds):
        elapsed = await bench_fn()
        if rnd >= config.warmup_rounds:
            times.append(elapsed)
            println(
                f"  {lib:10} 第{rnd - config.warmup_rounds + 1}轮: {format_duration(elapsed)}"
            )
    return BenchmarkStats(name=f"{lib}_{name}", values=times)


def print_stats(lib: str, stats: BenchmarkStats, sleep_median_us: float = 0):
    if RICH_AVAILABLE and console:
        color = "cyan" if lib == "ayafileio" else "dim"
        console.print(
            f"\n  📈 [{color}]{lib}[/{color}]: 中位数 {format_duration(stats.median)}, "
            f"抖动 {stats.jitter:.1f}%, P99 {format_duration(stats.p99)}"
        )
    else:
        println(
            f"\n  📈 {lib}: 中位数 {format_duration(stats.median)}, "
            f"抖动 {stats.jitter:.1f}%, P99 {format_duration(stats.p99)}"
        )


def print_latency_detail(
    lib: str, latency_stats: BenchmarkStats, sleep_median_us: float = 0
):
    if RICH_AVAILABLE and console:
        color = "cyan" if lib == "ayafileio" else "dim"
        console.print(f"\n  📝 [{color}]{lib}[/{color}] 单次 write 延迟:")
        console.print(
            f"      中位数: [green]{latency_stats.median * 1_000_000:.1f}μs[/green], "
            f"P95: {latency_stats.p95 * 1_000_000:.1f}μs, P99: {latency_stats.p99 * 1_000_000:.1f}μs"
        )
        console.print(
            f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x"
        )
    else:
        println(f"\n  📝 {lib} 单次 write 延迟:")
        println(
            f"      中位数: {latency_stats.median * 1_000_000:.1f}μs, "
            f"P95: {latency_stats.p95 * 1_000_000:.1f}μs, P99: {latency_stats.p99 * 1_000_000:.1f}μs"
        )
        println(
            f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x"
        )


async def calibrate_event_loop_latency() -> dict:
    println("\n" + "─" * 80)
    println("📏 平台延迟基准: asyncio.sleep(0)")
    println("─" * 80)
    latencies = []
    for _ in range(1000):
        t0 = time.perf_counter()
        await asyncio.sleep(0)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1_000_000)
    stats = {
        "median_us": statistics.median(latencies),
        "p95_us": sorted(latencies)[int(len(latencies) * 0.95)],
        "p99_us": sorted(latencies)[int(len(latencies) * 0.99)],
        "min_us": min(latencies),
        "max_us": max(latencies),
    }
    println(
        f"  中位数: {stats['median_us']:.1f}μs, P95: {stats['p95_us']:.1f}μs, "
        f"P99: {stats['p99_us']:.1f}μs, 最小: {stats['min_us']:.1f}μs"
    )
    return stats


async def run_scenario(
    name: str,
    title: str,
    bench_builder: Callable[[str, Config], Awaitable[Callable[[], Awaitable[float]]]],
    config: Config,
    sleep_stats: dict,
    results: dict,
    extra_config: Config = None,
) -> dict:
    """通用场景运行器：消除所有重复的 for lib / print / speedup 逻辑"""
    cfg = extra_config or config
    println("\n" + "─" * 80)
    println(f"📊 {title}")
    println("─" * 80)

    result = {}
    for lib in ["ayafileio", "aiofiles"]:
        bench_fn = await bench_builder(lib, cfg)
        stats = await run_benchmark_rounds(name, lib, bench_fn, cfg)
        result[lib] = stats
        print_stats(lib, stats, sleep_stats["median_us"])

    aya = result["ayafileio"]
    aio = result["aiofiles"]
    speedup = aio.median / aya.median if aya.median > 0 else 0
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else "  (持平)")
    results["benchmarks"][name] = {
        "ayafileio": aya.to_dict(),
        "aiofiles": aio.to_dict(),
        "speedup": speedup,
    }
    return result


# ════════════════════════════════════════════════════════════════════════════
# 场景构建器
# ════════════════════════════════════════════════════════════════════════════


def _sem(cfg: Config) -> asyncio.Semaphore:
    return asyncio.Semaphore(cfg.concurrent_limit)


async def build_kvs_write(lib: str, cfg: Config) -> Callable[[], Awaitable[float]]:
    sem = _sem(cfg)

    async def bench() -> float:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            data_list = [
                os.urandom(cfg.screenshot_size) for _ in range(cfg.screenshot_count)
            ]
            if lib == "ayafileio":

                async def fn(i, d):
                    async with sem:
                        async with ayafileio.open(tmp / f"file_{i}.bin", "wb") as f:
                            await f.write(d)
            else:

                async def fn(i, d):
                    async with sem:
                        async with aiofiles.open(tmp / f"file_{i}.bin", "wb") as f:
                            await f.write(d)

            t0 = time.perf_counter()
            await asyncio.gather(*[fn(i, d) for i, d in enumerate(data_list)])
            await asyncio.sleep(0.05)
            return time.perf_counter() - t0

    return bench


async def build_kvs_read(lib: str, cfg: Config) -> Callable[[], Awaitable[float]]:
    sem = _sem(cfg)

    async def bench() -> float:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            data_list = [
                os.urandom(cfg.screenshot_size) for _ in range(cfg.screenshot_count)
            ]
            for i, d in enumerate(data_list):
                (tmp / f"file_{i}.bin").write_bytes(d)
            if lib == "ayafileio":

                async def fn(i):
                    async with sem:
                        async with ayafileio.open(tmp / f"file_{i}.bin", "rb") as f:
                            return await f.read()
            else:

                async def fn(i):
                    async with sem:
                        async with aiofiles.open(tmp / f"file_{i}.bin", "rb") as f:
                            return await f.read()

            t0 = time.perf_counter()
            await asyncio.gather(*[fn(i) for i in range(cfg.screenshot_count)])
            await asyncio.sleep(0.05)
            return time.perf_counter() - t0

    return bench


async def build_dataset_write(lib: str, cfg: Config) -> Callable[[], Awaitable[float]]:
    sem = _sem(cfg)
    latencies: list[float] = []

    async def bench() -> float:
        latencies.clear()
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "dataset.jsonl"
            items = [
                {
                    "url": f"https://ex.com/{os.urandom(4).hex()}",
                    "title": os.urandom(8).hex(),
                    "ts": time.time(),
                    "data": os.urandom(cfg.item_size - 200).hex()[
                        : cfg.item_size - 200
                    ],
                }
                for _ in range(cfg.item_count)
            ]
            batch_size = len(items) // 10
            batches = [
                items[i : i + batch_size] for i in range(0, len(items), batch_size)
            ]
            if lib == "ayafileio":

                async def write_batch(batch):
                    async with sem:
                        async with ayafileio.open(path, "a", encoding="utf-8") as f:
                            for item in batch:
                                line = json.dumps(item, ensure_ascii=False) + "\n"
                                t0 = time.perf_counter_ns()
                                await f.write(line)
                                latencies.append((time.perf_counter_ns() - t0) / 1e9)
            else:

                async def write_batch(batch):
                    async with sem:
                        async with aiofiles.open(path, "a", encoding="utf-8") as f:
                            for item in batch:
                                line = json.dumps(item, ensure_ascii=False) + "\n"
                                t0 = time.perf_counter_ns()
                                await f.write(line)
                                latencies.append((time.perf_counter_ns() - t0) / 1e9)

            t0 = time.perf_counter()
            await asyncio.gather(*[write_batch(b) for b in batches])
            await asyncio.sleep(0.05)
            return time.perf_counter() - t0

    return bench


async def build_mixed(lib: str, cfg: Config) -> Callable[[], Awaitable[float]]:
    sem = _sem(cfg)
    import random

    async def bench() -> float:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            read_count = 50
            write_data = [os.urandom(cfg.screenshot_size) for _ in range(30)]
            for i in range(read_count):
                (tmp / f"existing_{i}.bin").write_bytes(os.urandom(cfg.screenshot_size))
            if lib == "ayafileio":

                async def r(i):
                    async with sem:
                        async with ayafileio.open(tmp / f"existing_{i}.bin", "rb") as f:
                            return await f.read()

                async def w(i, d):
                    async with sem:
                        async with ayafileio.open(tmp / f"mixed_{i}.bin", "wb") as f:
                            await f.write(d)
            else:

                async def r(i):
                    async with sem:
                        async with aiofiles.open(tmp / f"existing_{i}.bin", "rb") as f:
                            return await f.read()

                async def w(i, d):
                    async with sem:
                        async with aiofiles.open(tmp / f"mixed_{i}.bin", "wb") as f:
                            await f.write(d)

            tasks = [r(i) for i in range(read_count)] + [
                w(i, d) for i, d in enumerate(write_data)
            ]
            random.shuffle(tasks)
            t0 = time.perf_counter()
            await asyncio.gather(*tasks)
            await asyncio.sleep(0.05)
            return time.perf_counter() - t0

    return bench


async def build_tempfile_storm(lib: str, cfg: Config) -> Callable[[], Awaitable[float]]:
    sem = _sem(cfg)
    FILE_COUNT, FILE_SIZE = 2000, 4096

    async def bench() -> float:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            if lib == "ayafileio":

                async def fn(i):
                    async with sem:
                        p = tmp / f"temp_{i}.bin"
                        d = os.urandom(FILE_SIZE)
                        async with ayafileio.open(p, "wb") as f:
                            await f.write(d)
                        async with ayafileio.open(p, "rb") as f:
                            await f.read()
                        p.unlink(missing_ok=True)
            else:

                async def fn(i):
                    async with sem:
                        p = tmp / f"temp_{i}.bin"
                        d = os.urandom(FILE_SIZE)
                        async with aiofiles.open(p, "wb") as f:
                            await f.write(d)
                        async with aiofiles.open(p, "rb") as f:
                            await f.read()
                        p.unlink(missing_ok=True)

            t0 = time.perf_counter()
            await asyncio.gather(*[fn(i) for i in range(FILE_COUNT)])
            await asyncio.sleep(0.05)
            return time.perf_counter() - t0

    return bench


# ════════════════════════════════════════════════════════════════════════════
# 平台自适应调优
# ════════════════════════════════════════════════════════════════════════════


def apply_smart_tuning(config: Config):
    platform_info = {
        "system": sys.platform,
        "cpu_count": os.cpu_count() or 4,
        "is_ci": os.environ.get("CI", "").lower() in ("true", "1", "yes"),
    }
    backend_info = ayafileio.get_backend_info()

    if RICH_AVAILABLE and console:
        console.print(
            Panel(
                f"[cyan]系统:[/cyan] {platform_info['system']}\n"
                f"[cyan]后端:[/cyan] {backend_info['backend']}\n"
                f"[cyan]CPU:[/cyan] {platform_info['cpu_count']} 核心\n"
                f"[cyan]CI 环境:[/cyan] {platform_info['is_ci']}\n"
                f"[cyan]调优模式:[/cyan] {config.tuning_mode}",
                title="🔧 平台自适应调优",
                border_style="cyan",
            )
        )
    else:
        println(
            f"\n🔧 平台自适应调优: 系统={platform_info['system']}, "
            f"后端={backend_info['backend']}, CPU={platform_info['cpu_count']}核心"
        )

    if config.tuning_mode == "none":
        return

    backend = backend_info["backend"]
    mode = config.tuning_mode
    is_ci = platform_info["is_ci"]

    tuning = {
        "iocp": {
            "buffer_size": 512 * 1024,
            "buffer_pool_max": 1024,
            "close_timeout_ms": 3000,
        },
        "io_uring": {
            "throughput": {
                "buffer_size": 256 * 1024,
                "buffer_pool_max": 1024,
                "io_uring_queue_depth": 512,
                "io_uring_sqpoll": not is_ci,
                "close_timeout_ms": 4000,
            },
            "latency": {
                "buffer_size": 64 * 1024,
                "buffer_pool_max": 512,
                "io_uring_queue_depth": 256,
                "io_uring_sqpoll": False,
                "close_timeout_ms": 2000,
            },
        }.get(
            mode,
            {
                "buffer_size": 128 * 1024,
                "buffer_pool_max": 768,
                "io_uring_queue_depth": 384,
                "io_uring_sqpoll": False,
                "close_timeout_ms": 3000,
            },
        ),
        "dispatch_io": {
            "throughput": {
                "buffer_size": 256 * 1024,
                "buffer_pool_max": 1024,
                "close_timeout_ms": 4000,
            },
        }.get(
            mode,
            {
                "buffer_size": 128 * 1024,
                "buffer_pool_max": 512,
                "close_timeout_ms": 3000,
            },
        ),
    }.get(
        backend,
        {
            "io_worker_count": min(platform_info["cpu_count"], 8),
            "buffer_size": 128 * 1024,
            "buffer_pool_max": 512,
            "close_timeout_ms": 4000,
        },
    )

    try:
        ayafileio.configure(tuning)
    except Exception as e:
        println(f"⚠️ 配置应用失败: {e}")


# ════════════════════════════════════════════════════════════════════════════
# 主函数
# ════════════════════════════════════════════════════════════════════════════


async def run_benchmark(config: Config) -> dict:
    sleep_stats = await calibrate_event_loop_latency()
    apply_smart_tuning(config)

    total_mb = (config.screenshot_size * config.screenshot_count) / (1024 * 1024)
    println(
        f"\n⚙️  测试配置: 截图 {config.screenshot_size // 1024}KB × {config.screenshot_count} = {total_mb:.1f}MB, "
        f"Dataset {config.item_count}条, 并发 {config.concurrent_limit}"
    )
    println("\n📦 准备测试数据...")
    println(
        f"✅ 生成了 {config.screenshot_count} 个模拟截图文件 (总计 {total_mb:.1f} MB)"
    )

    results: dict = {
        "platform": ayafileio.get_backend_info()["platform"],
        "backend": ayafileio.get_backend_info()["backend"],
        "is_truly_async": ayafileio.get_backend_info()["is_truly_async"],
        "tuning_mode": config.tuning_mode,
        "sleep_0_latency_us": sleep_stats,
        "benchmarks": {},
    }

    # 场景 1-4
    await run_scenario(
        "kvs_write",
        "场景 1：KeyValueStore 写入 (模拟截图/PDF 存储)",
        build_kvs_write,
        config,
        sleep_stats,
        results,
    )
    await run_scenario(
        "kvs_read",
        "场景 2：KeyValueStore 读取",
        build_kvs_read,
        config,
        sleep_stats,
        results,
    )

    # 场景 3 需要额外处理 latency
    println("\n" + "─" * 80)
    println("📊 场景 3：Dataset 追加写入 (详细延迟分析)")
    println("─" * 80)
    dataset_results = {}
    for lib in ["ayafileio", "aiofiles"]:
        times, all_latencies = [], []
        for rnd in range(config.test_rounds + config.warmup_rounds):
            bench_fn = await build_dataset_write(lib, config)
            # 闭包里捕获 latency — 需要从 bench_fn 里拿到
            # 这里保留原有的 dataset write 逻辑，因为它比较特殊
            latencies: list[float] = []

            async def _wrap():
                nonlocal latencies
                with tempfile.TemporaryDirectory() as tmpdir:
                    path = Path(tmpdir) / "dataset.jsonl"
                    items = [
                        {
                            "url": f"https://ex.com/{os.urandom(4).hex()}",
                            "title": os.urandom(8).hex(),
                            "ts": time.time(),
                            "data": os.urandom(config.item_size - 200).hex()[
                                : config.item_size - 200
                            ],
                        }
                        for _ in range(config.item_count)
                    ]
                    batch_size = len(items) // 10
                    batches = [
                        items[i : i + batch_size]
                        for i in range(0, len(items), batch_size)
                    ]
                    sem = asyncio.Semaphore(config.concurrent_limit)
                    if lib == "ayafileio":

                        async def wb(b):
                            async with sem:
                                async with ayafileio.open(
                                    path, "a", encoding="utf-8"
                                ) as f:
                                    for item in b:
                                        line = (
                                            json.dumps(item, ensure_ascii=False) + "\n"
                                        )
                                        t0 = time.perf_counter_ns()
                                        await f.write(line)
                                        latencies.append(
                                            (time.perf_counter_ns() - t0) / 1e9
                                        )
                    else:

                        async def wb(b):
                            async with sem:
                                async with aiofiles.open(
                                    path, "a", encoding="utf-8"
                                ) as f:
                                    for item in b:
                                        line = (
                                            json.dumps(item, ensure_ascii=False) + "\n"
                                        )
                                        t0 = time.perf_counter_ns()
                                        await f.write(line)
                                        latencies.append(
                                            (time.perf_counter_ns() - t0) / 1e9
                                        )

                    t0 = time.perf_counter()
                    await asyncio.gather(*[wb(b) for b in batches])
                    await asyncio.sleep(0.05)
                    return time.perf_counter() - t0

            elapsed = await _wrap()
            if rnd >= config.warmup_rounds:
                times.append(elapsed)
                all_latencies.extend(latencies)
                println(
                    f"  {lib:10} 第{rnd - config.warmup_rounds + 1}轮: {format_duration(elapsed)}"
                )
        stats = BenchmarkStats(name=f"{lib}_dataset", values=times)
        lat_stats = BenchmarkStats(name=f"{lib}_write_latency", values=all_latencies)
        tp = config.item_count / stats.median if stats.median > 0 else 0
        dataset_results[lib] = (stats, lat_stats, tp)
        print_latency_detail(lib, lat_stats, sleep_stats["median_us"])

    aya_ds, aya_lat, aya_tp = dataset_results["ayafileio"]
    aio_ds, aio_lat, aio_tp = dataset_results["aiofiles"]
    speedup = aio_ds.median / aya_ds.median if aya_ds.median > 0 else 0
    println(f"\n  📈 总体:")
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else "  (持平)")
    println(f"    吞吐量: ayafileio {aya_tp:.0f} 条/秒, aiofiles {aio_tp:.0f} 条/秒")
    results["benchmarks"]["dataset_write"] = {
        "ayafileio": aya_ds.to_dict(),
        "aiofiles": aio_ds.to_dict(),
        "speedup": speedup,
        "write_latency_ms": {
            "ayafileio": aya_lat.to_dict(),
            "aiofiles": aio_lat.to_dict(),
        },
    }

    # 场景 4
    await run_scenario(
        "mixed_workload", "场景 4：混合读写", build_mixed, config, sleep_stats, results
    )

    # 场景 5 — 降低并发避免 "Too many open files"
    storm_cfg = Config(
        test_rounds=config.test_rounds,
        warmup_rounds=config.warmup_rounds,
        concurrent_limit=20,  # macOS 上限
        enable_tuning=False,
    )
    await run_scenario(
        "tempfile_storm",
        "场景 5：临时文件风暴 (open-read-close, 无句柄复用)",
        build_tempfile_storm,
        storm_cfg,
        sleep_stats,
        results,
    )

    # 保存
    output_file = Path("benchmark_results_detailed.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    println(f"\n📁 详细结果已保存到: {output_file}")
    return results


def main():
    import argparse

    parser = argparse.ArgumentParser(description="ayafileio 性能基准测试")
    parser.add_argument(
        "--tuning",
        choices=["auto", "balanced", "throughput", "latency", "none"],
        default="balanced",
    )
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--items", type=int, default=5000)
    args = parser.parse_args()

    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    except Exception:
        pass

    config = Config(
        test_rounds=args.rounds,
        item_count=args.items,
        tuning_mode=args.tuning if args.tuning != "auto" else "balanced",
        enable_tuning=(args.tuning != "none"),
    )
    try:
        asyncio.run(run_benchmark(config))
    except KeyboardInterrupt:
        println("\n⚠️ 测试被用户中断")


if __name__ == "__main__":
    main()
