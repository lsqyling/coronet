#!/bin/bash
# coronet MT vs co_context MT — Linux comparison
set -euo pipefail

CORONET_MT=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_MT
CO_CTX_MT=/mnt/d/dev/workspace/yidaoyun/co_context/build-release/example/redis_echo_MT
CONCS=(50 100 200 500 1000)
TOTAL=100000
PORT=6379
TIMEOUT=60

echo "============================================"
echo "  coronet MT vs co_context MT (6 threads)"
echo "============================================"
echo ""

for conc in "${CONCS[@]}"; do
    echo "--- c=$conc ---"

    # coronet MT
    pkill -9 redis_echo_MT 2>/dev/null || true; sleep 2
    ${CORONET_MT} &
    sleep 3
    CR=$(timeout ${TIMEOUT} redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
    CRPS=$(echo "${CR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  coronet MT:   ${CRPS:-0} req/s"
    pkill -9 redis_echo_MT 2>/dev/null || true; sleep 2

    # co_context MT
    pkill -9 redis_echo_MT 2>/dev/null || true; sleep 2
    ${CO_CTX_MT} &
    sleep 3
    AR=$(timeout ${TIMEOUT} redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
    ARPS=$(echo "${AR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  co_context MT: ${ARPS:-0} req/s"
    pkill -9 redis_echo_MT 2>/dev/null || true; sleep 2

    echo ""
done

echo "=== Done ==="
