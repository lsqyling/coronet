#!/bin/bash
# ============================================================
# coronet vs ASIO — Fair Redis PING Benchmark
# Fresh server instance for EACH concurrency level.
# Usage: bash script/linux/fair_bench.sh
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJ_DIR}/build-release"
DATA_DIR="${PROJ_DIR}/data"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${DATA_DIR}/bench_${TIMESTAMP}"

CORO_BIN="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_BIN="${BUILD_DIR}/bench/redis_echo_asio"
REDIS_BENCH="${REDIS_BENCH:-redis-benchmark}"

# ---- Config ----
CONCURRENCIES=(10 50 100 200 500 1000)
TOTAL_REQUESTS=100000
CORO_PORT=7700
ASIO_PORT=8700
TIMEOUT_SEC=120

mkdir -p "${RESULT_DIR}"

echo "============================================"
echo "  coronet vs ASIO — Fair Benchmark"
echo "  (fresh server for each concurrency level)"
echo "============================================"
echo "Timestamp:  ${TIMESTAMP}"
echo "Total reqs: ${TOTAL_REQUESTS}"
echo "Concurrency: ${CONCURRENCIES[*]}"
echo "Results:    ${RESULT_DIR}"
echo ""

# ---- Run single test ----
run_one() {
    local server=$1 name=$2 port=$3 conc=$4

    pkill -9 $(basename ${server}) 2>/dev/null || true
    sleep 1

    ${server} ${port} > /dev/null 2>&1 &
    local PID=$!
    sleep 2

    if ! kill -0 ${PID} 2>/dev/null; then
        echo "CRASH"
        return
    fi

    local output
    output=$(timeout ${TIMEOUT_SEC} ${REDIS_BENCH} \
        -h 127.0.0.1 -p ${port} \
        -t ping -n ${TOTAL_REQUESTS} -c ${conc} -q 2>/dev/null) || true

    local rps
    rps=$(echo "${output}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1)
    [ -z "${rps}" ] && rps=0

    kill ${PID} 2>/dev/null || true
    wait ${PID} 2>/dev/null || true
    sleep 1

    echo "${rps}"
}

# ---- Main ----
echo "concurrency,coronet,asio" > "${RESULT_DIR}/results.csv"

for conc in "${CONCURRENCIES[@]}"; do
    echo "--- Concurrency: ${conc} ---"

    echo -n "  coronet: "
    crps=$(run_one "${CORO_BIN}" "coronet" ${CORO_PORT} ${conc})
    echo "${crps} req/s"

    echo -n "  ASIO:    "
    arps=$(run_one "${ASIO_BIN}" "ASIO" ${ASIO_PORT} ${conc})
    echo "${arps} req/s"

    echo "${conc},${crps},${arps}" >> "${RESULT_DIR}/results.csv"
    echo ""
done

# ---- Summary ----
echo ""
echo "============================================"
echo "  Results: coronet vs ASIO"
echo "============================================"
printf "%-10s %15s %15s %10s\n" "Conc" "coronet" "ASIO" "Ratio"
echo "----------------------------------------------------------"
while IFS=, read -r conc crps arps; do
    [ "${conc}" = "concurrency" ] && continue
    if [ "${arps}" != "0" ] && [ -n "${arps}" ]; then
        ratio=$(awk "BEGIN {printf \"%.2f\", ${crps}/${arps}}")
    else
        ratio="N/A"
    fi
    printf "%-10s %15s %15s %10s\n" "${conc}" "${crps}" "${arps}" "${ratio}"
done < "${RESULT_DIR}/results.csv"

echo ""
echo "Results: ${RESULT_DIR}/results.csv"
