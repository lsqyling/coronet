#!/bin/bash
# ============================================================
# Step-by-step scaling test — start small, scale up gradually.
# Useful for debugging crashes and finding breaking points.
# Usage: bash script/linux/scaling_test.sh
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJ_DIR}/build-release"

CORO_BIN="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_BIN="${BUILD_DIR}/bench/redis_echo_asio"
REDIS_BENCH="${REDIS_BENCH:-redis-benchmark}"

echo "============================================"
echo "  coronet Scaling Test"
echo "============================================"
echo ""

# Step 1-4: coronet only, increasing concurrency
echo "=== Phase 1: coronet scaling ==="

steps=(
    "1 1000"
    "10 10000"
    "50 50000"
    "100 100000"
    "200 100000"
    "500 100000"
    "1000 100000"
)

for step in "${steps[@]}"; do
    read -r conc reqs <<< "${step}"
    port=$((6700 + conc))

    echo "--- c=${conc} n=${reqs} ---"

    pkill -9 redis_echo_coro 2>/dev/null || true
    sleep 1

    ${CORO_BIN} ${port} > /dev/null 2>&1 &
    PID=$!
    sleep 2

    if ! kill -0 ${PID} 2>/dev/null; then
        echo "  coronet: CRASHED at startup"
        continue
    fi

    result=$(timeout 120 ${REDIS_BENCH} -h 127.0.0.1 -p ${port} \
        -t ping -n ${reqs} -c ${conc} -q 2>/dev/null) || true
    rps=$(echo "${result}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    [ -z "${rps}" ] && rps=0
    echo "  coronet: ${rps} req/s"

    kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null || true
    sleep 1
done

# Step 5: ASIO baseline
echo ""
echo "=== Phase 2: ASIO baseline (c=100 n=100000) ==="

pkill -9 redis_echo_asio 2>/dev/null || true
sleep 1
${ASIO_BIN} 6800 > /dev/null 2>&1 &
APID=$!
sleep 2

aresult=$(timeout 60 ${REDIS_BENCH} -h 127.0.0.1 -p 6800 \
    -t ping -n 100000 -c 100 -q 2>/dev/null) || true
arps=$(echo "${aresult}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
echo "  ASIO: ${arps} req/s"

kill ${APID} 2>/dev/null; wait ${APID} 2>/dev/null || true

echo ""
echo "=== Done ==="
