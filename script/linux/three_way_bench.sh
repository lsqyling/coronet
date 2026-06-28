#!/bin/bash
# ============================================================
# 3-Way Comparison: coronet vs co_context vs ASIO
# Fresh server instance for EACH test.
# Requires co_context built alongside coronet.
# Usage: bash script/linux/three_way_bench.sh
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJ_DIR}/build-release"
DATA_DIR="${PROJ_DIR}/data"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${DATA_DIR}/threeway_${TIMESTAMP}"

# ---- Binaries ----
CORONET_BIN="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_BIN="${BUILD_DIR}/bench/redis_echo_asio"

# co_context project (sibling directory)
CO_CTX_PROJ="${PROJ_DIR}/../co_context"
if [ -d "${CO_CTX_PROJ}" ]; then
    CO_CTX_BIN="${CO_CTX_PROJ}/build-release/example/redis_echo"
    CO_ASIO_BIN="${CO_CTX_PROJ}/build-release/bench/redis_echo_asio"
else
    echo "WARNING: co_context project not found at ${CO_CTX_PROJ}"
    echo "  Only coronet vs ASIO will be tested."
    CO_CTX_BIN=""
    CO_ASIO_BIN=""
fi

REDIS_BENCH="${REDIS_BENCH:-redis-benchmark}"

# ---- Config ----
CONCURRENCIES=(10 50 100 200 500 1000)
TOTAL_REQUESTS=100000
TIMEOUT_SEC=120

# Use separate port ranges for each server
CORONET_PORT=7600
CO_CTX_PORT=7700
ASIO_PORT=8600

mkdir -p "${RESULT_DIR}"

echo "============================================"
echo "  3-Way Redis PING Benchmark"
echo "  coronet vs co_context vs ASIO"
echo "  (fresh server for each test)"
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
if [ -n "${CO_CTX_BIN}" ]; then
    echo "concurrency,coronet,co_context,asio" > "${RESULT_DIR}/results.csv"
else
    echo "concurrency,coronet,asio" > "${RESULT_DIR}/results.csv"
fi

for conc in "${CONCURRENCIES[@]}"; do
    echo "--- Concurrency: ${conc} ---"

    echo -n "  coronet:    "
    crps=$(run_one "${CORONET_BIN}" "coronet" ${CORONET_PORT} ${conc})
    echo "${crps} req/s"

    if [ -n "${CO_CTX_BIN}" ]; then
        echo -n "  co_context: "
        cxps=$(run_one "${CO_CTX_BIN}" "co_context" ${CO_CTX_PORT} ${conc})
        echo "${cxps} req/s"
    else
        cxps="N/A"
    fi

    echo -n "  ASIO:       "
    aps=$(run_one "${ASIO_BIN}" "ASIO" ${ASIO_PORT} ${conc})
    echo "${aps} req/s"

    if [ -n "${CO_CTX_BIN}" ]; then
        echo "${conc},${crps},${cxps},${aps}" >> "${RESULT_DIR}/results.csv"
    else
        echo "${conc},${crps},${aps}" >> "${RESULT_DIR}/results.csv"
    fi
    echo ""
done

# ---- Summary ----
echo ""
echo "============================================"
echo "  3-Way Comparison Results"
echo "============================================"
if [ -n "${CO_CTX_BIN}" ]; then
    printf "%-10s %12s %12s %12s\n" "Conc" "coronet" "co_context" "ASIO"
    echo "----------------------------------------------------"
    while IFS=, read -r conc crps cxps aps; do
        [ "${conc}" = "concurrency" ] && continue
        printf "%-10s %12s %12s %12s\n" "${conc}" "${crps}" "${cxps}" "${aps}"
    done < "${RESULT_DIR}/results.csv"
else
    printf "%-10s %12s %12s\n" "Conc" "coronet" "ASIO"
    echo "-----------------------------------"
    while IFS=, read -r conc crps aps; do
        [ "${conc}" = "concurrency" ] && continue
        printf "%-10s %12s %12s\n" "${conc}" "${crps}" "${aps}"
    done < "${RESULT_DIR}/results.csv"
fi

echo ""
echo "Results: ${RESULT_DIR}/results.csv"
