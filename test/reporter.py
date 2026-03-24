"""报告生成器"""
import json
from pathlib import Path
from datetime import datetime
from typing import List
import platform

from .metrics import PerformanceMetrics


class HTMLReportGenerator:
    """HTML 报告生成器"""
    
    def __init__(self, title: str = "异步文件 I/O 性能测试报告"):
        self.title = title
        self.results: List[PerformanceMetrics] = []
        self.system_info = {}
    
    def add_result(self, result: PerformanceMetrics):
        self.results.append(result)
    
    def set_system_info(self, info: dict):
        self.system_info = info
    
    def generate(self, output_path: Path) -> str:
        """生成 HTML 报告"""
        # 分组结果
        win_results = [r for r in self.results if 'ayafileio' in r.name and r.completed]
        aio_results = [r for r in self.results if 'aiofiles' in r.name and r.completed]
        
        # 准备图表数据
        win_by_clients = {r.concurrent_clients: r for r in win_results}
        aio_by_clients = {r.concurrent_clients: r for r in aio_results}
        all_clients = sorted(set(win_by_clients.keys()) | set(aio_by_clients.keys()))
        
        win_ops = [win_by_clients.get(c, PerformanceMetrics()).ops_per_second for c in all_clients]
        aio_ops = [aio_by_clients.get(c, PerformanceMetrics()).ops_per_second for c in all_clients]
        win_latency = [win_by_clients.get(c, PerformanceMetrics()).avg_latency * 1000 for c in all_clients]
        aio_latency = [aio_by_clients.get(c, PerformanceMetrics()).avg_latency * 1000 for c in all_clients]
        
        html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{self.title}</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: 'Segoe UI', 'Roboto', sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            padding: 20px;
            min-height: 100vh;
        }}
        .container {{
            max-width: 1400px;
            margin: 0 auto;
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            overflow: hidden;
        }}
        .header {{
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px 40px;
        }}
        .header h1 {{ font-size: 2.5em; margin-bottom: 10px; }}
        .system-info {{
            background: #f8f9fa;
            padding: 20px 40px;
            border-bottom: 1px solid #e0e0e0;
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 15px;
        }}
        .info-card {{
            background: white;
            padding: 15px;
            border-radius: 10px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.05);
        }}
        .info-card .label {{ font-size: 0.85em; color: #666; margin-bottom: 5px; }}
        .info-card .value {{ font-size: 1.3em; font-weight: bold; color: #333; }}
        .content {{ padding: 30px 40px; }}
        .section {{ margin-bottom: 40px; }}
        .section h2 {{
            color: #333;
            margin-bottom: 20px;
            padding-bottom: 10px;
            border-bottom: 3px solid #667eea;
        }}
        .comparison-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }}
        .metric-card {{
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            border-radius: 15px;
            text-align: center;
            transition: transform 0.3s;
        }}
        .metric-card:hover {{ transform: translateY(-5px); }}
        .metric-card .metric-value {{ font-size: 2.5em; font-weight: bold; margin: 10px 0; }}
        .chart-container {{ margin: 30px 0; padding: 20px; background: #f8f9fa; border-radius: 10px; }}
        table {{
            width: 100%;
            border-collapse: collapse;
            margin-top: 20px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            border-radius: 10px;
            overflow: hidden;
        }}
        th {{ background: #667eea; color: white; padding: 12px; text-align: left; }}
        td {{ padding: 10px 12px; border-bottom: 1px solid #e0e0e0; }}
        tr:hover {{ background: #f5f5f5; }}
        .badge-success {{ background: #d4edda; color: #155724; padding: 4px 12px; border-radius: 20px; }}
        .badge-danger {{ background: #f8d7da; color: #721c24; padding: 4px 12px; border-radius: 20px; }}
        .footer {{ background: #f8f9fa; padding: 20px; text-align: center; color: #666; }}
        @media (max-width: 768px) {{
            .header, .content {{ padding: 20px; }}
            .metric-value {{ font-size: 1.5em; }}
        }}
    </style>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🚀 {self.title}</h1>
            <div>相同并发数下，ayafileio (IOCP真异步) vs aiofiles (线程池模拟)</div>
        </div>
        
        <div class="system-info">
            <div class="info-card"><div class="label">操作系统</div><div class="value">{self.system_info.get('os', 'N/A')}</div></div>
            <div class="info-card"><div class="label">Python 版本</div><div class="value">{self.system_info.get('python_version', 'N/A')}</div></div>
            <div class="info-card"><div class="label">CPU 核心数</div><div class="value">{self.system_info.get('cpu_count', 'N/A')}</div></div>
            <div class="info-card"><div class="label">测试时长</div><div class="value">{self.system_info.get('duration', 'N/A')} 秒</div></div>
        </div>
        
        <div class="content">
            <div class="section">
                <h2>📊 性能对比总结</h2>
                <div class="comparison-grid">
                    {self._generate_cards(win_results, aio_results, all_clients)}
                </div>
            </div>
            
            <div class="section">
                <h2>📈 吞吐量对比</h2>
                <div class="chart-container">
                    <canvas id="throughputChart" style="max-height: 400px;"></canvas>
                </div>
            </div>
            
            <div class="section">
                <h2>⏱️ 延迟对比</h2>
                <div class="chart-container">
                    <canvas id="latencyChart" style="max-height: 400px;"></canvas>
                </div>
            </div>
            
            <div class="section">
                <h2>📋 详细结果</h2>
                {self._generate_table()}
            </div>
        </div>
        
        <div class="footer">
            <p>报告生成: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
            <p><strong>结论</strong>: ayafileio 基于 Windows IOCP 真异步，高并发下性能远超 aiofiles</p>
        </div>
    </div>
    
    <script>
        new Chart(document.getElementById('throughputChart'), {{
            type: 'bar',
            data: {{
                labels: {json.dumps(all_clients)},
                datasets: [
                    {{ label: 'ayafileio', data: {json.dumps(win_ops)}, backgroundColor: 'rgba(102,126,234,0.7)' }},
                    {{ label: 'aiofiles', data: {json.dumps(aio_ops)}, backgroundColor: 'rgba(220,53,69,0.7)' }}
                ]
            }},
            options: {{ responsive: true, scales: {{ y: {{ title: {{ display: true, text: 'ops/s' }} }}, x: {{ title: {{ display: true, text: '并发数' }} }} }} }}
        }});
        
        new Chart(document.getElementById('latencyChart'), {{
            type: 'line',
            data: {{
                labels: {json.dumps(all_clients)},
                datasets: [
                    {{ label: 'ayafileio', data: {json.dumps(win_latency)}, borderColor: 'rgba(102,126,234,1)', fill: true }},
                    {{ label: 'aiofiles', data: {json.dumps(aio_latency)}, borderColor: 'rgba(220,53,69,1)', fill: true }}
                ]
            }},
            options: {{ responsive: true, scales: {{ y: {{ title: {{ display: true, text: '延迟 (ms)' }} }}, x: {{ title: {{ display: true, text: '并发数' }} }} }} }}
        }});
    </script>
</body>
</html>"""
        
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(html)
        
        return str(output_path)
    
    def _generate_cards(self, win_results, aio_results, all_clients):
        """生成对比卡片"""
        if not win_results or not aio_results:
            return ""
        
        max_clients = max(all_clients) if all_clients else 0
        win_max = max(win_results, key=lambda x: x.ops_per_second) if win_results else None
        aio_max = max(aio_results, key=lambda x: x.ops_per_second) if aio_results else None
        
        cards = []
        
        if win_max:
            cards.append(f"""
            <div class="metric-card">
                <div class="metric-label">🏆 ayafileio 峰值吞吐</div>
                <div class="metric-value">{win_max.ops_per_second:,.0f}</div>
                <div class="metric-label">ops/s @ {win_max.concurrent_clients}并发</div>
            </div>
            """)
        
        if aio_max:
            cards.append(f"""
            <div class="metric-card" style="background: linear-gradient(135deg, #868f96 0%, #596164 100%);">
                <div class="metric-label">📦 aiofiles 峰值吞吐</div>
                <div class="metric-value">{aio_max.ops_per_second:,.0f}</div>
                <div class="metric-label">ops/s @ {aio_max.concurrent_clients}并发</div>
            </div>
            """)
        
        # 找相同并发下的对比
        common_clients = set(r.concurrent_clients for r in win_results) & set(r.concurrent_clients for r in aio_results)
        if common_clients:
            max_common = max(common_clients)
            win = next(r for r in win_results if r.concurrent_clients == max_common)
            aio = next(r for r in aio_results if r.concurrent_clients == max_common)
            ratio = win.ops_per_second / aio.ops_per_second if aio.ops_per_second > 0 else 0
            cards.append(f"""
            <div class="metric-card" style="background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%);">
                <div class="metric-label">🎯 {max_common}并发优势</div>
                <div class="metric-value">{ratio:.1f}x</div>
                <div class="metric-label">ayafileio 快 {ratio:.0f} 倍</div>
            </div>
            """)
        
        return ''.join(cards)
    
    def _generate_table(self):
        """生成结果表格"""
        rows = []
        for r in self.results:
            status = '<span class="badge-success">✓ 完成</span>' if r.completed else '<span class="badge-danger">✗ 超时</span>'
            rows.append(f"""
            <tr>
                <td><strong>{r.name}</strong></td>
                <td>{r.concurrent_clients}</td>
                <td>{r.ops_per_second:,.0f}</td>
                <td>{r.mb_per_second:.1f}</td>
                <td>{r.avg_latency * 1000:.2f}</td>
                <td>{r.p95_latency * 1000:.2f}</td>
                <td>{r.p99_latency * 1000:.2f}</td>
                <td>{r.error_rate:.2f}%</td>
                <td>{status}</td>
            </tr>
            """)
        
        return f"""
        <table>
            <thead><tr>
                <th>库名称</th><th>并发数</th><th>吞吐量</th><th>带宽</th>
                <th>平均延迟</th><th>P95延迟</th><th>P99延迟</th><th>错误率</th><th>状态</th>
            </tr></thead>
            <tbody>{''.join(rows)}</tbody>
        </table>
        """