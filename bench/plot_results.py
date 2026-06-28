#!/usr/bin/env python3
"""
Plot coronet (coroutine) vs ASIO (callback) Redis PING benchmark results.
Usage: python3 plot_results.py <results_dir> [output.png]
"""

import sys
import os
import csv
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def load_csv(path):
    """Load benchmark CSV with format: concurrency,rps"""
    data = {}
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader, None)
        for row in reader:
            if len(row) >= 2:
                conc = int(row[0])
                rps = float(row[1])
                data[conc] = rps
    return data


def plot(results_dir, output_path):
    coro_path = os.path.join(results_dir, 'coronet.csv')
    asio_path = os.path.join(results_dir, 'asio.csv')

    coro = load_csv(coro_path) if os.path.exists(coro_path) else {}
    asio = load_csv(asio_path) if os.path.exists(asio_path) else {}

    if not coro and not asio:
        print("ERROR: No data found")
        sys.exit(1)

    # Common concurrency levels
    all_conc = sorted(set(coro.keys()) | set(asio.keys()))
    coro_vals = [coro.get(c, 0) for c in all_conc]
    asio_vals = [asio.get(c, 0) for c in all_conc]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 7))

    # ---- Left panel: Throughput curves ----
    ax1.plot(all_conc, coro_vals, 'o-', color='#2196F3', linewidth=2,
             markersize=8, label='coronet (coroutine)')
    ax1.plot(all_conc, asio_vals, 's--', color='#FF5722', linewidth=2,
             markersize=8, label='ASIO (callback)')

    ax1.set_xlabel('Concurrent Clients', fontsize=12)
    ax1.set_ylabel('Requests / Second', fontsize=12)
    ax1.set_title('Redis PING Benchmark: Throughput', fontsize=14)
    ax1.set_xscale('log')
    ax1.legend(fontsize=11)
    ax1.grid(True, alpha=0.3)

    # Annotate peak values
    if coro_vals:
        coro_peak = max(coro_vals)
        coro_peak_conc = all_conc[coro_vals.index(coro_peak)]
        ax1.annotate(f'{coro_peak:,.0f}',
                     xy=(coro_peak_conc, coro_peak),
                     xytext=(coro_peak_conc, coro_peak * 1.1),
                     arrowprops=dict(arrowstyle='->', color='#2196F3'),
                     fontsize=10, color='#2196F3', fontweight='bold')

    if asio_vals:
        asio_peak = max(asio_vals)
        asio_peak_conc = all_conc[asio_vals.index(asio_peak)]
        ax1.annotate(f'{asio_peak:,.0f}',
                     xy=(asio_peak_conc, asio_peak),
                     xytext=(asio_peak_conc, asio_peak * 1.1),
                     arrowprops=dict(arrowstyle='->', color='#FF5722'),
                     fontsize=10, color='#FF5722', fontweight='bold')

    # ---- Right panel: Analysis ----
    ax2.axis('off')

    # Compute statistics
    coro_avg = np.mean([v for v in coro_vals if v > 0]) if any(v > 0 for v in coro_vals) else 0
    asio_avg = np.mean([v for v in asio_vals if v > 0]) if any(v > 0 for v in asio_vals) else 0
    coro_fails = sum(1 for v in coro_vals if v == 0)
    asio_fails = sum(1 for v in asio_vals if v == 0)

    # High concurrency (top 3 levels)
    high_conc = all_conc[-3:] if len(all_conc) >= 3 else all_conc
    high_coro = [coro.get(c, 0) for c in high_conc]
    high_asio = [asio.get(c, 0) for c in high_conc]
    high_coro_avg = np.mean([v for v in high_coro if v > 0]) if any(v > 0 for v in high_coro) else 0
    high_asio_avg = np.mean([v for v in high_asio if v > 0]) if any(v > 0 for v in high_asio) else 0

    lines = []
    lines.append("=" * 50)
    lines.append("  Performance Comparison Summary")
    lines.append("=" * 50)
    lines.append("")
    lines.append(f"{'':20} {'coronet':>12} {'ASIO':>12}")
    lines.append(f"{'Peak RPS:':20} {max(coro_vals):>12,.0f} {max(asio_vals):>12,.0f}")
    lines.append(f"{'Average RPS:':20} {coro_avg:>12,.0f} {asio_avg:>12,.0f}")
    lines.append(f"{'Failed tests:':20} {coro_fails:>12} {asio_fails:>12}")
    lines.append(f"{'High-conc avg:':20} {high_coro_avg:>12,.0f} {high_asio_avg:>12,.0f}")
    lines.append("")

    # Winner determination
    if max(coro_vals) > max(asio_vals) and coro_avg > asio_avg:
        lines.append("WINNER: coronet (coroutine)")
        lines.append("  Higher peak throughput AND better average")
    elif max(asio_vals) > max(coro_vals) and asio_avg > coro_avg:
        lines.append("WINNER: ASIO (callback)")
        lines.append("  Higher peak throughput AND better average")
    else:
        winner_peak = "coronet" if max(coro_vals) > max(asio_vals) else "ASIO"
        winner_avg = "coronet" if coro_avg > asio_avg else "ASIO"
        lines.append(f"  Peak winner: {winner_peak}")
        lines.append(f"  Average winner: {winner_avg}")

    lines.append("")
    lines.append("Key observations:")
    if coro_fails > asio_fails:
        lines.append(f"  - coronet failed {coro_fails} high-concurrency tests")
    if asio_fails > coro_fails:
        lines.append(f"  - ASIO failed {asio_fails} high-concurrency tests")

    # Show speedup
    if asio_avg > 0:
        speedup = coro_avg / asio_avg
        lines.append(f"  - coronet vs ASIO speedup: {speedup:.2f}x")

    lines.append("")
    lines.append("Architecture notes:")
    lines.append("  - coronet: C++20 coroutines, proactor I/O (io_uring/IOCP)")
    lines.append("  - ASIO:    callback-based, epoll/IOCP reactor")

    text = "\n".join(lines)
    ax2.text(0.05, 0.95, text, transform=ax2.transAxes,
             fontsize=10, fontfamily='monospace',
             verticalalignment='top',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.3))

    ax2.set_title('Analysis', fontsize=14)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"Plot saved to: {output_path}")

    # Print data table
    print("")
    print(f"{'Concurrency':>12} {'coronet':>12} {'ASIO':>12} {'Speedup':>10}")
    print("-" * 46)
    for c, co, ao in zip(all_conc, coro_vals, asio_vals):
        sp = co / ao if ao > 0 else 0
        print(f"{c:>12} {co:>12,.0f} {ao:>12,.0f} {sp:>9.2f}x")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    results_dir = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else 'performance_comparison.png'
    plot(results_dir, output)
