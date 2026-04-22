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
from pathlib import Path
from dataclasses import dataclass, field

if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

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
    # KeyValueStore 场景：模拟截图/PDF 存储
    screenshot_size: int = 512 * 1024      # 512KB
    screenshot_count: int = 100
    
    # Dataset 场景：模拟爬取结果追加写入
    item_size: int = 1024                  # 1KB
    item_count: int = 5000
    
    # 并发配置
    concurrent_limit: int = 50
    
    # 测试轮数
    warmup_rounds: int = 2
    test_rounds: int = 5
    
    # 是否启用 ayafileio 性能调优
    enable_tuning: bool = True
    
    # 调优模式: "auto", "aggressive", "conservative", "none"
    tuning_mode: str = "auto"


# ════════════════════════════════════════════════════════════════════════════
# 平台自适应调优
# ════════════════════════════════════════════════════════════════════════════

def get_platform_info() -> dict:
    """获取详细平台信息"""
    info = {
        "system": sys.platform,
        "is_linux": sys.platform == "linux",
        "is_windows": sys.platform == "win32",
        "is_macos": sys.platform == "darwin",
    }
    
    # 获取 CPU 核心数
    try:
        info["cpu_count"] = os.cpu_count() or 4
    except:
        info["cpu_count"] = 4
    
    # 检测内存大小（粗略）
    try:
        if sys.platform == "linux":
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if line.startswith("MemTotal"):
                        info["mem_kb"] = int(line.split()[1])
                        break
        elif sys.platform == "win32":
            import ctypes
            kernel32 = ctypes.windll.kernel32
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]
            memoryStatus = MEMORYSTATUSEX()
            memoryStatus.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            if kernel32.GlobalMemoryStatusEx(ctypes.byref(memoryStatus)):
                info["mem_bytes"] = memoryStatus.ullTotalPhys
        elif sys.platform == "darwin":
            import subprocess
            result = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True)
            if result.returncode == 0:
                info["mem_bytes"] = int(result.stdout.strip())
    except:
        pass
    
    return info


def apply_platform_tuning(config: Config):
    """根据平台和调优模式应用最优配置"""
    platform_info = get_platform_info()
    backend_info = ayafileio.get_backend_info()
    
    print(f"\n🔧 平台自适应调优:")
    print(f"   - 系统: {platform_info['system']}")
    print(f"   - 后端: {backend_info['backend']}")
    print(f"   - CPU: {platform_info.get('cpu_count', '?')} 核心")
    
    tuning_config = {}
    
    # 根据调优模式决定策略
    if config.tuning_mode == "none":
        print("   - 调优模式: 无 (使用默认配置)")
        return
    
    if config.tuning_mode == "aggressive":
        print("   - 调优模式: 激进 (追求极致性能)")
        # 激进模式：最大化缓冲区
        tuning_config["buffer_size"] = 1024 * 1024  # 1MB
        tuning_config["buffer_pool_max"] = 2048
        tuning_config["close_timeout_ms"] = 5000
        
        if platform_info["is_linux"]:
            tuning_config["io_uring_queue_depth"] = 1024
            tuning_config["io_uring_sqpoll"] = True
        
    elif config.tuning_mode == "conservative":
        print("   - 调优模式: 保守 (稳定性优先)")
        tuning_config["buffer_size"] = 64 * 1024
        tuning_config["buffer_pool_max"] = 256
        tuning_config["close_timeout_ms"] = 4000
        
    else:  # "auto" - 自动选择
        print("   - 调优模式: 自动")
        
        # 根据后端类型自动调优
        if backend_info["backend"] == "iocp":
            # Windows IOCP: 大缓冲区 + 高并发
            tuning_config["buffer_size"] = 512 * 1024  # 512KB
            tuning_config["buffer_pool_max"] = 1024
            tuning_config["close_timeout_ms"] = 3000
            print("     - Windows IOCP: 大缓冲区模式 (512KB)")
            
        elif backend_info["backend"] == "io_uring":
            # Linux io_uring: 中等缓冲区 + 大队列
            tuning_config["buffer_size"] = 256 * 1024  # 256KB
            tuning_config["buffer_pool_max"] = 1024
            tuning_config["io_uring_queue_depth"] = 512
            tuning_config["io_uring_sqpoll"] = False  # CI 环境不建议开启
            tuning_config["close_timeout_ms"] = 4000
            print("     - Linux io_uring: 批量提交模式 (队列深度=512)")
            
        elif backend_info["backend"] == "dispatch_io":
            # macOS Dispatch I/O: 中等缓冲区
            tuning_config["buffer_size"] = 256 * 1024
            tuning_config["buffer_pool_max"] = 512
            tuning_config["close_timeout_ms"] = 4000
            print("     - macOS Dispatch I/O: 标准模式")
            
        else:  # thread_pool
            # 线程池降级模式
            cpu_count = platform_info.get("cpu_count", 4)
            tuning_config["io_worker_count"] = min(cpu_count * 2, 16)
            tuning_config["buffer_size"] = 128 * 1024
            tuning_config["buffer_pool_max"] = 512
            print(f"     - 线程池模式: worker数={tuning_config['io_worker_count']}")
    
    # 应用配置
    if tuning_config:
        try:
            ayafileio.configure(tuning_config)
            print(f"   ✅ 已应用配置: {tuning_config}")
        except Exception as e:
            print(f"   ⚠️ 配置应用失败: {e}")


def apply_ayafileio_tuning(config: Config):
    """应用 ayafileio 性能调优配置（兼容旧接口）"""
    if not config.enable_tuning:
        print("  ⚙️  ayafileio 使用默认配置")
        return
    
    # 使用平台自适应调优
    apply_platform_tuning(config)


# ════════════════════════════════════════════════════════════════════════════
# 统计数据类
# ════════════════════════════════════════════════════════════════════════════

@dataclass
class BenchmarkStats:
    """基准测试统计数据"""
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
        """抖动系数 = 标准差 / 平均值，衡量稳定性"""
        return (self.stdev / self.mean * 100) if self.mean > 0 else 0
    
    @property
    def range_ratio(self) -> float:
        """极差比 = 最大值 / 最小值，衡量波动幅度"""
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
            "raw_values": self.values,
        }


@dataclass
class ComparisonResult:
    """对比结果"""
    library: str
    stats: BenchmarkStats
    throughput: float = 0  # MB/s 或 items/s
    
    def speedup_vs(self, other: "ComparisonResult") -> float:
        """相对于另一个库的提速倍数"""
        return other.stats.mean / self.stats.mean if self.stats.mean > 0 else 0


# ════════════════════════════════════════════════════════════════════════════
# 辅助函数
# ════════════════════════════════════════════════════════════════════════════

def generate_screenshot_data(size: int) -> bytes:
    """生成模拟截图数据"""
    return os.urandom(size)


def generate_json_item(size: int) -> dict:
    """生成模拟爬取结果的 JSON 数据"""
    item = {
        "url": f"https://example.com/page/{os.urandom(4).hex()}",
        "title": f"Page Title {os.urandom(8).hex()}",
        "timestamp": time.time(),
        "data": os.urandom(size - 200).hex()[:size - 200],
    }
    return item


# ════════════════════════════════════════════════════════════════════════════
# 场景 1：KeyValueStore 写入
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_kvs_write(
    library: str,
    temp_dir: Path,
    data_list: list,
    config: Config,
) -> float:
    """执行一次 KeyValueStore 写入测试"""
    semaphore = asyncio.Semaphore(config.concurrent_limit)
    
    if library == "ayafileio":
        async def write_one(i: int, data: bytes):
            async with semaphore:
                path = temp_dir / f"file_{i}.bin"
                async with ayafileio.open(path, "wb") as f:
                    await f.write(data)
    else:  # aiofiles
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
    """执行一次 KeyValueStore 读取测试"""
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
# 场景 3：Dataset 追加写入（记录每次 write 的延迟）
# ════════════════════════════════════════════════════════════════════════════

async def benchmark_dataset_write_detailed(
    library: str,
    path: Path,
    items: list,
    config: Config,
) -> tuple[float, list[float]]:
    """
    执行 Dataset 追加写入测试，返回总时间和每次 write 的延迟
    """
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
                        write_latencies.append((w_end - w_start) * 1000)  # 转为毫秒
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


async def benchmark_dataset_write_simple(
    library: str,
    path: Path,
    items: list,
    config: Config,
) -> float:
    """执行 Dataset 追加写入测试（只返回总时间）"""
    elapsed, _ = await benchmark_dataset_write_detailed(library, path, items, config)
    return elapsed


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
    """执行一次混合读写测试"""
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
    print("=" * 80)
    print("ayafileio vs aiofiles 专业基准测试")
    print("包含延迟分布、抖动分析、配置调优")
    print("=" * 80)
    
    # 显示后端信息
    info = ayafileio.get_backend_info()
    print(f"\n📌 当前平台: {info['platform']}")
    print(f"📌 ayafileio 后端: {info['backend']} (真异步: {info['is_truly_async']})")
    print(f"📌 aiofiles: 线程池模拟异步 (假异步)")
    
    # 应用性能调优（使用新的平台自适应调优）
    apply_ayafileio_tuning(config)
    
    print(f"\n⚙️  测试配置:")
    print(f"   - 截图文件大小: {config.screenshot_size // 1024} KB")
    print(f"   - 截图文件数量: {config.screenshot_count}")
    print(f"   - Dataset 每条大小: ~{config.item_size} bytes")
    print(f"   - Dataset 条数: {config.item_count}")
    print(f"   - 最大并发: {config.concurrent_limit}")
    print(f"   - 预热轮数: {config.warmup_rounds}")
    print(f"   - 测试轮数: {config.test_rounds}")
    print(f"   - 调优模式: {config.tuning_mode}")
    
    # 准备测试数据
    print("\n📦 准备测试数据...")
    
    screenshot_data = [generate_screenshot_data(config.screenshot_size) 
                       for _ in range(config.screenshot_count)]
    total_size_mb = (config.screenshot_size * config.screenshot_count) / (1024 * 1024)
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
                    print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
        
        stats = BenchmarkStats(name=f"{lib}_kvs_write", values=times)
        throughput = total_size_mb / stats.mean if stats.mean > 0 else 0
        kvs_write_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
    
    aya_write = kvs_write_results["ayafileio"]
    aio_write = kvs_write_results["aiofiles"]
    speedup = aya_write.speedup_vs(aio_write)
    
    print(f"\n  📈 ayafileio: 中位数 {aya_write.stats.median:.4f}s, 抖动 {aya_write.stats.jitter:.1f}%, P99 {aya_write.stats.p99:.4f}s")
    print(f"  📈 aiofiles:  中位数 {aio_write.stats.median:.4f}s, 抖动 {aio_write.stats.jitter:.1f}%, P99 {aio_write.stats.p99:.4f}s")
    print(f"  🚀 提速: {speedup:.2f}x" + (" ✅" if speedup > 1 else ""))
    print(f"  📊 稳定性: ayafileio 抖动 {aya_write.stats.jitter:.1f}% vs aiofiles {aio_write.stats.jitter:.1f}%")
    
    results["benchmarks"]["kvs_write"] = {
        "ayafileio": aya_write.stats.to_dict(),
        "aiofiles": aio_write.stats.to_dict(),
        "speedup": speedup,
        "throughput_aya_mbps": aya_write.throughput,
        "throughput_aio_mbps": aio_write.throughput,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 2：KeyValueStore 读取
    # ════════════════════════════════════════════════════════════════════════
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
                    print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
            
            stats = BenchmarkStats(name=f"{lib}_kvs_read", values=times)
            throughput = total_size_mb / stats.mean if stats.mean > 0 else 0
            kvs_read_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
    
    aya_read = kvs_read_results["ayafileio"]
    aio_read = kvs_read_results["aiofiles"]
    speedup_read = aya_read.speedup_vs(aio_read)
    
    print(f"\n  📈 ayafileio: 中位数 {aya_read.stats.median:.4f}s, 抖动 {aya_read.stats.jitter:.1f}%, P99 {aya_read.stats.p99:.4f}s")
    print(f"  📈 aiofiles:  中位数 {aio_read.stats.median:.4f}s, 抖动 {aio_read.stats.jitter:.1f}%, P99 {aio_read.stats.p99:.4f}s")
    print(f"  🚀 提速: {speedup_read:.2f}x" + (" ✅" if speedup_read > 1 else ""))
    
    results["benchmarks"]["kvs_read"] = {
        "ayafileio": aya_read.stats.to_dict(),
        "aiofiles": aio_read.stats.to_dict(),
        "speedup": speedup_read,
        "throughput_aya_mbps": aya_read.throughput,
        "throughput_aio_mbps": aio_read.throughput,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 场景 3：Dataset 追加写入（详细延迟分析）
    # ════════════════════════════════════════════════════════════════════════
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
                    print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
        
        stats = BenchmarkStats(name=f"{lib}_dataset", values=times)
        latency_stats = BenchmarkStats(name=f"{lib}_write_latency_ms", values=all_latencies)
        throughput = config.item_count / stats.mean if stats.mean > 0 else 0
        
        dataset_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=throughput)
        write_latencies[lib] = latency_stats
        
        print(f"\n  📝 {lib} 单次 write 延迟 (毫秒):")
        print(f"      中位数: {latency_stats.median:.3f}ms, P95: {latency_stats.p95:.3f}ms, P99: {latency_stats.p99:.3f}ms")
        print(f"      抖动: {latency_stats.jitter:.1f}%, 极差比: {latency_stats.range_ratio:.1f}x")
    
    aya_dataset = dataset_results["ayafileio"]
    aio_dataset = dataset_results["aiofiles"]
    speedup_dataset = aya_dataset.speedup_vs(aio_dataset)
    
    print(f"\n  📈 总体:")
    print(f"  🚀 提速: {speedup_dataset:.2f}x" + (" ✅" if speedup_dataset > 1 else ""))
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
                    print(f"  {lib:10} 第{round_num - config.warmup_rounds + 1}轮: {elapsed:.4f}s")
            
            stats = BenchmarkStats(name=f"{lib}_mixed", values=times)
            mixed_results[lib] = ComparisonResult(library=lib, stats=stats, throughput=0)
    
    aya_mixed = mixed_results["ayafileio"]
    aio_mixed = mixed_results["aiofiles"]
    speedup_mixed = aya_mixed.speedup_vs(aio_mixed)
    
    print(f"\n  📈 ayafileio: 中位数 {aya_mixed.stats.median:.4f}s, 抖动 {aya_mixed.stats.jitter:.1f}%")
    print(f"  📈 aiofiles:  中位数 {aio_mixed.stats.median:.4f}s, 抖动 {aio_mixed.stats.jitter:.1f}%")
    print(f"  🚀 提速: {speedup_mixed:.2f}x" + (" ✅" if speedup_mixed > 1 else ""))
    
    results["benchmarks"]["mixed_workload"] = {
        "ayafileio": aya_mixed.stats.to_dict(),
        "aiofiles": aio_mixed.stats.to_dict(),
        "speedup": speedup_mixed,
    }
    
    # ════════════════════════════════════════════════════════════════════════
    # 汇总表格
    # ════════════════════════════════════════════════════════════════════════
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
│ KeyValueStore 写入       │ 中位数 (s)        │ {aya_write.stats.median:8.4f} │ {aio_write.stats.median:8.4f} │ {speedup:5.2f}x     │
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
    print(f"\n📁 详细结果已保存到: {output_file}")
    
    return results


def main():
    """主函数"""
    import argparse
    
    parser = argparse.ArgumentParser(description="ayafileio 性能基准测试")
    parser.add_argument("--tuning", choices=["auto", "aggressive", "conservative", "none"],
                        default="auto", help="调优模式 (默认: auto)")
    parser.add_argument("--rounds", type=int, default=5, help="测试轮数")
    parser.add_argument("--items", type=int, default=5000, help="Dataset 条目数")
    
    args = parser.parse_args()
    
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
        tuning_mode=args.tuning,
        enable_tuning=(args.tuning != "none")
    )
    
    try:
        results = asyncio.run(run_benchmark(config))
        return results
    except KeyboardInterrupt:
        print("\n\n⚠️  测试被用户中断")
        return None


if __name__ == "__main__":
    main()