"""测试配置"""
from dataclasses import dataclass, field


@dataclass
class BenchmarkConfig:
    """测试配置"""
    # 测试时长（秒）
    duration_seconds: int = 10
    
    # 超时时间（秒）
    timeout_seconds: int = 30
    
    # 测试并发数列表
    client_counts: list = field(default_factory=lambda: [10, 20, 50, 100, 200, 500])
    
    # 文件配置
    file_sizes: dict = field(default_factory=lambda: {
        'small': 100 * 1024 * 1024,      # 100MB
        'medium': 500 * 1024 * 1024,     # 500MB
        'large': 1024 * 1024 * 1024,     # 1GB
    })
    
    num_files_per_size: int = 3
    
    # 读写比例 (读%, 写%)
    read_write_ratio: tuple = (70, 30)
    
    # 是否预热
    warmup_enabled: bool = True
    
    # 是否清理系统缓存（需要管理员权限）
    clear_cache: bool = False
    # 每个测试点重复次数（取中间值以降低噪声）
    repeats: int = 3
    # 去除延迟分布中两端的比例（每侧），用于剔除抖动导致的异常值
    discard_fraction_per_side: float = 0.10


# 默认配置
DEFAULT_CONFIG = BenchmarkConfig()