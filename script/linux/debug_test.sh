#!/bin/bash
# Debug test - find crash root cause
set -e
cd "$(dirname "$0")/.."
BIN=./build-release/bench/redis_echo_coro

echo "=== Enabling core dumps ==="
ulimit -c unlimited
echo "Core pattern: $(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo default)"

echo ""
echo "=== Test 1: c=1 n=100 ==="
${BIN} 7201 &
PID=$!
sleep 2
if kill -0 ${PID} 2>/dev/null; then
    echo "Server running (PID=${PID})"
    redis-benchmark -h 127.0.0.1 -p 7201 -t ping -n 100 -c 1 -q
else
    echo "CRASHED at startup"
fi
kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null; sleep 1

echo ""
echo "=== Test 2: c=10 n=10000 ==="
${BIN} 7202 &
PID=$!
sleep 2
if kill -0 ${PID} 2>/dev/null; then
    echo "Server running (PID=${PID})"
    redis-benchmark -h 127.0.0.1 -p 7202 -t ping -n 10000 -c 10 -q
else
    echo "CRASHED at startup"
fi
kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null; sleep 1

echo ""
echo "=== Test 3: c=100 n=100000 ==="
${BIN} 7203 &
PID=$!
sleep 2
if kill -0 ${PID} 2>/dev/null; then
    echo "Server running (PID=${PID})"
    timeout 30 redis-benchmark -h 127.0.0.1 -p 7203 -t ping -n 100000 -c 100 -q || echo "TIMEOUT/FAIL"
else
    echo "CRASHED at startup"
fi
kill ${PID} 2>/dev/null; wait ${PID} 2>/dev/null

echo ""
echo "=== Done ==="
ls -la core* 2>/dev/null || echo "No core dumps"
