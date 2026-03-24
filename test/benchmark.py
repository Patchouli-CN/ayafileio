"""核心测试逻辑"""
import asyncio
import os
import time
import random
import psutil
from pathlib import Path

from .config import BenchmarkConfig, DEFAULT_CONFIG
from .metrics import PerformanceMetrics
import statistics

try:
    from rich.progress import Progress, SpinnerColumn, BarColumn, TimeElapsedColumn, TimeRemainingColumn
    RICH_PROGRESS_AVAILABLE = True
except ImportError:
    RICH_PROGRESS_AVAILABLE = False

try:
    import ayafileio
    # 性能调优可以加这个，让测试结果更准确
    ayafileio.set_handle_pool_limits(512, 16384)
except ImportError:
    ayafileio = None

try:
    import aiofiles
except ImportError:
    aiofiles = None


class ServerBenchmark:
    """服务器级性能测试"""
    
    def __init__(self, test_dir: Path, config: BenchmarkConfig | None = None):
        self.test_dir = test_dir
        self.config = config or DEFAULT_CONFIG
        
        # 确保依赖可用
        self.ayafileio_available = ayafileio is not None
        self.aiofiles_available = aiofiles is not None
        
        # 进度回调
        self.progress_callback = None
    
    def set_progress_callback(self, callback):
        """设置进度回调函数"""
        self.progress_callback = callback
    
    def _get_resource_usage(self) -> tuple[float, float, int, int]:
        """获取当前资源使用情况"""
        try:
            process = psutil.Process()
            cpu = process.cpu_percent(interval=0.1)
            memory = process.memory_info().rss / 1024 / 1024
            threads = process.num_threads()
            
            try:
                import ctypes
                from ctypes import wintypes
                kernel32 = ctypes.windll.kernel32
                handle_count = wintypes.ULONG()
                if kernel32.GetProcessHandleCount(process._handle, ctypes.byref(handle_count)): # type: ignore
                    handle_count = handle_count.value
                else:
                    handle_count = 0
            except:
                handle_count = 0
                
            return cpu, memory, threads, handle_count
        except:
            return 0.0, 0.0, 0, 0
    
    async def _warmup(self, file_paths: list[str]):
        """预热：读取文件到缓存"""
        for path in file_paths:
            try:
                with open(path, 'rb') as f:
                    f.read(1024 * 1024)
            except:
                pass
    
    async def _clear_system_cache(self):
        """清理系统缓存（需要管理员权限）"""
        if not self.config.clear_cache:
            return
        
        if os.name == 'nt':  # Windows
            try:
                import ctypes
                kernel32 = ctypes.windll.kernel32
                kernel32.SetSystemFileCacheSize(-1, -1, 0)
            except:
                pass
    
    async def test_ayafileio(self, file_paths: list[str], num_clients: int) -> PerformanceMetrics:
        """测试 ayafileio - 真正的异步"""
        if not self.ayafileio_available:
            return PerformanceMetrics(name="ayafileio", completed=False)
        
        metrics = PerformanceMetrics()
        metrics.name = "ayafileio (IOCP真异步)"
        metrics.concurrent_clients = num_clients
        stop_event = asyncio.Event()
        
        read_ratio, write_ratio = self.config.read_write_ratio
        
        async def client_worker(client_id: int):
            local_ops = 0
            local_bytes = 0
            local_errors = 0
            local_latencies = []
            file_handles = {}
            
            while not stop_event.is_set():
                try:
                    file_path = random.choice(file_paths)
                    op_type = random.choices(
                        ['read', 'write'],
                        weights=[read_ratio, write_ratio]
                    )[0]
                    
                    start_time = time.perf_counter()
                    
                    if op_type == 'read':
                        if file_path not in file_handles:
                            file_handles[file_path] = ayafileio.open(file_path, 'rb')
                        f = file_handles[file_path]
                        
                        # 更大范围的随机偏移
                        max_offset = 500 * 1024 * 1024  # 500MB 范围
                        offset = random.randint(0, max_offset - 64*1024)
                        await f.seek(offset)
                        data = await f.read(64 * 1024)
                        local_bytes += len(data)
                    else:
                        # 写入临时文件，避免影响读取测试
                        write_path = self.test_dir / f"write_{client_id}_{random.randint(1,10000)}.tmp"
                        async with ayafileio.open(str(write_path), 'wb') as f:
                            data = os.urandom(4096)
                            await f.write(data)
                            local_bytes += len(data)
                    
                    latency = time.perf_counter() - start_time
                    local_latencies.append(latency)
                    local_ops += 1
                    
                    # 随机休眠模拟真实场景
                    await asyncio.sleep(random.uniform(0, 0.002))
                    
                except Exception as e:
                    local_errors += 1
            
            return local_ops, local_bytes, local_errors, local_latencies
        
        # 启动客户端
        clients = [asyncio.create_task(client_worker(i)) for i in range(num_clients)]
        
        # 运行指定时间
        await asyncio.sleep(self.config.duration_seconds)
        stop_event.set()
        
        # 收集结果
        results = await asyncio.gather(*clients)
        metrics.total_operations = sum(r[0] for r in results)
        metrics.total_bytes = sum(r[1] for r in results)
        metrics.error_count = sum(r[2] for r in results)
        for r in results:
            metrics.raw_latencies.extend(r[3])
        
        metrics.total_time = self.config.duration_seconds
        metrics.completed = True
        
        # 计算统计
        metrics.calculate_percentiles()
        
        # 资源使用
        cpu, mem, threads, handles = self._get_resource_usage()
        metrics.cpu_usage = cpu
        metrics.memory_usage = mem
        metrics.thread_count = threads
        metrics.handle_count = handles
        
        return metrics
    
    async def test_aiofiles(self, file_paths: list[str], num_clients: int) -> PerformanceMetrics:
        """测试 aiofiles - 相同并发数，带超时"""
        if not self.aiofiles_available:
            return PerformanceMetrics(name="aiofiles", completed=False)
        
        metrics = PerformanceMetrics()
        metrics.name = "aiofiles (线程池模拟)"
        metrics.concurrent_clients = num_clients
        
        try:
            async with asyncio.timeout(self.config.timeout_seconds):
                result = await self._run_aiofiles_test(file_paths, num_clients)
                metrics = result
                metrics.completed = True
        except asyncio.TimeoutError:
            metrics.completed = False
            metrics.error_count = num_clients * 100
        except Exception as e:
            metrics.completed = False
        
        # 资源使用
        cpu, mem, threads, handles = self._get_resource_usage()
        metrics.cpu_usage = cpu
        metrics.memory_usage = mem
        metrics.thread_count = threads
        metrics.handle_count = handles
        
        return metrics
    
    async def _run_aiofiles_test(self, file_paths: list[str], num_clients: int) -> PerformanceMetrics:
        """实际运行 aiofiles 测试"""
        metrics = PerformanceMetrics()
        metrics.name = "aiofiles (线程池模拟)"
        metrics.concurrent_clients = num_clients
        stop_event = asyncio.Event()
        
        read_ratio, write_ratio = self.config.read_write_ratio
        
        async def client_worker(client_id: int):
            local_ops = 0
            local_bytes = 0
            local_errors = 0
            local_latencies = []
            
            while not stop_event.is_set():
                try:
                    file_path = random.choice(file_paths)
                    op_type = random.choices(
                        ['read', 'write'],
                        weights=[read_ratio, write_ratio]
                    )[0]
                    
                    start_time = time.perf_counter()
                    
                    if op_type == 'read':
                        async with aiofiles.open(file_path, 'rb') as f:
                            max_offset = 500 * 1024 * 1024
                            offset = random.randint(0, max_offset - 64*1024)
                            await f.seek(offset)
                            data = await f.read(64 * 1024)
                            local_bytes += len(data)
                    else:
                        write_path = self.test_dir / f"aio_write_{client_id}_{random.randint(1,10000)}.tmp"
                        async with aiofiles.open(str(write_path), 'wb') as f:
                            data = os.urandom(4096)
                            await f.write(data)
                            local_bytes += len(data)
                    
                    latency = time.perf_counter() - start_time
                    local_latencies.append(latency)
                    local_ops += 1
                    await asyncio.sleep(random.uniform(0, 0.002))
                    
                except Exception as e:
                    local_errors += 1
            
            return local_ops, local_bytes, local_errors, local_latencies
        
        # 启动客户端
        clients = [asyncio.create_task(client_worker(i)) for i in range(num_clients)]
        
        # 运行指定时间
        await asyncio.sleep(self.config.duration_seconds)
        stop_event.set()
        
        # 收集结果
        try:
            results = await asyncio.wait_for(asyncio.gather(*clients), timeout=5.0)
        except asyncio.TimeoutError:
            for c in clients:
                c.cancel()
            results = [(0, 0, 0, []) for _ in clients]
        
        metrics.total_operations = sum(r[0] for r in results)
        metrics.total_bytes = sum(r[1] for r in results)
        metrics.error_count = sum(r[2] for r in results)
        for r in results:
            metrics.raw_latencies.extend(r[3])
        
        metrics.total_time = self.config.duration_seconds
        metrics.calculate_percentiles()
        
        return metrics
    
    async def prepare_test_data(self, console=None):
        """准备测试数据 - 修复版"""
        if console:
            console.print("\n[bold cyan]📦 准备测试数据...[/bold cyan]")
        
        file_paths = []
        
        # 确保测试目录存在
        self.test_dir.mkdir(parents=True, exist_ok=True)
        
        file_list = []
        for name, size in self.config.file_sizes.items():
            for i in range(self.config.num_files_per_size):
                path = self.test_dir / f"{name}_{i}.dat"
                file_list.append((path, size))

        if console and RICH_PROGRESS_AVAILABLE:
            progress = Progress(
                SpinnerColumn(),
                "[progress.description]{task.description}",
                BarColumn(),
                "[progress.percentage]{task.percentage:>3.0f}%",
                TimeElapsedColumn(),
                TimeRemainingColumn(),
                console=console,
            )
            progress_task = progress.add_task("正在准备测试数据", total=len(file_list))
            progress.start()
        else:
            progress = None
            progress_task = None

        for path, size in file_list:
                # 检查文件是否存在且大小正确
                need_create = True
                if path.exists():
                    actual_size = path.stat().st_size
                    if actual_size >= size:
                        need_create = False
                        if console:
                            console.print(f"   [dim]使用现有文件: {path.name} ({size//1024//1024}MB)[/dim]")
                    else:
                        if console:
                            console.print(f"   [yellow]文件 {path.name} 大小不匹配，重新创建...[/yellow]")
                        path.unlink()
                
                if need_create:
                    if console:
                        console.print(f"   创建 [green]{path.name}[/green] ({size//1024//1024}MB)...")
                    
                    # 使用大块写入加快速度
                    chunk_size = 10 * 1024 * 1024  # 10MB 块
                    with open(path, 'wb') as f:
                        # 生成一次随机数据并重复使用，提高效率
                        chunk = os.urandom(chunk_size)
                        written = 0
                        while written < size:
                            write_size = min(chunk_size, size - written)
                            f.write(chunk[:write_size])
                            written += write_size
                
                # 确保文件路径被添加（无论是否新创建）
                file_paths.append(str(path))
                if progress is not None:
                    progress.update(progress_task, advance=1)

        if progress is not None:
            progress.stop()
            progress.refresh()

        # 统计
        total_size = sum(self.config.file_sizes.values()) * self.config.num_files_per_size
        if console:
            console.print(f"[green]✅ 准备完成: {len(file_paths)} 个文件, 总大小: {total_size//1024//1024}MB[/green]")
        
        # 预热
        if self.config.warmup_enabled:
            if console:
                console.print("[dim]🔥 预热中...[/dim]")
            await self._warmup(file_paths)
            if console:
                console.print("[dim]✅ 预热完成[/dim]")
        
        return file_paths
    
    async def run_fair_comparison(self, file_paths: list[str], client_counts: list[int] = None, console=None) -> list[PerformanceMetrics]:
        """运行公平对比测试"""
        if client_counts is None:
            client_counts = self.config.client_counts
        
        results = []

        if console and RICH_PROGRESS_AVAILABLE:
            progress = Progress(
                SpinnerColumn(),
                "[progress.description]{task.description}",
                BarColumn(),
                "[progress.percentage]{task.percentage:>3.0f}%",
                TimeElapsedColumn(),
                TimeRemainingColumn(),
                console=console,
            )
            task = progress.add_task("正在执行公平对比测试", total=len(client_counts) * 2)
            progress.start()
        else:
            progress = None
            task = None

        for clients in client_counts:
            # ayafileio 多次运行并聚合
            win_runs = []
            for i in range(self.config.repeats):
                if progress is not None:
                    progress.update(task, description=f"ayafileio {clients}并发 第{i+1}/{self.config.repeats} 次")
                win_runs.append(await self.test_ayafileio(file_paths, clients))
                await asyncio.sleep(0.5)
            win_metrics = self._aggregate_metrics(win_runs)
            results.append(win_metrics)
            if progress is not None:
                progress.update(task, advance=1)

            # aiofiles 多次运行并聚合
            aio_runs = []
            for i in range(self.config.repeats):
                if progress is not None:
                    progress.update(task, description=f"aiofiles {clients}并发 第{i+1}/{self.config.repeats} 次")
                aio_runs.append(await self.test_aiofiles(file_paths, clients))
                await asyncio.sleep(0.5)
            aio_metrics = self._aggregate_metrics(aio_runs)
            results.append(aio_metrics)
            if progress is not None:
                progress.update(task, advance=1)

            # 如果 aiofiles 在较大并发下持续不完成，停止测试
            if not aio_metrics.completed and clients >= 100:
                break

        if progress is not None:
            progress.stop()
            progress.refresh()

        return results

    def _aggregate_metrics(self, metrics_list: list[PerformanceMetrics]) -> PerformanceMetrics:
        """将多次运行的结果聚合为一个稳定的指标，剔除两侧异常值并取中位数/中位延迟等。"""
        if not metrics_list:
            return PerformanceMetrics(completed=False)

        # 优先使用已完成的运行
        completed = [m for m in metrics_list if m.completed]
        use = completed if completed else metrics_list

        # 聚合基本数值使用中位数
        ops = [m.total_operations for m in use]
        times = [m.total_time for m in use]
        bytes_ = [m.total_bytes for m in use]
        errors = [m.error_count for m in use]

        agg = PerformanceMetrics()
        agg.name = use[0].name
        agg.concurrent_clients = use[0].concurrent_clients
        agg.total_operations = int(statistics.median(ops)) if ops else 0
        agg.total_time = float(statistics.median(times)) if times else 0.0
        agg.total_bytes = int(statistics.median(bytes_)) if bytes_ else 0
        agg.error_count = int(statistics.median(errors)) if errors else 0

        # 合并并修剪延迟数据以去掉抖动导致的异常值
        combined_lat = []
        for m in use:
            combined_lat.extend(m.raw_latencies)
        combined_lat.sort()
        n = len(combined_lat)
        drop = int(n * self.config.discard_fraction_per_side)
        if n > 20 and drop > 0 and n > drop * 2:
            trimmed = combined_lat[drop: n - drop]
        else:
            trimmed = combined_lat

        agg.raw_latencies = trimmed
        agg.calculate_percentiles()

        # 资源使用取中位数
        try:
            agg.cpu_usage = statistics.median([m.cpu_usage for m in use])
            agg.memory_usage = statistics.median([m.memory_usage for m in use])
            agg.thread_count = int(statistics.median([m.thread_count for m in use]))
            agg.handle_count = int(statistics.median([m.handle_count for m in use]))
        except Exception:
            pass

        agg.completed = all(m.completed for m in metrics_list)
        return agg