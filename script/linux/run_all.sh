#!/bin/bash
# ============================================================
# Master script: run all benchmarks in sequence
# Usage: bash script/linux/run_all.sh [--smoke|--full|--threeway]
#   --smoke    : quick smoke test only
#   --full     : coronet vs ASIO fair comparison (default)
#   --threeway : coronet vs co_context vs ASIO
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

MODE="${1:---full}"

case "${MODE}" in
    --smoke)
        echo "Running smoke test..."
        bash "${SCRIPT_DIR}/smoke_test.sh"
        ;;
    --full)
        echo "Running coronet vs ASIO fair benchmark..."
        bash "${SCRIPT_DIR}/fair_bench.sh"
        ;;
    --threeway)
        echo "Running 3-way benchmark..."
        bash "${SCRIPT_DIR}/three_way_bench.sh"
        ;;
    --scale)
        echo "Running scaling test..."
        bash "${SCRIPT_DIR}/scaling_test.sh"
        ;;
    *)
        echo "Usage: $0 [--smoke|--full|--threeway|--scale]"
        exit 1
        ;;
esac

echo ""
echo "All done."
