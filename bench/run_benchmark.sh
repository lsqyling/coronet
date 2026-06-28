#!/bin/bash
# ============================================================
# Redis echo server benchmark: coronet (coroutine) vs ASIO (callback)
# ============================================================
# Requires: redis-benchmark (or redis_loadgen fallback)
# Usage: ./run_benchmark.sh [--loadgen]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build-release"
DATA_DIR="${SCRIPT_DIR}/../data"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${DATA_DIR}/benchmark_results_${TIMESTAMP}"

CORO_SERVER="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_SERVER="${BUILD_DIR}/bench/redis_echo_asio"
LOADGEN="${BUILD_DIR}/bench/redis_loadgen"

CORO_PORT=6379
ASIO_PORT=6380
USE_LOADGEN=false

if [ "$1" = "--loadgen" ]; then
    USE_LOADGEN=true
    BENCH_TOOL="${LOADGEN}"
else
    BENCH_TOOL="redis-benchmark"
    if ! command -v redis-benchmark &>/dev/null; then
        echo "redis-benchmark not found, falling back to loadgen"
        USE_LOADGEN=true
        BENCH_TOOL="${LOADGEN}"
    fi
fi

CONCURRENCIES=(10 50 100 200 500 1000 2000 5000)
TOTAL_REQUESTS=100000
TIMEOUT_SEC=120

mkdir -p "${RESULT_DIR}"

echo "============================================"
echo "  coronet vs ASIO Redis Benchmark"
echo "============================================"
echo "Bench tool: ${BENCH_TOOL}"
echo "Total requests per test: ${TOTAL_REQUESTS}"
echo "Concurrency levels: ${CONCURRENCIES[*]}"
echo "Results: ${RESULT_DIR}"
echo ""

run_bench() {
    local server=$1 server_name=$2 port=$3 csv_file=$4
    echo "=== Testing ${server_name} on port ${port} ==="

    # Start server in background
    ${server} ${port} &
    local SERVER_PID=$!
    sleep 1

    # Verify server is running
    if ! kill -0 ${SERVER_PID} 2>/dev/null; then
        echo "ERROR: ${server_name} failed to start"
        return 1
    fi

    echo "concurrency,rps" > "${csv_file}"

    for conc in "${CONCURRENCIES[@]}"; do
        echo -n "  concurrency=${conc}: "
        local rps=0

        if $USE_LOADGEN; then
            # Use custom load generator
            local output
            output=$(timeout ${TIMEOUT_SEC} ${LOADGEN} -c ${conc} -n ${TOTAL_REQUESTS} -p ${port} 2>&1) || true
            rps=$(echo "${output}" | grep "RPS:" | awk '{print $2}' || echo "0")
        else
            # Use redis-benchmark
            local result
            result=$(timeout ${TIMEOUT_SEC} redis-benchmark -h 127.0.0.1 -p ${port} \
                -t ping -n ${TOTAL_REQUESTS} -c ${conc} -q 2>/dev/null) || true
            if [ $? -eq 124 ]; then
                rps=0
                echo "TIMEOUT"
            else
                rps=$(echo "${result}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' || echo "0")
            fi
        fi

        [ -z "$rps" ] && rps=0
        echo "${conc},${rps}" >> "${csv_file}"
        echo "${rps} req/s"

        sleep 0.5  # Brief pause between tests
    done

    # Stop server
    kill ${SERVER_PID} 2>/dev/null || true
    wait ${SERVER_PID} 2>/dev/null || true
    echo ""
}

# ---- Run benchmarks ----
run_bench "${CORO_SERVER}" "coronet (coroutine)" ${CORO_PORT} \
    "${RESULT_DIR}/coronet.csv"
run_bench "${ASIO_SERVER}" "ASIO (callback)" ${ASIO_PORT} \
    "${RESULT_DIR}/asio.csv"

# ---- Generate plot ----
echo "=== Generating plot ==="
if command -v python3 &>/dev/null; then
    python3 "${SCRIPT_DIR}/plot_results.py" "${RESULT_DIR}" \
        "${SCRIPT_DIR}/../doc/coronet_asio_comparison.png" || true
else
    echo "python3 not found, skipping plot generation"
fi

echo ""
echo "============================================"
echo "  Benchmark complete!"
echo "  Results: ${RESULT_DIR}"
echo "============================================"
