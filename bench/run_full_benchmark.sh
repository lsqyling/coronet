#!/bin/bash
# Full benchmark: coronet (coroutine) vs ASIO (callback)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build-release"
DATA_DIR="${SCRIPT_DIR}/../data"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${DATA_DIR}/benchmark_results_${TIMESTAMP}"

CORO_BIN="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_BIN="${BUILD_DIR}/bench/redis_echo_asio"

CONCURRENCIES=(10 50 100 200 500 1000)
TOTAL_REQUESTS=100000
CORO_PORT=6390
ASIO_PORT=6391

mkdir -p "${RESULT_DIR}"

echo "============================================"
echo "  coronet vs ASIO Redis PING Benchmark"
echo "============================================"
echo "Timestamp: ${TIMESTAMP}"
echo "Total requests per test: ${TOTAL_REQUESTS}"
echo "Concurrency: ${CONCURRENCIES[*]}"
echo ""

run_bench() {
    local server=$1 name=$2 port=$3 csv=$4

    echo "=== ${name} (port ${port}) ==="

    ${server} ${port} > /tmp/server_bench.log 2>&1 &
    local PID=$!
    sleep 2

    if ! kill -0 ${PID} 2>/dev/null; then
        echo "ERROR: ${name} failed to start"
        cat /tmp/server_bench.log
        return 1
    fi

    echo "concurrency,rps" > "${csv}"

    for conc in "${CONCURRENCIES[@]}"; do
        echo -n "  c=${conc}: "
        local rps=0

        local output
        output=$(timeout 120 redis-benchmark -h 127.0.0.1 -p ${port} \
            -t ping -n ${TOTAL_REQUESTS} -c ${conc} -q 2>/dev/null) || true

        if [ $? -eq 124 ]; then
            rps=0
            echo "TIMEOUT"
        else
            rps=$(echo "${output}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' || echo "0")
            [ -z "${rps}" ] && rps=0
            echo "${rps} req/s"
        fi

        echo "${conc},${rps}" >> "${csv}"
        sleep 1
    done

    kill ${PID} 2>/dev/null || true
    wait ${PID} 2>/dev/null || true
    echo ""
}

# Run benchmarks
CORO_CSV="${RESULT_DIR}/coronet.csv"
ASIO_CSV="${RESULT_DIR}/asio.csv"

run_bench "${CORO_BIN}" "coronet (coroutine)" ${CORO_PORT} "${CORO_CSV}"
run_bench "${ASIO_BIN}" "ASIO (callback)" ${ASIO_PORT} "${ASIO_CSV}"

# Summary table
echo ""
echo "============================================"
echo "  Results Summary"
echo "============================================"
printf "%-10s %15s %15s\n" "Concurrency" "coronet" "ASIO"
echo "------------------------------------------"
paste "${CORO_CSV}" "${ASIO_CSV}" | tail -n +2 | while read -r cline aline; do
    conc=$(echo "${cline}" | cut -d, -f1)
    crps=$(echo "${cline}" | cut -d, -f2)
    arps=$(echo "${aline}" | cut -d, -f2)
    printf "%-10s %15s %15s\n" "${conc}" "${crps}" "${arps}"
done

echo ""
echo "Results: ${RESULT_DIR}"
