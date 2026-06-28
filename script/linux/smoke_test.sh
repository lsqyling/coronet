#!/bin/bash
# ============================================================
# Quick smoke test — verify both servers work
# Usage: bash script/linux/smoke_test.sh
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJ_DIR}/build-release"
CO_SERVER="${BUILD_DIR}/bench/redis_echo_coro"
ASIO_SERVER="${BUILD_DIR}/bench/redis_echo_asio"

REDIS_BENCH="${REDIS_BENCH:-redis-benchmark}"

# Kill stale servers
pkill -9 redis_echo_coro 2>/dev/null || true
pkill -9 redis_echo_asio 2>/dev/null || true
sleep 1

echo "=== coronet Smoke Test ==="
echo ""

# ---- coronet ----
echo "--- coronet (coroutine) ---"
${CO_SERVER} 7701 &
PID=$!
sleep 2
if kill -0 ${PID} 2>/dev/null; then
    ${REDIS_BENCH} -h 127.0.0.1 -p 7701 -t ping -n 200 -c 10 -q 2>/dev/null
else
    echo "FAILED to start coronet server"
fi
kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null || true
sleep 1

# ---- ASIO ----
echo "--- ASIO (callback) ---"
${ASIO_SERVER} 7702 &
PID=$!
sleep 2
if kill -0 ${PID} 2>/dev/null; then
    ${REDIS_BENCH} -h 127.0.0.1 -p 7702 -t ping -n 200 -c 10 -q 2>/dev/null
else
    echo "FAILED to start ASIO server"
fi
kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null || true

echo ""
echo "=== Smoke test complete ==="
