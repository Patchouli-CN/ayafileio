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
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

# 尝试导入 Rich（可选）
RICH_AVAILABLE = False
try:
    from rich.console import Console
    from rich.table import Table
    from rich.panel import Panel
    from rich import box
    RICH_AVAILABLE = True
    console = Console()
except ImportError:
    RICH_AVAILABLE = False
    console = None

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
# 配置
# ════════════════════════════════════════════════════════════════════════════

@dataclass
class Config:
    """测试配置"""
    screenshot_size: int = 512 * 1024
    screenshot_count: int = 100
    item_size: int = 1024
    item_count: int = 5000
    concurrent_limit: int = 50
    warmup_rounds: int = 2
    test_rounds: int = 5
    enable_tuning: bool = True
    tuning_mode: str = "balanced"


# ════════════════════════════════════════════════════════════════════════════
# 智能平台自适应调优
# ════════════════════════════════════════════════════════════════════════════

def get_platform_info() -> dict:
    """获取详细平台信息"""
    info = {
        "system": sys.platform,
        "is_linux": sys.platform == "linux",
        "is_windows": sys.platform == "win32",
        "is_macos": sys.platform == "darwin",
    }
    try:
        info["cpu_count"] = os.cpu_count() or 4
    except:
        info["cpu_count"] = 4
    info["is_ci"] = os.environ.get("CI", "").lower() in ("true", "1", "yes")
    info["is_github_actions"] = os.environ.get("GITHUB_ACTIONS", "").lower() == "true"
    return info


def apply_smart_tuning(config: Config):
    """智能平台自适应调优"""
    platform_info = get_platform_info()
    backend_info = ayafileio.get_backend_info()

    if RICH_AVAILABLE and console:
        console.print(Panel(
            f"[cyan]系统:[/cyan] {platform_info['system']}\n"
            f"[cyan]后端:[/cyan] {backend_info['backend']}\n"
            f"[cyan]CPU:[/cyan] {platform_info.get('cpu_count', '?')} 核心\n"
            f"[cyan]CI 环境:[/cyan] {platform_info['is_ci']}\n"
            f"[cyan]调优模式:[/cyan] {config.tuning_mode}",
            title="🔧 平台自适应调优",
            border_style="cyan"
        ))
    else:
        print(f"\n🔧 平台自适应调优:")
        print(f"   - 系统: {platform_info['system']}")
        print(f"   - 后端: {backend_info['backend']}")
        print(f"   - CPU: {platform_info.get('cpu_count', '?')} 核心")
        print(f"   - CI 环境: {platform_info['is_ci']}")
        print(f"   - 调优模式: {config.tuning_mode}")

    if config.tuning_mode == "none":
        return

    tuning_config = {}

    if backend_info["backend"] == "iocp":
        tuning_config = {
            "buffer_size": 512 * 1024, "buffer_pool_max": 1024, "close_timeout_ms": 3000,
        }
    elif backend_info["backend"] == "io_uring":
        if config.tuning_mode == "throughput":
            tuning_config = {
                "buffer_size": 256 * 1024, "buffer_pool_max": 1024,
                "io_uring_queue_depth": 512,
                "io_uring_sqpoll": True if not platform_info["is_ci"] else False,
                "close_timeout_ms": 4000,
            }
        elif config.tuning_mode == "latency":
            tuning_config = {
                "buffer_size": 64 * 1024, "buffer_pool_max": 512,
                "io_uring_queue_depth": 256, "io_uring_sqpoll": False,
                "close_timeout_ms": 2000,
            }
        else:
            tuning_config = {
                "buffer_size": 128 * 1024, "buffer_pool_max": 768,
                "io_uring_queue_depth": 384, "io_uring_sqpoll": False,
                "close_timeout_ms": 3000,
            }
    elif backend_info["backend"] == "dispatch_io":
        if config.tuning_mode == "throughput":
            tuning_config = {
                "buffer_size": 256 * 1024, "buffer_pool_max": 1024, "close_timeout_ms": 4000,
            }
        else:
            tuning_config = {
                "buffer_size": 128 * 1024, "buffer_pool_max": 512, "close_timeout_ms": 3000,
            }
    else:
        cpu_count = platform_info.get("cpu_count", 4)
        tuning_config = {
            "io_worker_count": min(cpu_count, 8),
            "buffer_size": 128 * 1024, "buffer_pool_max": 512, "close_timeout_ms": 4000,
        }

    if tuning_config:
        try:
            ayafileio.configure(tuning_config)
        except Exception as e:
            if RICH_AVAILABLE and console:
                console.print(f"[red]⚠️ 配置应用失败: {e}[/red]")
            else:
                print(f"⚠️ 配置应用失败: {e}")


# ════════════════════════════════════════════════════════════════════════════
# 统计数据类
# ════════════════════════════════════════════════════════════════════════════

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

def generate_screenshot_data(size: int) -> bytes:
    return os.urandom(size)


def generate_json_item(size: int) -> dict:
    item = {
        "url": f"https://example.com/page/{os.urandom(4).hex()}",
        "title": f"Page Title {os.urandom(8).hex()}",
        "timestamp": time.time(),
        "data": os.urandom(size - 200).hex()[:size - 200],
    }
    return item


def format_duration(seconds: float) -> str:
    if seconds < 0.000001:
        return f"{seconds * 1_000_000_000:.0f}ns"
    elif seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}μs"
    elif seconds < 1:
        return f"{seconds * 1000:.1f}ms"
    return f"{seconds:.3f}s"


def println(msg: str) -> None:
    """统一打印输出 — 纯文本，不含 Rich 标记"""
    if RICH_AVAILABLE and console:
        console.print(msg, markup=False)
    else:
        print(msg)


def println_rich(msg: str) -> None:
    """打印包含 Rich 标记的文本"""
    if RICH_AVAILABLE and console:
        console.print(msg)
    else:
        # 去掉 Rich 标记再打印
        import re
        clean = re.sub(r'\[/?[a-z_ ]+\]', '', msg)
        print(clean)


# ════════════════════════════════════════════════════════════════════════════
# 通用测试运行器
# ════════════════════════════════════════════════════════════════════════════

async def run_benchmark_rounds(
    name: str,
    lib: str,
    bench_fn: Callable[[], Awaitable[float]],
    config: Config,
) -> BenchmarkStats:
    """通用多轮测试：预热 → 正式 → 收集统计"""
    times = []
    for rnd in range(config.test_rounds + config.warmup_rounds):
        elapsed = await bench_fn()
        if rnd >= config.warmup_rounds:
            round_num = rnd - config.warmup_rounds + 1
            times.append(elapsed)
            println(f"  {lib:10} 第{round_num}轮: {format_duration(elapsed)}")
    return BenchmarkStats(name=f"{lib}_{name}", values=times)


def print_stats(
    lib: str,
    stats: BenchmarkStats,
    extra: str = "",
    sleep_median_us: float = 0,
) -> None:
    """统一打印延迟统计"""
    if RICH_AVAILABLE and console:
        color = "cyan" if lib == "ayafileio" else "dim"
        console.print(
            f"\n  📈 [{color}]{lib}[/{color}]: "
            f"中位数 {format_duration(stats.median)}, "
            f"抖动 {stats.jitter:.1f}%, "
            f"P99 {format_duration(stats.p99)}{extra}"
        )
    else:
        println(f"\n  📈 {lib}: "
                f"中位数 {format_duration(stats.median)}, "
                f"抖动 {stats.jitter:.1f}%, "
                f"P99 {format_duration(stats.p99)}{extra}")

    if lib == "ayafileio" and sleep_median_us > 0:
        ratio = sleep_median_us / (stats.median * 1_000_000)
        if ratio > 0.5:
            msg = f"      ⚡ 中位数 vs asyncio.sleep(0) 中位数({sleep_median_us:.1f}μs): {ratio:.1f}x"
            if RICH_AVAILABLE and console:
                console.print(f"      [bold yellow]{msg}[/bold yellow]")
            else:
                println(msg)


def print_latency_detail(lib: str, latency_stats: BenchmarkStats, sleep_median_us: float = 0) -> None:
    """打印单次 write 延迟详情"""
    if RICH_AVAILABLE and console:
        color = "cyan" if lib == "ayafileio" else "dim"
        console.print(f"\n  📝 [{color}]{lib}[/{color}] 单次 write 延迟:")
        console.print(f"      中位数: [green]{latency_stats.median * 1_000_000:.1f}μs[/green], "
                      f"P95: {latency_stats.p95 * 1_000_000:.1f}μs, "
                      f"P99: {latency_stats.p99 * 1_000_000:.1f}μs")
        console.print(f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x")
    else:
        println(f"\n  📝 {lib} 单次 write 延迟:")
        println(f"      中位数: {latency_stats.median * 1_000_000:.1f}μs, "
                f"P95: {latency_stats.p95 * 1_000_000:.1f}μs, "
                f"P99: {latency_stats.p99 * 1_000_000:.1f}μs")
        println(f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x")

    if lib == "ayafileio" and sleep_median_us > 0 and latency_stats.median > 0:
        ratio = sleep_median_us / (latency_stats.median * 1_000_000)
        if ratio > 0.5:
            msg = f"      ⚡ 单次 write 中位数 vs asyncio.sleep(0) 中位数({sleep_median_us:.1f}μs): {ratio:.1f}x"
            if RICH_AVAILABLE and console:
                console.print(f"      [bold yellow]{msg}[/bold yellow]")
            else:
                println(msg)


# ════════════════════════════════════════════════════════════════════════════
# 事件循环基准校准
# ════════════════════════════════════════════════════════════════════════════

async def calibrate_event_loop_latency() -> dict:
    """测量当前事件循环的最小可测延迟（asyncio.sleep(0) 精度）"""
    println("\n" + "─" * 80)
    println("📏 平台延迟基准: asyncio.sleep(0)")
    println("─" * 80)

    latencies = []
    for _ in range(1000):
        t0 = time.perf_counter()
        await asyncio.sleep(0)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1_000_000)  # 微秒

    stats = {
        "median_us": statistics.median(latencies),
        "p95_us": sorted(latencies)[int(len(latencies) * 0.95)],
        "p99_us": sorted(latencies)[int(len(latencies) * 0.99)],
        "min_us": min(latencies),
        "max_us": max(latencies),
    }

    println(f"  中位数: {stats['median_us']:.1f}μs, "
            f"P95: {stats['p95_us']:.1f}μs, "
            f"P99: {stats['p99_us']:.1f}μs, "
            f"最小: {stats['min_us']:.1f}μs")

    return stats


# ════════════════════════════════════════════════════════════════════════════
# 场景 1：KeyValueStore 写入
# ════════════════════════════════════════════════════════════════════════════

async def build_kvs_write_bench(lib: str, temp_dir: Path, data_list: list, config: Config):
    """构建一个适合传入 run_benchmark_rounds 的 bench 函数"""
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    total_size_mb = sum(len(d) for d in data_list) / (1024 * 1024)

    async def bench() -> float:
        if lib == "ayafileio":
            async def write_one(i: int, data: bytes):
                async with semaphore:
                    path = temp_dir / f"file_{i}.bin"
                    async with ayafileio.open(path, "wb") as f:
                        await f.write(data)
        else:
            async def write_one(i: int, data: bytes):
                async with semaphore:
                    path = temp_dir / f"file_{i}.bin"
                    async with aiofiles.open(path, "wb") as f:
                        await f.write(data)

        start = time.perf_counter()
        tasks = [write_one(i, data) for i, data in enumerate(data_list)]
        await asyncio.gather(*tasks)
        await asyncio.sleep(0.05)
        return time.perf_counter() - start

    return bench, total_size_mb


# ════════════════════════════════════════════════════════════════════════════
# 场景 2：KeyValueStore 读取
# ════════════════════════════════════════════════════════════════════════════

async def build_kvs_read_bench(lib: str, temp_dir: Path, count: int, config: Config):
    """构建读取 bench"""
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    total_size_mb = sum(
        (temp_dir / f"file_{i}.bin").stat().st_size for i in range(count)
    ) / (1024 * 1024)

    async def bench() -> float:
        if lib == "ayafileio":
            async def read_one(i: int):
                async with semaphore:
                    path = temp_dir / f"file_{i}.bin"
                    async with ayafileio.open(path, "rb") as f:
                        return await f.read()
        else:
            async def read_one(i: int):
                async with semaphore:
                    path = temp_dir / f"file_{i}.bin"
                    async with aiofiles.open(path, "rb") as f:
                        return await f.read()

        start = time.perf_counter()
        tasks = [read_one(i) for i in range(count)]
        await asyncio.gather(*tasks)
        await asyncio.sleep(0.05)
        return time.perf_counter() - start

    return bench, total_size_mb


# ════════════════════════════════════════════════════════════════════════════
# 场景 3：Dataset 追加写入
# ════════════════════════════════════════════════════════════════════════════

async def build_dataset_write_bench(lib: str, path: Path, items: list, config: Config):
    """构建 Dataset 写入 bench，同时收集单次 write 延迟"""
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    write_latencies: list[float] = []

    async def bench() -> float:
        nonlocal write_latencies
        write_latencies.clear()

        if lib == "ayafileio":
            async def write_batch(batch: list):
                async with semaphore:
                    async with ayafileio.open(path, "a", encoding="utf-8") as f:
                        for item in batch:
                            line = json.dumps(item, ensure_ascii=False) + "\n"
                            w_start = time.perf_counter_ns()
                            await f.write(line)
                            write_latencies.append((time.perf_counter_ns() - w_start) / 1e9)
        else:
            async def write_batch(batch: list):
                async with semaphore:
                    async with aiofiles.open(path, "a", encoding="utf-8") as f:
                        for item in batch:
                            line = json.dumps(item, ensure_ascii=False) + "\n"
                            w_start = time.perf_counter_ns()
                            await f.write(line)
                            write_latencies.append((time.perf_counter_ns() - w_start) / 1e9)

        batch_size = len(items) // 10
        batches = [items[i:i + batch_size] for i in range(0, len(items), batch_size)]

        start = time.perf_counter()
        await asyncio.gather(*[write_batch(batch) for batch in batches])
        await asyncio.sleep(0.05)
        return time.perf_counter() - start

    return bench, write_latencies


# ════════════════════════════════════════════════════════════════════════════
# 场景 4：混合读写
# ════════════════════════════════════════════════════════════════════════════

async def build_mixed_workload_bench(lib: str, temp_dir: Path, read_count: int, write_data: list, config: Config):
    """构建混合读写 bench"""
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    import random

    async def bench() -> float:
        if lib == "ayafileio":
            async def read_one(i: int):
                async with semaphore:
                    path = temp_dir / f"existing_{i}.bin"
                    async with ayafileio.open(path, "rb") as f:
                        return await f.read()

            async def write_one(i: int, data: bytes):
                async with semaphore:
                    path = temp_dir / f"mixed_{i}.bin"
                    async with ayafileio.open(path, "wb") as f:
                        await f.write(data)
        else:
            async def read_one(i: int):
                async with semaphore:
                    path = temp_dir / f"existing_{i}.bin"
                    async with aiofiles.open(path, "rb") as f:
                        return await f.read()

            async def write_one(i: int, data: bytes):
                async with semaphore:
                    path = temp_dir / f"mixed_{i}.bin"
                    async with aiofiles.open(path, "wb") as f:
                        await f.write(data)

        all_tasks = [read_one(i) for i in range(read_count)] + \
                    [write_one(i, data) for i, data in enumerate(write_data)]
        random.shuffle(all_tasks)

        start = time.perf_counter()
        await asyncio.gather(*all_tasks)
        await asyncio.sleep(0.05)
        return time.perf_counter() - start

    return bench


# ════════════════════════════════════════════════════════════════════════════
# 主基准运行函数
# ════════════════════════════════════════════════════════════════════════════

async def run_benchmark(config: Config) -> dict:
    """运行完整基准测试"""

    # ── 平台延迟基准 ──
    sleep_stats = await calibrate_event_loop_latency()

    # ── 应用调优 ──
    apply_smart_tuning(config)

    # ── 准备测试数据 ──
    total_size_mb = (config.screenshot_size * config.screenshot_count) / (1024 * 1024)
    println(f"\n⚙️  测试配置: "
            f"截图 {config.screenshot_size // 1024}KB × {config.screenshot_count} = {total_size_mb:.1f}MB, "
            f"Dataset {config.item_count}条, 并发 {config.concurrent_limit}")

    println("\n📦 准备测试数据...")
    screenshot_data = [generate_screenshot_data(config.screenshot_size) for _ in range(config.screenshot_count)]
    dataset_items = [generate_json_item(config.item_size) for _ in range(config.item_count)]
    existing_count = 50
    existing_data = [generate_screenshot_data(config.screenshot_size) for _ in range(existing_count)]
    println(f"✅ 生成了 {config.screenshot_count} 个模拟截图文件 (总计 {total_size_mb:.1f} MB)")

    results = {
        "platform": ayafileio.get_backend_info()["platform"],
        "backend": ayafileio.get_backend_info()["backend"],
        "is_truly_async": ayafileio.get_backend_info()["is_truly_async"],
        "tuning_mode": config.tuning_mode,
        "sleep_0_latency_us": sleep_stats,
        "benchmarks": {}
    }

    # ── 场景 1：KeyValueStore 写入 ──
    println("\n" + "─" * 80)
    println("📊 场景 1：KeyValueStore 写入 (模拟截图/PDF 存储)")
    println("─" * 80)

    kvs_write = {}
    for lib in ["ayafileio", "aiofiles"]:
        with tempfile.TemporaryDirectory() as tmpdir:
            bench_fn, _ = await build_kvs_write_bench(lib, Path(tmpdir), screenshot_data, config)
            stats = await run_benchmark_rounds("kvs_write", lib, bench_fn, config)
            kvs_write[lib] = stats
            print_stats(lib, stats, sleep_median_us=sleep_stats["median_us"])

    aya_w = kvs_write["ayafileio"]
    aio_w = kvs_write["aiofiles"]
    speedup = aio_w.median / aya_w.median if aya_w.median > 0 else 0
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else f"  (持平)")
    results["benchmarks"]["kvs_write"] = {
        "ayafileio": aya_w.to_dict(),
        "aiofiles": aio_w.to_dict(),
        "speedup": speedup,
    }

    # ── 场景 2：KeyValueStore 读取 ──
    println("\n" + "─" * 80)
    println("📊 场景 2：KeyValueStore 读取")
    println("─" * 80)

    kvs_read = {}
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        for i, data in enumerate(screenshot_data):
            (tmp_path / f"file_{i}.bin").write_bytes(data)

        for lib in ["ayafileio", "aiofiles"]:
            bench_fn, _ = await build_kvs_read_bench(lib, tmp_path, config.screenshot_count, config)
            stats = await run_benchmark_rounds("kvs_read", lib, bench_fn, config)
            kvs_read[lib] = stats
            print_stats(lib, stats, sleep_median_us=sleep_stats["median_us"])

    aya_r = kvs_read["ayafileio"]
    aio_r = kvs_read["aiofiles"]
    speedup = aio_r.median / aya_r.median if aya_r.median > 0 else 0
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else f"  (持平)")
    results["benchmarks"]["kvs_read"] = {
        "ayafileio": aya_r.to_dict(),
        "aiofiles": aio_r.to_dict(),
        "speedup": speedup,
    }

    # ── 场景 3：Dataset 追加写入 ──
    println("\n" + "─" * 80)
    println("📊 场景 3：Dataset 追加写入 (详细延迟分析)")
    println("─" * 80)

    dataset_results = {}
    for lib in ["ayafileio", "aiofiles"]:
        times = []
        all_latencies = []
        for rnd in range(config.test_rounds + config.warmup_rounds):
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp_path = Path(tmpdir) / "dataset.jsonl"
                bench_fn, latencies = await build_dataset_write_bench(lib, tmp_path, dataset_items, config)
                elapsed = await bench_fn()
                if rnd >= config.warmup_rounds:
                    times.append(elapsed)
                    all_latencies.extend(latencies)
                    println(f"  {lib:10} 第{rnd - config.warmup_rounds + 1}轮: {format_duration(elapsed)}")

        stats = BenchmarkStats(name=f"{lib}_dataset", values=times)
        latency_stats = BenchmarkStats(name=f"{lib}_write_latency", values=all_latencies)
        throughput = config.item_count / stats.median if stats.median > 0 else 0
        dataset_results[lib] = (stats, latency_stats, throughput)

        print_latency_detail(lib, latency_stats, sleep_stats["median_us"])

    aya_stats, aya_lat, aya_tp = dataset_results["ayafileio"]
    aio_stats, aio_lat, aio_tp = dataset_results["aiofiles"]
    speedup = aio_stats.median / aya_stats.median if aya_stats.median > 0 else 0
    println(f"\n  📈 总体:")
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else f"  (持平)")
    println(f"    吞吐量: ayafileio {aya_tp:.0f} 条/秒, aiofiles {aio_tp:.0f} 条/秒")
    results["benchmarks"]["dataset_write"] = {
        "ayafileio": aya_stats.to_dict(),
        "aiofiles": aio_stats.to_dict(),
        "speedup": speedup,
        "write_latency_ms": {"ayafileio": aya_lat.to_dict(), "aiofiles": aio_lat.to_dict()},
    }

    # ── 场景 4：混合读写 ──
    println("\n" + "─" * 80)
    println("📊 场景 4：混合读写")
    println("─" * 80)

    mixed = {}
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        for i, data in enumerate(existing_data):
            (tmp_path / f"existing_{i}.bin").write_bytes(data)

        for lib in ["ayafileio", "aiofiles"]:
            bench_fn = await build_mixed_workload_bench(lib, tmp_path, existing_count, screenshot_data[:30], config)
            stats = await run_benchmark_rounds("mixed", lib, bench_fn, config)
            mixed[lib] = stats
            print_stats(lib, stats, sleep_median_us=sleep_stats["median_us"])

    aya_m = mixed["ayafileio"]
    aio_m = mixed["aiofiles"]
    speedup = aio_m.median / aya_m.median if aya_m.median > 0 else 0
    println(f"  🚀 提速: {speedup:.2f}x" if speedup > 1 else f"  (持平)")
    results["benchmarks"]["mixed_workload"] = {
        "ayafileio": aya_m.to_dict(),
        "aiofiles": aio_m.to_dict(),
        "speedup": speedup,
    }

    # ── 保存结果 ──
    output_file = Path("benchmark_results_detailed.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    println(f"\n📁 详细结果已保存到: {output_file}")

    return results


def main():
    """主函数"""
    import argparse

    parser = argparse.ArgumentParser(description="ayafileio 性能基准测试")
    parser.add_argument("--tuning", choices=["auto", "balanced", "throughput", "latency", "none"],
                        default="balanced", help="调优模式 (默认: balanced)")
    parser.add_argument("--rounds", type=int, default=5, help="测试轮数")
    parser.add_argument("--items", type=int, default=5000, help="Dataset 条目数")

    args = parser.parse_args()
    tuning_mode = args.tuning if args.tuning != "auto" else "balanced"

    if sys.platform == "win32":
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    else:
        try:
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
        except Exception:
            pass

    config = Config(
        test_rounds=args.rounds,
        item_count=args.items,
        tuning_mode=tuning_mode,
        enable_tuning=(tuning_mode != "none")
    )

    try:
        asyncio.run(run_benchmark(config))
    except KeyboardInterrupt:
        println("\n⚠️ 测试被用户中断")


if __name__ == "__main__":
    main()