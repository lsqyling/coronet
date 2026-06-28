#!/bin/bash
# Step-by-step benchmark: start small, scale up
set -e
cd "$(dirname "$0")/.."

BIN=./build-release/bench/redis_echo_coro
ASIO=./build-release/bench/redis_echo_asio

echo "============================================"
echo "  coronet Step-by-Step Benchmark"
echo "============================================"

for conc in 1 10 50 100 200; do
    reqs=1000
    [ $conc -ge 10 ] && reqs=10000
    [ $conc -ge 50 ] && reqs=50000
    [ $conc -ge 100 ] && reqs=100000

    echo ""
    echo "--- Step: c=${conc} n=${reqs} ---"

    PORT=$((6700 + conc))
    ${BIN} ${PORT} 2>/dev/null &
    PID=$!

    # Wait for port to be ready
    for i in $(seq 1 15); do
        if timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/${PORT}" 2>/dev/null; then
            break
        fi
        sleep 0.3
    done

    if ! kill -0 ${PID} 2>/dev/null; then
        echo "  coronet: CRASHED at startup"
        continue
    fi

    RESULT=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} \
        -t ping -n ${reqs} -c ${conc} -q 2>/dev/null) || true
    RPS=$(echo "${RESULT}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | head -1 || echo "0")
    [ -z "${RPS}" ] && RPS=0
    echo "  coronet: ${RPS} req/s (n=${reqs}, c=${conc})"

    kill ${PID} 2>/dev/null || true
    wait ${PID} 2>/dev/null || true
    sleep 0.5
done

echo ""
echo "--- ASIO baseline c=100 n=100000 ---"
${ASIO} 6800 2>/dev/null &
APID=$!
sleep 2
ARESULT=$(timeout 30 redis-benchmark -h 127.0.0.1 -p 6800 -t ping -n 100000 -c 100 -q 2>/dev/null) || true
ARPS=$(echo "${ARESULT}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | head -1 || echo "0")
echo "  ASIO: ${ARPS} req/s"
kill ${APID} 2>/dev/null || true
wait ${APID} 2>/dev/null || true

echo ""
echo "=== Done ==="
