"""
coronet vs Boost.Asio vs co_context — 性能对比可视化

运行环境: Windows PowerShell (pyvenv)
用法:
    .\pyvenv\Scripts\activate
    pip install matplotlib numpy
    python script\coronet_asio_cocontext_Plot.py

数据文件: data/summary_*.csv
输出:      data/benchmark_comparison.png
"""

import os
import sys
import glob

# 确保能导入 matplotlib
try:
    import matplotlib
    matplotlib.use('Agg')  # 无头模式
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("请安装依赖: pip install matplotlib numpy")
    sys.exit(1)


def parse_csv(filepath):
    """解析 CSV 文件，返回 (connections, throughput) 两个列表"""
    conns = []
    troughputs = []
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()
        for line in lines[1:]:  # 跳过表头
            line = line.strip()
            if not line:
                continue
            parts = line.split(',')
            if len(parts) >= 2:
                try:
                    c = int(parts[0])
                    t = float(parts[1])
                    conns.append(c)
                    troughputs.append(t)
                except ValueError:
                    continue
    return conns, troughputs


def main():
    # 项目根目录 (脚本在 script/ 下)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    data_dir = os.path.join(project_dir, 'data')

    if not os.path.isdir(data_dir):
        print(f"错误: 数据目录 {data_dir} 不存在，请先运行压测脚本")
        sys.exit(1)

    # 后端配置: (文件名前缀, 显示名称, 颜色, 线型)
    backends = [
        ('summary_coronet',   'coronet',   '#2196F3', 'o-'),
        ('summary_asio',      'Boost.Asio','#FF5722', 's--'),
        ('summary_cocontext', 'co_context','#4CAF50', '^-'),
    ]

    plt.style.use('seaborn-v0_8-darkgrid')
    fig, ax = plt.subplots(figsize=(10, 6))

    all_conns = []

    for prefix, label, color, marker in backends:
        csv_path = os.path.join(data_dir, f'{prefix}.csv')
        if not os.path.exists(csv_path):
            print(f"警告: {csv_path} 不存在，跳过")
            continue

        conns, tps = parse_csv(csv_path)
        if not conns:
            print(f"警告: {csv_path} 无有效数据，跳过")
            continue

        all_conns.extend(conns)
        ax.plot(conns, tps, marker, color=color, linewidth=2,
                markersize=8, label=label)

        # 在数据点上标注数值
        for c, t in zip(conns, tps):
            ax.annotate(f'{t:.0f}', (c, t), textcoords="offset points",
                       xytext=(0, 12), ha='center', fontsize=8,
                       color=color, fontweight='bold')

    if not all_conns:
        print("错误: 没有可用的数据，请先运行压测脚本")
        sys.exit(1)

    ax.set_xlabel('Concurrent Connections', fontsize=13, fontweight='bold')
    ax.set_ylabel('Throughput (requests/sec)', fontsize=13, fontweight='bold')
    ax.set_title('coronet vs Boost.Asio vs co_context — PING Benchmark',
                 fontsize=15, fontweight='bold', pad=15)
    ax.legend(fontsize=12, loc='upper left', framealpha=0.9)
    ax.grid(True, alpha=0.3)
    ax.set_xticks(sorted(set(all_conns)))

    # 添加性能说明
    fig.text(0.5, 0.01,
             'Benchmark: redis-benchmark -t ping | 100,000 requests per test | Lower is better',
             ha='center', fontsize=10, style='italic', alpha=0.7)

    plt.tight_layout(rect=[0, 0.03, 1, 0.98])

    # 保存图片
    output_path = os.path.join(data_dir, 'benchmark_comparison.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"图表已保存: {output_path}")

    # 同时输出文本表格
    print("\n========== 性能对比表 (req/s) ==========")
    print(f"{'Connections':>12}", end='')
    for _, label, _, _ in backends:
        print(f"{label:>18}", end='')
    print()

    for prefix, label, _, _ in backends:
        csv_path = os.path.join(data_dir, f'{prefix}.csv')
        if not os.path.exists(csv_path):
            continue
        conns, tps = parse_csv(csv_path)
        for i, c in enumerate(conns):
            if i == 0:
                print(f"{c:>12}", end='')
            print(f"{tps[i]:>18.0f}", end='')
        print()

    print("==========================================")


if __name__ == '__main__':
    main()
