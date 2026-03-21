"""主入口 - 运行性能测试"""
import asyncio
import os
import sys
import platform
from pathlib import Path
import traceback
from datetime import datetime

# 添加项目根目录到路径
sys.path.insert(0, str(Path(__file__).parent.parent))

from test.config import DEFAULT_CONFIG
from test.benchmark import ServerBenchmark
from test.reporter import HTMLReportGenerator

# Rich 美化
try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich import box
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False
    print("⚠️  Rich 未安装，将使用简单输出。安装: pip install rich")


async def main():
    """主函数"""
    console = Console() if RICH_AVAILABLE else None
    
    # 打印标题
    title = "🚀 公平对比测试: aiowinfile (IOCP真异步) vs aiofiles (线程池模拟)"
    if console:
        console.print(Panel.fit(
            f"[bold cyan]{title}[/bold cyan]\n"
            f"[dim]相同并发数下对比性能 - 测试时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}[/dim]",
            border_style="cyan"
        ))
    else:
        print("=" * 80)
        print(title)
        print("=" * 80)
    
    # 检查依赖
    try:
        import aiowinfile
        console.print("[green]✅ aiowinfile: 可用[/green]")
    except ImportError:
        console.print("[red]❌ aiowinfile: 不可用，请先安装[/red]")
        return
    
    try:
        import aiofiles
        console.print("[yellow]📦 aiofiles: 可用[/yellow]")
    except ImportError:
        console.print("[yellow]⚠️  aiofiles: 不可用，将只测试 aiowinfile[/yellow]")
    
    # 创建测试目录
    test_dir = Path("./benchmark_data")
    test_dir.mkdir(exist_ok=True)
    
    # 创建测试实例
    benchmark = ServerBenchmark(test_dir, DEFAULT_CONFIG)
    
    # 准备数据
    file_paths = await benchmark.prepare_test_data(console)
    
    # 运行测试
    results = await benchmark.run_fair_comparison(file_paths, console=console)
    
    # 显示结果表格
    if console:
        console.print("\n[bold green]📊 测试结果汇总[/bold green]")
        
        for r in results:
            if 'aiowinfile' in r.name:
                color = "green"
            else:
                color = "yellow" if r.completed else "red"
            
            status = "✅ 完成" if r.completed else "⏰ 超时"
            
            table = Table(title=f"{r.name} - {r.concurrent_clients}并发", box=box.ROUNDED)
            table.add_column("指标", style="cyan")
            table.add_column("数值", style=color)
            
            table.add_row("总操作数", f"{r.total_operations:,}")
            table.add_row("吞吐量", f"{r.ops_per_second:,.0f} ops/s")
            table.add_row("带宽", f"{r.mb_per_second:.1f} MB/s")
            table.add_row("平均延迟", f"{r.avg_latency * 1000:.2f} ms")
            table.add_row("P95延迟", f"{r.p95_latency * 1000:.2f} ms")
            table.add_row("P99延迟", f"{r.p99_latency * 1000:.2f} ms")
            table.add_row("错误率", f"{r.error_rate:.2f}%")
            table.add_row("线程数", str(r.thread_count))
            table.add_row("状态", status)
            
            console.print(table)
    
    # 生成 HTML 报告
    report_gen = HTMLReportGenerator("公平对比: aiowinfile vs aiofiles 性能测试报告")
    report_gen.set_system_info({
        'os': platform.platform(),
        'python_version': platform.python_version(),
        'cpu_count': os.cpu_count(),
        'duration': DEFAULT_CONFIG.duration_seconds,
        'test_time': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    })
    
    for r in results:
        report_gen.add_result(r)
    
    report_path = Path("fair_comparison_report.html")
    report_gen.generate(report_path)
    
    if console:
        console.print(f"\n[bold green]📄 HTML 报告已保存: {report_path.absolute()}[/bold green]")
        console.print("[dim]在浏览器中打开查看详细图表[/dim]")
    
    # 清理
    import shutil
    try:
        shutil.rmtree(test_dir)
        if console:
            console.print("[dim]🧹 已清理测试数据[/dim]")
    except:
        pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n\n⏹️  测试被中断")
    except Exception as e:
        print(f"\n❌ 测试失败: {e}")
        traceback.print_exc()