#!/usr/bin/env python3
"""
ayafileio vs aiofiles 专业基准测试
包含延迟分布、抖动分析、配置调优对比
"""

import asyncio
import tempfile
import time
import json
import os
import io
import sys
import statistics
import platform
from pathlib import Path
from dataclasses import dataclass, field

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
    tuning_mode: str = "auto"  # auto, balanced, throughput, latency, none


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
    
    # 检测是否为 CI 环境
    info["is_ci"] = os.environ.get("CI", "").lower() in ("true", "1", "yes")
    info["is_github_actions"] = os.environ.get("GITHUB_ACTIONS", "").lower() == "true"
    
    return info


def apply_smart_tuning(config: Config):
    """智能平台自适应调优 - 基于实际测试数据优化"""
    platform_info = get_platform_info()
    backend_info = ayafileio.get_backend_info()
    
    if console:
        console.print(Panel(
            f"[cyan]系统:[/cyan] {platform_info['system']}\n"
            f"[cyan]后端:[/cyan] {backend_info['backend']}\n"
            f"[cyan]CPU:[/cyan] {platform_info.get('cpu_count', '?')} 核心\n"
            f"[cyan]CI环境:[/cyan] {platform_info['is_ci']}\n"
            f"[cyan]调优模式:[/cyan] {config.tuning_mode}",
            title="🔧 平台自适应调优",
            border_style="cyan"
        ))
    else:
        print(f"\n🔧 平台自适应调优:")
        print(f"   - 系统: {platform_info['system']}")
        print(f"   - 后端: {backend_info['backend']}")
        print(f"   - CPU: {platform_info.get('cpu_count', '?')} 核心")
    
    tuning_config = {}
    
    if config.tuning_mode == "none":
        if console:
            console.print("[dim]调优模式: 无 (使用默认配置)[/dim]")
        return
    
    # 根据后端类型和平台选择最优配置
    if backend_info["backend"] == "iocp":
        # Windows IOCP - 最佳性能配置
        tuning_config = {
            "buffer_size": 512 * 1024,
            "buffer_pool_max": 1024,
            "close_timeout_ms": 3000,
        }
        if console:
            console.print("[green]✓ Windows IOCP: 大缓冲区模式 (512KB)[/green]")
    
    elif backend_info["backend"] == "io_uring":
        # Linux io_uring - 平衡配置
        if config.tuning_mode == "throughput":
            tuning_config = {
                "buffer_size": 256 * 1024,
                "buffer_pool_max": 1024,
                "io_uring_queue_depth": 512,
                "io_uring_sqpoll": True if not platform_info["is_ci"] else False,
                "close_timeout_ms": 4000,
            }
            if console:
                console.print("[green]✓ Linux io_uring: 吞吐优先模式[/green]")
        elif config.tuning_mode == "latency":
            tuning_config = {
                "buffer_size": 64 * 1024,
                "buffer_pool_max": 512,
                "io_uring_queue_depth": 256,
                "io_uring_sqpoll": False,
                "close_timeout_ms": 2000,
            }
            if console:
                console.print("[green]✓ Linux io_uring: 延迟优先模式[/green]")
        else:  # balanced
            tuning_config = {
                "buffer_size": 128 * 1024,
                "buffer_pool_max": 768,
                "io_uring_queue_depth": 384,
                "io_uring_sqpoll": False,
                "close_timeout_ms": 3000,
            }
            if console:
                console.print("[green]✓ Linux io_uring: 平衡模式[/green]")
    
    elif backend_info["backend"] == "dispatch_io":
        # macOS Dispatch I/O - 保守配置
        if config.tuning_mode == "throughput":
            tuning_config = {
                "buffer_size": 256 * 1024,
                "buffer_pool_max": 1024,
                "close_timeout_ms": 4000,
            }
        else:
            tuning_config = {
                "buffer_size": 128 * 1024,
                "buffer_pool_max": 512,
                "close_timeout_ms": 3000,
            }
        if console:
            console.print("[green]✓ macOS Dispatch I/O: 标准模式[/green]")
    
    else:  # thread_pool
        cpu_count = platform_info.get("cpu_count", 4)
        tuning_config = {
            "io_worker_count": min(cpu_count, 8),
            "buffer_size": 128 * 1024,
            "buffer_pool_max": 512,
            "close_timeout_ms": 4000,
        }
        if console:
            console.print(f"[green]✓ 线程池模式: worker数={tuning_config['io_worker_count']}[/green]")
    
    # 应用配置
    if tuning_config:
        try:
            ayafileio.configure(tuning_config)
            if console:
                config_str = ", ".join(f"{k}={v}" for k, v in tuning_config.items())
                console.print(f"[dim]   配置: {config_str}[/dim]")
        except Exception as e:
            if console:
                console.print(f"[red]⚠️ 配置应用失败: {e}[/red]")


def apply_ayafileio_tuning(config: Config):
    """应用 ayafileio 性能调优"""
    if not config.enable_tuning:
        if console:
            console.print("[dim]  ⚙️  使用默认配置[/dim]")
        return
    
    apply_smart_tuning(config)


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
        sorted_vals = sorted(self.values)
        idx = int(len(sorted_vals) * 0.95)
        return sorted_vals[min(idx, len(sorted_vals) - 1)]
    
    @property
    def p99(self) -> float:
        if not self.values:
            return 0
        sorted_vals = sorted(self.values)
        idx = int(len(sorted_vals) * 0.99)
        return sorted_vals[min(idx, len(sorted_vals) - 1)]
    
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
    
    def get_color(self) -> str:
        if self.mean < 0.05:
            return "green"
        elif self.mean < 0.1:
            return "yellow"
        return "red"


@dataclass
class ComparisonResult:
    library: str
    stats: BenchmarkStats
    throughput: float = 0
    
    def speedup_vs(self, other: "ComparisonResult") -> float:
        return other.stats.mean / self.stats.mean if self.stats.mean > 0 else 0


# ════════════════════════════════════════════════════════════════════════════
# 辅助函数
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
    if seconds < 0.001:
        return f"{seconds * 1000:.2f}ms"
    elif seconds < 1:
        return f"{seconds * 1000:.1f}ms"
    return f"{seconds:.3f}s"


# ════════════════════════════════════════════════════════════════════════════
# 场景 1：KeyValueStore 写入
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_kvs_write(
    library: str,
    temp_dir: Path,
    data_list: list,
    config: Config,
) -> float:
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    
    if library == "ayafileio":
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
    elapsed = time.perf_counter() - start
    
    await asyncio.sleep(0.05)
    return elapsed


# ════════════════════════════════════════════════════════════════════════════
# 场景 2：KeyValueStore 读取
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_kvs_read(
    library: str,
    temp_dir: Path,
    count: int,
    config: Config,
) -> float:
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    
    if library == "ayafileio":
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
    elapsed = time.perf_counter() - start
    
    await asyncio.sleep(0.05)
    return elapsed


# ════════════════════════════════════════════════════════════════════════════
# 场景 3：Dataset 追加写入
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_dataset_write_detailed(
    library: str,
    path: Path,
    items: list,
    config: Config,
) -> tuple[float, list[float]]:
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    write_latencies: list[float] = []
    
    if library == "ayafileio":
        async def write_batch(batch: list):
            async with semaphore:
                async with ayafileio.open(path, "a", encoding="utf-8") as f:
                    for item in batch:
                        line = json.dumps(item, ensure_ascii=False) + "\n"
                        w_start = time.perf_counter()
                        await f.write(line)
                        w_end = time.perf_counter()
                        write_latencies.append((w_end - w_start) * 1000)
    else:
        async def write_batch(batch: list):
            async with semaphore:
                async with aiofiles.open(path, "a", encoding="utf-8") as f:
                    for item in batch:
                        line = json.dumps(item, ensure_ascii=False) + "\n"
                        w_start = time.perf_counter()
                        await f.write(line)
                        w_end = time.perf_counter()
                        write_latencies.append((w_end - w_start) * 1000)
    
    batch_size = len(items) // 10
    batches = [items[i:i+batch_size] for i in range(0, len(items), batch_size)]
    
    start = time.perf_counter()
    await asyncio.gather(*[write_batch(batch) for batch in batches])
    elapsed = time.perf_counter() - start
    
    await asyncio.sleep(0.05)
    return elapsed, write_latencies


# ════════════════════════════════════════════════════════════════════════════
# 场景 4：混合读写
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_mixed_workload(
    library: str,
    temp_dir: Path,
    read_count: int,
    write_data: list,
    config: Config,
) -> float:
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    
    if library == "ayafileio":
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
    
    read_tasks = [read_one(i) for i in range(read_count)]
    write_tasks = [write_one(i, data) for i, data in enumerate(write_data)]
    all_tasks = read_tasks + write_tasks
    
    import random
    random.shuffle(all_tasks)
    
    start = time.perf_counter()
    await asyncio.gather(*all_tasks)
    elapsed = time.perf_counter() - start
    
    await asyncio.sleep(0.05)
    return elapsed


# ════════════════════════════════════════════════════════════════════════════
# 运行完整基准测试
# ════════════════════════════════════════════════════════════════════════════

async def run_benchmark(config: Config) -> dict:
    """运行完整基准测试"""
    # 显示后端信息
    info = ayafileio.get_backend_info()
    
    # 应用性能调优
    apply_ayafileio_tuning(config)
    
    # 打印测试配置
    total_size_mb = (config.screenshot_size * config.screenshot_count) / (1024 * 1024)
    
    if console:
        console.print(f"\n[bold]⚙️  测试配置:[/bold]")
        console.print(f"   - 截图: {config.screenshot_count} 个 x {config.screenshot_size // 1024}KB = {total_size_mb:.1f}MB")
        console.print(f"   - Dataset: {config.item_count} 条 x ~{config.item_size} bytes")
        console.print(f"   - 并发: {config.concurrent_limit} | 轮数: {config.test_rounds}")
    else:
        print(f"\n⚙️  测试配置:")
        print(f"   - 截图文件大小: {config.screenshot_size // 1024} KB")
        print(f"   - 截图文件数量: {config.screenshot_count}")
        print(f"   - Dataset 每条大小: ~{config.item_size} bytes")
        print(f"   - Dataset 条数: {config.item_count}")
        print(f"   - 最大并发: {config.concurrent_limit}")
        print(f"   - 预热轮数: {config.warmup_rounds}")
        print(f"   - 测试轮数: {config.test_rounds}")
    
    # 准备测试数据
    if console:
        console.print("\n[bold cyan]📦 准备测试数据...[/bold cyan]")
    else:
        print("\n📦 准备测试数据...")
    
    screenshot_data = [generate_screenshot_data(config.screenshot_size) 
                       for _ in range(config.screenshot_count)]
    
    if console:
        console.print(f"[green]✅ 生成了 {config.screenshot_count} 个模拟截图文件 (总计 {total_size_mb:.1f} MB)[/green]")
    else:
        print(f"   生成了 {config.screenshot_count} 个模拟截图文件 (总计 {total_size_mb:.1f} MB)")
    
    dataset_items = [generate_json_item(config.item_size) 
                     for _ in range(config.item_count)]
    
    existing_count = 50
    existing_data = [generate_screenshot_data(config.screenshot_size) 
                     for _ in range(existing_count)]
    
    results = {
        "platform": info["platform"],
        "backend": info["backend"],
        "is_truly_async": info["is_truly_async"],
        "tuning_enabled": config.enable_tuning,
        "tuning_mode": config.tuning_mode,
        "config": {
            "screenshot_size_kb": config.screenshot_size // 1024,
            "screenshot_count": config.screenshot_count,
            "item_size_bytes": config.item_size,
            "item_count": config.item_count,
            "concurrent_limit": config.concurrent_limit,
            "warmup_rounds": config.warmup_rounds,
            "test_rounds": config.test_rounds,
        },
        "benchmarks": {}
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 1：KeyValueStore 写入
    # ════════════════════════════════════════════════════════════════════════
    if console:
        console.print("\n[bold cyan]📊 场景 1：KeyValueStore 写入[/bold cyan]")
        console.print("[dim]模拟截图/PDF 存储[/dim]")
    else:
        print("\n" + "─" * 80)
        print("📊 场景 1：KeyValueStore 写入 (模拟截图/PDF 存储)")
        print("─" * 80)
    
    kvs_write_results = {}
    
    for lib in ["ayafileio", "aiofiles"]:
        times = []
        for round_num in range(config.test_rounds + config.warmup_rounds):
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp_path = Path(tmpdir)
                elapsed = await benchmark_kvs_write(lib, tmp_path, screenshot_data, config)
                if round_num >= config.warmup_rounds:
                    times.append(elapsed)
                    if console:
                        console.print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {format_duration(elapsed)}")
                    else:
                        print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
        
        stats = BenchmarkStats(name=f"{lib}_kvs_write", values=times)
        throughput = total_size_mb / stats.mean if stats.mean > 0 else 0
        kvs_write_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
    
    aya_write = kvs_write_results["ayafileio"]
    aio_write = kvs_write_results["aiofiles"]
    speedup_write = aya_write.speedup_vs(aio_write)
    
    if console:
        console.print(f"\n  📈 [cyan]ayafileio[/cyan]: 中位数 {format_duration(aya_write.stats.median)}, 抖动 {aya_write.stats.jitter:.1f}%, P99 {format_duration(aya_write.stats.p99)}")
        console.print(f"  📈 [dim]aiofiles[/dim]:   中位数 {format_duration(aio_write.stats.median)}, 抖动 {aio_write.stats.jitter:.1f}%, P99 {format_duration(aio_write.stats.p99)}")
        if speedup_write > 1:
            console.print(f"  🚀 [green]提速: {speedup_write:.2f}x[/green]")
        else:
            console.print(f"  📉 [red]减速: {speedup_write:.2f}x[/red]")
    else:
        print(f"\n  📈 ayafileio: 中位数 {aya_write.stats.median:.4f}s, 抖动 {aya_write.stats.jitter:.1f}%, P99 {aya_write.stats.p99:.4f}s")
        print(f"  📈 aiofiles:  中位数 {aio_write.stats.median:.4f}s, 抖动 {aio_write.stats.jitter:.1f}%, P99 {aio_write.stats.p99:.4f}s")
        print(f"  🚀 提速: {speedup_write:.2f}x")
    
    results["benchmarks"]["kvs_write"] = {
        "ayafileio": aya_write.stats.to_dict(),
        "aiofiles": aio_write.stats.to_dict(),
        "speedup": speedup_write,
        "throughput_aya_mbps": aya_write.throughput,
        "throughput_aio_mbps": aio_write.throughput,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 2：KeyValueStore 读取
    # ════════════════════════════════════════════════════════════════════════
    if console:
        console.print("\n[bold cyan]📊 场景 2：KeyValueStore 读取[/bold cyan]")
    else:
        print("\n" + "─" * 80)
        print("📊 场景 2：KeyValueStore 读取")
        print("─" * 80)
    
    kvs_read_results = {}
    
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        for i, data in enumerate(screenshot_data):
            (tmp_path / f"file_{i}.bin").write_bytes(data)
        
        for lib in ["ayafileio", "aiofiles"]:
            times = []
            for round_num in range(config.test_rounds + config.warmup_rounds):
                elapsed = await benchmark_kvs_read(lib, tmp_path, config.screenshot_count, config)
                if round_num >= config.warmup_rounds:
                    times.append(elapsed)
                    if console:
                        console.print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {format_duration(elapsed)}")
                    else:
                        print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
            
            stats = BenchmarkStats(name=f"{lib}_kvs_read", values=times)
            throughput = total_size_mb / stats.mean if stats.mean > 0 else 0
            kvs_read_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
    
    aya_read = kvs_read_results["ayafileio"]
    aio_read = kvs_read_results["aiofiles"]
    speedup_read = aya_read.speedup_vs(aio_read)
    
    if console:
        console.print(f"\n  📈 [cyan]ayafileio[/cyan]: 中位数 {format_duration(aya_read.stats.median)}, 抖动 {aya_read.stats.jitter:.1f}%, P99 {format_duration(aya_read.stats.p99)}")
        console.print(f"  📈 [dim]aiofiles[/dim]:   中位数 {format_duration(aio_read.stats.median)}, 抖动 {aio_read.stats.jitter:.1f}%, P99 {format_duration(aio_read.stats.p99)}")
        if speedup_read > 1:
            console.print(f"  🚀 [green]提速: {speedup_read:.2f}x[/green]")
        else:
            console.print(f"  📉 [red]减速: {speedup_read:.2f}x[/red]")
    else:
        print(f"\n  📈 ayafileio: 中位数 {aya_read.stats.median:.4f}s, 抖动 {aya_read.stats.jitter:.1f}%, P99 {aya_read.stats.p99:.4f}s")
        print(f"  📈 aiofiles:  中位数 {aio_read.stats.median:.4f}s, 抖动 {aio_read.stats.jitter:.1f}%, P99 {aio_read.stats.p99:.4f}s")
        print(f"  🚀 提速: {speedup_read:.2f}x")
    
    results["benchmarks"]["kvs_read"] = {
        "ayafileio": aya_read.stats.to_dict(),
        "aiofiles": aio_read.stats.to_dict(),
        "speedup": speedup_read,
        "throughput_aya_mbps": aya_read.throughput,
        "throughput_aio_mbps": aio_read.throughput,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 3：Dataset 追加写入
    # ════════════════════════════════════════════════════════════════════════
    if console:
        console.print("\n[bold cyan]📊 场景 3：Dataset 追加写入[/bold cyan]")
        console.print("[dim]详细延迟分析[/dim]")
    else:
        print("\n" + "─" * 80)
        print("📊 场景 3：Dataset 追加写入 (详细延迟分析)")
        print("─" * 80)
    
    dataset_results = {}
    write_latencies = {}
    
    for lib in ["ayafileio", "aiofiles"]:
        times = []
        all_latencies = []
        
        for round_num in range(config.test_rounds + config.warmup_rounds):
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp_path = Path(tmpdir) / "dataset.jsonl"
                elapsed, latencies = await benchmark_dataset_write_detailed(lib, tmp_path, dataset_items, config)
                if round_num >= config.warmup_rounds:
                    times.append(elapsed)
                    all_latencies.extend(latencies)
                    if console:
                        console.print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {format_duration(elapsed)}")
                    else:
                        print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
        
        stats = BenchmarkStats(name=f"{lib}_dataset", values=times)
        latency_stats = BenchmarkStats(name=f"{lib}_write_latency_ms", values=all_latencies)
        throughput = config.item_count / stats.mean if stats.mean > 0 else 0
        
        dataset_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
        write_latencies[lib] = latency_stats
        
        if console:
            console.print(f"\n  📝 [cyan]{lib}[/cyan] 单次 write 延迟:")
            console.print(f"      中位数: [green]{latency_stats.median:.3f}ms[/green], P95: {latency_stats.p95:.3f}ms, P99: {latency_stats.p99:.3f}ms")
            console.print(f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x")
        else:
            print(f"\n  📝 {lib} 单次 write 延迟 (毫秒):")
            print(f"      中位数: {latency_stats.median:.3f}ms, P95: {latency_stats.p95:.3f}ms, P99: {latency_stats.p99:.3f}ms")
            print(f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x")
    
    aya_dataset = dataset_results["ayafileio"]
    aio_dataset = dataset_results["aiofiles"]
    speedup_dataset = aya_dataset.speedup_vs(aio_dataset)
    
    if console:
        console.print(f"\n  📈 [cyan]总体:[/cyan]")
        if speedup_dataset > 1:
            console.print(f"  🚀 [green]提速: {speedup_dataset:.2f}x[/green]")
        else:
            console.print(f"  📉 [red]减速: {speedup_dataset:.2f}x[/red]")
        console.print(f"  📝 吞吐量: [green]{aya_dataset.throughput:.0f}[/green] 条/秒 vs {aio_dataset.throughput:.0f} 条/秒")
    else:
        print(f"\n  📈 总体:")
        print(f"  🚀 提速: {speedup_dataset:.2f}x")
        print(f"  📝 吞吐量: ayafileio {aya_dataset.throughput:.0f} 条/秒, aiofiles {aio_dataset.throughput:.0f} 条/秒")
    
    results["benchmarks"]["dataset_write"] = {
        "ayafileio": aya_dataset.stats.to_dict(),
        "aiofiles": aio_dataset.stats.to_dict(),
        "speedup": speedup_dataset,
        "items_per_sec_aya": aya_dataset.throughput,
        "items_per_sec_aio": aio_dataset.throughput,
        "write_latency_ms": {
            "ayafileio": write_latencies["ayafileio"].to_dict(),
            "aiofiles": write_latencies["aiofiles"].to_dict(),
        }
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 4：混合读写
    # ════════════════════════════════════════════════════════════════════════
    if console:
        console.print("\n[bold cyan]📊 场景 4：混合读写[/bold cyan]")
    else:
        print("\n" + "─" * 80)
        print("📊 场景 4：混合读写")
        print("─" * 80)
    
    mixed_results = {}
    
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        for i, data in enumerate(existing_data):
            (tmp_path / f"existing_{i}.bin").write_bytes(data)
        
        for lib in ["ayafileio", "aiofiles"]:
            times = []
            for round_num in range(config.test_rounds + config.warmup_rounds):
                elapsed = await benchmark_mixed_workload(
                    lib, tmp_path, existing_count, screenshot_data[:30], config
                )
                if round_num >= config.warmup_rounds:
                    times.append(elapsed)
                    if console:
                        console.print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {format_duration(elapsed)}")
                    else:
                        print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
            
            stats = BenchmarkStats(name=f"{lib}_mixed", values=times)
            mixed_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=0)
    
    aya_mixed = mixed_results["ayafileio"]
    aio_mixed = mixed_results["aiofiles"]
    speedup_mixed = aya_mixed.speedup_vs(aio_mixed)
    
    if console:
        console.print(f"\n  📈 [cyan]ayafileio[/cyan]: 中位数 {format_duration(aya_mixed.stats.median)}, 抖动 {aya_mixed.stats.jitter:.1f}%")
        console.print(f"  📈 [dim]aiofiles[/dim]:   中位数 {format_duration(aio_mixed.stats.median)}, 抖动 {aio_mixed.stats.jitter:.1f}%")
        if speedup_mixed > 1:
            console.print(f"  🚀 [green]提速: {speedup_mixed:.2f}x[/green]")
        else:
            console.print(f"  📉 [red]减速: {speedup_mixed:.2f}x[/red]")
    else:
        print(f"\n  📈 ayafileio: 中位数 {aya_mixed.stats.median:.4f}s, 抖动 {aya_mixed.stats.jitter:.1f}%")
        print(f"  📈 aiofiles:  中位数 {aio_mixed.stats.median:.4f}s, 抖动 {aio_mixed.stats.jitter:.1f}%")
        print(f"  🚀 提速: {speedup_mixed:.2f}x")
    
    results["benchmarks"]["mixed_workload"] = {
        "ayafileio": aya_mixed.stats.to_dict(),
        "aiofiles": aio_mixed.stats.to_dict(),
        "speedup": speedup_mixed,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 汇总输出
    # ════════════════════════════════════════════════════════════════════════
    if console and RICH_AVAILABLE:
        console.print("\n")
        table = Table(title="📊 测试结果对比", box=box.ROUNDED, header_style="bold cyan")
        table.add_column("场景", style="cyan", no_wrap=True)
        table.add_column("指标", style="dim")
        table.add_column("ayafileio", justify="right")
        table.add_column("aiofiles", justify="right")
        table.add_column("对比", justify="center")
        
        table.add_row("KeyValueStore\n写入", "中位数", format_duration(aya_write.stats.median), format_duration(aio_write.stats.median), f"{speedup_write:.2f}x")
        table.add_row("", "抖动", f"{aya_write.stats.jitter:.1f}%", f"{aio_write.stats.jitter:.1f}%", "✅" if aya_write.stats.jitter < aio_write.stats.jitter else "❌")
        table.add_row("", "P99", format_duration(aya_write.stats.p99), format_duration(aio_write.stats.p99), "")
        table.add_row("", "", "", "", "")
        table.add_row("KeyValueStore\n读取", "中位数", format_duration(aya_read.stats.median), format_duration(aio_read.stats.median), f"{speedup_read:.2f}x")
        table.add_row("", "抖动", f"{aya_read.stats.jitter:.1f}%", f"{aio_read.stats.jitter:.1f}%", "✅" if aya_read.stats.jitter < aio_read.stats.jitter else "❌")
        table.add_row("", "", "", "", "")
        table.add_row("Dataset\n追加写入", "中位数", format_duration(aya_dataset.stats.median), format_duration(aio_dataset.stats.median), f"{speedup_dataset:.2f}x")
        table.add_row("", "吞吐量", f"{aya_dataset.throughput:.0f}条/秒", f"{aio_dataset.throughput:.0f}条/秒", f"{speedup_dataset:.2f}x")
        table.add_row("", "P99延迟", f"{write_latencies['ayafileio'].p99:.3f}ms", f"{write_latencies['aiofiles'].p99:.3f}ms", "")
        table.add_row("", "", "", "", "")
        table.add_row("混合读写", "中位数", format_duration(aya_mixed.stats.median), format_duration(aio_mixed.stats.median), f"{speedup_mixed:.2f}x")
        table.add_row("", "抖动", f"{aya_mixed.stats.jitter:.1f}%", f"{aio_mixed.stats.jitter:.1f}%", "✅" if aya_mixed.stats.jitter < aio_mixed.stats.jitter else "❌")
        
        console.print(table)
    else:
        print("\n" + "=" * 90)
        print("📊 测试结果汇总")
        print("=" * 90)
        print(f"""
┌───────────────────────────────────────────────────────────────────────────────────────┐
│                                    测试结果对比                                         │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ 平台: {info['platform']:<10}  ayafileio 后端: {info['backend']:<12}  真异步: {info['is_truly_async']} │
│ 调优: {config.tuning_mode} 模式                                                         │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ 场景                     │ 指标              │ ayafileio  │ aiofiles   │ 对比         │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ KeyValueStore 写入       │ 中位数 (s)        │ {aya_write.stats.median:8.4f} │ {aio_write.stats.median:8.4f} │ {speedup_write:5.2f}x     │
│                          │ 抖动 (%)          │ {aya_write.stats.jitter:8.1f} │ {aio_write.stats.jitter:8.1f} │ {'✅ 更稳' if aya_write.stats.jitter < aio_write.stats.jitter else '❌'}      │
│                          │ P99 (s)           │ {aya_write.stats.p99:8.4f} │ {aio_write.stats.p99:8.4f} │            │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ KeyValueStore 读取       │ 中位数 (s)        │ {aya_read.stats.median:8.4f} │ {aio_read.stats.median:8.4f} │ {speedup_read:5.2f}x     │
│                          │ 抖动 (%)          │ {aya_read.stats.jitter:8.1f} │ {aio_read.stats.jitter:8.1f} │ {'✅ 更稳' if aya_read.stats.jitter < aio_read.stats.jitter else '❌'}      │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ Dataset 追加写入         │ 中位数 (s)        │ {aya_dataset.stats.median:8.4f} │ {aio_dataset.stats.median:8.4f} │ {speedup_dataset:5.2f}x     │
│                          │ 吞吐量 (条/秒)    │ {aya_dataset.throughput:8.0f} │ {aio_dataset.throughput:8.0f} │ {speedup_dataset:5.2f}x     │
│                          │ 单次write P99(ms) │ {write_latencies['ayafileio'].p99:8.3f} │ {write_latencies['aiofiles'].p99:8.3f} │            │
├───────────────────────────────────────────────────────────────────────────────────────┤
│ 混合读写                 │ 中位数 (s)        │ {aya_mixed.stats.median:8.4f} │ {aio_mixed.stats.median:8.4f} │ {speedup_mixed:5.2f}x     │
└───────────────────────────────────────────────────────────────────────────────────────┘
""")
    
    # 保存结果
    output_file = Path("benchmark_results_detailed.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    
    if console:
        console.print(f"\n[dim]📁 详细结果已保存到: {output_file}[/dim]")
    else:
        print(f"\n📁 详细结果已保存到: {output_file}")
    
    return results


def main():
    """主函数"""
    import argparse
    
    parser = argparse.ArgumentParser(description="ayafileio 性能基准测试")
    parser.add_argument("--tuning", choices=["auto", "balanced", "throughput", "latency", "none"],
                        default="balanced", help="调优模式 (默认: balanced)")
    parser.add_argument("--rounds", type=int, default=5, help="测试轮数")
    parser.add_argument("--items", type=int, default=5000, help="Dataset 条目数")
    parser.add_argument("--no-rich", action="store_true", help="禁用 Rich 美化输出")
    
    args = parser.parse_args()
    
    # 映射调优模式
    tuning_mode = args.tuning
    if tuning_mode == "auto":
        tuning_mode = "balanced"
    
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
        results = asyncio.run(run_benchmark(config))
        return results
    except KeyboardInterrupt:
        if console and RICH_AVAILABLE:
            console.print("\n\n[yellow]⚠️ 测试被用户中断[/yellow]")
        else:
            print("\n\n⚠️  测试被用户中断")
        return None


if __name__ == "__main__":
    main()