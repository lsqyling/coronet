#!/bin/bash
# coronet vs ASIO — Redis PING 压测脚本
# ⚠ DEPRECATED — use script/linux/fair_bench.sh instead
echo "⚠ This script is deprecated. Use: bash script/linux/fair_bench.sh"
echo ""

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "${SCRIPT_DIR}/linux/fair_bench.sh" "$@"
