#!/usr/bin/env python3
"""
Plot coronet vs co_context vs ASIO benchmark results.
Usage: python3 script/linux/plot_results.py <results_csv> [output.png]
"""
import sys
import csv
import os
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def load_csv(path):
    """Load CSV format: concurrency,server1[,server2[,server3]]"""
    data = {}
    servers = []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader, [])
        servers = header[1:]  # skip 'concurrency'
        for s in servers:
            data[s] = {}
        for row in reader:
            if len(row) >= 2:
                conc = int(row[0])
                for i, s in enumerate(servers):
                    if i + 1 < len(row) and row[i + 1]:
                        try:
                            data[s][conc] = float(row[i + 1])
                        except ValueError:
                            data[s][conc] = 0
    return data, servers


def plot(csv_path, output_path=None):
    if output_path is None:
        output_path = os.path.splitext(csv_path)[0] + '.png'

    data, servers = load_csv(csv_path)
    if not data:
        print("ERROR: No data found")
        sys.exit(1)

    # Colors for each server
    colors = {
        'coronet':    '#2196F3',
        'co_context': '#4CAF50',
        'asio':       '#FF5722',
    }
    markers = {
        'coronet':    'o',
        'co_context': '^',
        'asio':       's',
    }

    all_conc = sorted(set().union(*[set(d.keys()) for d in data.values()]))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))

    # ---- Panel 1: Throughput curves ----
    for name in servers:
        vals = [data[name].get(c, 0) for c in all_conc]
        c = colors.get(name, '#999999')
        m = markers.get(name, 'o')
        ax1.plot(all_conc, vals, f'{m}-', color=c, linewidth=2,
                 markersize=8, label=name)

    ax1.set_xlabel('Concurrent Clients', fontsize=12)
    ax1.set_ylabel('Requests / Second', fontsize=12)
    ax1.set_title('Redis PING Benchmark: Throughput', fontsize=14)
    ax1.set_xscale('log')
    ax1.legend(fontsize=11)
    ax1.grid(True, alpha=0.3)

    # Annotate peaks
    for name in servers:
        vals = [data[name].get(c, 0) for c in all_conc]
        if any(v > 0 for v in vals):
            peak = max(vals)
            peak_conc = all_conc[vals.index(peak)]
            ax1.annotate(f'{peak:,.0f}',
                         xy=(peak_conc, peak),
                         xytext=(peak_conc * 1.1, peak * 1.05),
                         arrowprops=dict(arrowstyle='->', color=colors.get(name, '#999')),
                         fontsize=9, color=colors.get(name, '#999'), fontweight='bold')

    # ---- Panel 2: Analysis ----
    ax2.axis('off')

    lines = []
    lines.append("=" * 55)
    lines.append("  Redis PING Benchmark — Performance Summary")
    lines.append("=" * 55)
    lines.append("")

    # Header
    header = f"{'':18}"
    for s in servers:
        header += f" {s:>11}"
    lines.append(header)

    # Peak row
    peak_row = f"{'Peak RPS:':18}"
    for s in servers:
        vals = [v for v in data[s].values() if v > 0]
        peak_rps = max(vals) if vals else 0
        peak_row += f" {peak_rps:>11,.0f}"
    lines.append(peak_row)

    # Average row
    avg_row = f"{'Average RPS:':18}"
    for s in servers:
        vals = [v for v in data[s].values() if v > 0]
        avg_rps = np.mean(vals) if vals else 0
        avg_row += f" {avg_rps:>11,.0f}"
    lines.append(avg_row)

    # Failed tests
    fail_row = f"{'Failed tests:':18}"
    for s in servers:
        fails = sum(1 for v in data[s].values() if v == 0)
        fail_row += f" {fails:>11}"
    lines.append(fail_row)

    lines.append("")

    # Winner determination
    avgs = {}
    for s in servers:
        vals = [v for v in data[s].values() if v > 0]
        avgs[s] = np.mean(vals) if vals else 0

    best = max(avgs, key=avgs.get)
    lines.append(f"WINNER: {best} ({avgs[best]:,.0f} avg rps)")
    lines.append("")

    # Speedup vs baseline (last server = ASIO typically)
    if len(servers) >= 2 and avgs[servers[-1]] > 0:
        baseline = servers[-1]
        lines.append(f"Speedup vs {baseline}:")
        for s in servers[:-1]:
            if avgs[s] > 0:
                speedup = avgs[s] / avgs[baseline]
                lines.append(f"  {s}: {speedup:.2f}x")

    lines.append("")
    lines.append("Architecture:")
    lines.append("  coronet:    C++20 coroutines + io_uring/IOCP")
    lines.append("  co_context: C++20 coroutines + io_uring")
    lines.append("  ASIO:       callback-based, epoll reactor")

    text = "\n".join(lines)
    ax2.text(0.05, 0.95, text, transform=ax2.transAxes,
             fontsize=10, fontfamily='monospace',
             verticalalignment='top',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.3))
    ax2.set_title('Analysis', fontsize=14)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"Plot saved: {output_path}")

    # Print data table
    print("")
    header = f"{'Conc':>8}"
    for s in servers:
        header += f" {s:>12}"
    print(header)
    print("-" * (8 + 13 * len(servers)))
    for c in all_conc:
        row = f"{c:>8}"
        for s in servers:
            row += f" {data[s].get(c, 0):>12,.0f}"
        print(row)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    csv_path = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else None
    plot(csv_path, output)
