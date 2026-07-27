#!/usr/bin/env python3
"""
coronet 测试后清理脚本

在 ctest --output-on-failure 结束后运行，清理：
  1. 残留的压测服务端进程 (redis_echo_*, stress_driver)
  2. 测试产生的临时文件 (/tmp/coronet_*.dat, %TEMP%/coronet_*.dat)
  3. 可选：清理 build 目录下的 bench_report 临时输出

用法:
  python script/cleanup.py           # 清理进程 + 临时文件
  python script/cleanup.py --keep    # 仅列出，不实际清理 (dry-run)
  python script/cleanup.py --ports   # 额外检查残留端口占用
"""

import argparse
import os
import sys
import glob
import shutil
import subprocess
import platform

# ---- 需要清理的进程名 ----
STRESS_PROCESSES = [
    "redis_echo_ST",
    "redis_echo_chain",
    "redis_echo_MT",
    "redis_echo_asio_ST",
    "redis_echo_asio_MT",
    "stress_driver",
]

# ---- 测试临时文件模式 ----
TEMP_PATTERNS = [
    "coronet_iso_write_test.dat",
    "coronet_fio.dat",
    "coronet_big.dat",
    "coronet_*.dat",
]


def is_windows():
    return platform.system() == "Windows"


def kill_processes(dry_run=False):
    """终止残留的压测服务端进程"""
    killed = 0
    if is_windows():
        # Windows: taskkill
        for proc in STRESS_PROCESSES:
            try:
                result = subprocess.run(
                    ["taskkill", "/F", "/IM", f"{proc}.exe"],
                    capture_output=True, text=True, timeout=5
                )
                if result.returncode == 0:
                    print(f"  [KILL] {proc}.exe")
                    killed += 1
            except Exception:
                pass
    else:
        # Linux/macOS: pkill
        for proc in STRESS_PROCESSES:
            try:
                result = subprocess.run(
                    ["pkill", "-f", proc],
                    capture_output=True, text=True, timeout=5
                )
                if result.returncode == 0:
                    print(f"  [KILL] {proc}")
                    killed += 1
            except Exception:
                pass
    return killed


def clean_temp_files(dry_run=False):
    """删除测试产生的临时文件"""
    removed = 0

    if is_windows():
        temp_dirs = [os.environ.get("TEMP", os.environ.get("TMP", "."))]
    else:
        temp_dirs = ["/tmp", "/var/tmp"]

    for temp_dir in temp_dirs:
        if not temp_dir or not os.path.isdir(temp_dir):
            continue
        for pattern in TEMP_PATTERNS:
            for fpath in glob.glob(os.path.join(temp_dir, pattern)):
                if dry_run:
                    print(f"  [DRY] {fpath}")
                else:
                    try:
                        os.remove(fpath)
                        print(f"  [DEL]  {fpath}")
                        removed += 1
                    except Exception as e:
                        print(f"  [ERR]  {fpath}: {e}")

    # 清理 WSL 路径（如果从 Windows 运行但测试在 WSL 中）
    if not is_windows():
        wsl_tmp = "/mnt/c/Users"
        # skip on Linux
    else:
        # 也清理 WSL /tmp 对应的 Windows 路径
        pass

    return removed


def clean_bench_reports(build_dir=None, dry_run=False):
    """清理 build 目录中压测产生的 bench_report 临时文件"""
    removed = 0
    search_dirs = []

    if build_dir:
        search_dirs.append(build_dir)
    else:
        # 自动查找项目 build 目录
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.dirname(script_dir)
        for name in os.listdir(project_root):
            full = os.path.join(project_root, name)
            if name.startswith("build") and os.path.isdir(full):
                search_dirs.append(full)

    for bdir in search_dirs:
        stress_dir = os.path.join(bdir, "stress-test")
        if not os.path.isdir(stress_dir):
            continue
        for pattern in ["bench_report_*.csv", "bench_report_*.txt"]:
            for fpath in glob.glob(os.path.join(stress_dir, pattern)):
                if dry_run:
                    print(f"  [DRY] {fpath}")
                else:
                    try:
                        os.remove(fpath)
                        print(f"  [DEL]  {fpath}")
                        removed += 1
                    except Exception as e:
                        print(f"  [ERR]  {fpath}: {e}")

    return removed


def check_ports():
    """检查可能残留的压测端口"""
    ports = [17080, 17081, 17082, 17090, 17091, 16500, 16501, 16502, 16510, 16511]
    occupied = []

    if is_windows():
        try:
            result = subprocess.run(
                ["netstat", "-ano"], capture_output=True, text=True, timeout=10
            )
            lines = result.stdout.split("\n")
            for port in ports:
                for line in lines:
                    if f":{port}" in line and "LISTENING" in line:
                        parts = line.split()
                        if len(parts) >= 5:
                            occupied.append((port, parts[-1]))
                        break
        except Exception:
            pass
    else:
        try:
            result = subprocess.run(
                ["ss", "-tlnp"], capture_output=True, text=True, timeout=10
            )
            for port in ports:
                if f":{port} " in result.stdout:
                    occupied.append((port, "?"))
        except Exception:
            pass

    return occupied


def main():
    parser = argparse.ArgumentParser(description="coronet test cleanup")
    parser.add_argument("--keep", action="store_true",
                        help="Dry-run: list without deleting/killing")
    parser.add_argument("--ports", action="store_true",
                        help="Also check for leftover listening ports")
    parser.add_argument("--build-dir", default=None,
                        help="Specific build directory to clean bench reports from")
    args = parser.parse_args()

    print("=" * 50)
    print("  coronet 测试清理")
    print(f"  Platform: {platform.system()}")
    print(f"  Mode:     {'DRY-RUN' if args.keep else 'CLEAN'}")
    print("=" * 50)

    # 1. Kill leftover processes
    print("\n[1/3] 清理残留进程...")
    killed = kill_processes(dry_run=args.keep)
    if killed == 0:
        print("  无残留进程")

    # 2. Remove temp files
    print("\n[2/3] 清理临时文件...")
    removed = clean_temp_files(dry_run=args.keep)
    if removed == 0:
        print("  无临时文件")

    # 3. Clean bench reports in build dirs
    print("\n[3/3] 清理 build 目录压测报告...")
    removed_reports = clean_bench_reports(
        build_dir=args.build_dir, dry_run=args.keep)
    if removed_reports == 0:
        print("  无压测报告")

    # 4. Optional port check
    if args.ports:
        print("\n[PORTS] 检查残留端口...")
        occupied = check_ports()
        if occupied:
            for port, pid in occupied:
                print(f"  [WARN] 端口 {port} 被 PID {pid} 占用")
        else:
            print("  无残留端口")

    print("\n" + "=" * 50)
    print("  清理完成")
    print("=" * 50)


if __name__ == "__main__":
    main()
