#!/bin/bash
# Linux chain co_await comparison:
#   coronet/redis_echo_chain.cpp (chained)
#   coronet/redis_echo_coro.cpp  (non-chained)
#   co_context/redis_echo.cpp    (chained ref)
set -euo pipefail

CORO_CHAIN=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_chain
CORO_PLAIN=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_coro
CO_CTX=/mnt/d/dev/workspace/yidaoyun/co_context/build-release/example/redis_echo
CONCS=(10 50 100 200 500)
TOTAL=100000
PORT=7200

echo "============================================"
echo "  Chain co_await Comparison (Linux)"
echo "============================================"
echo ""

echo "conc,coronet_chain,coronet_plain,co_context" > /tmp/chain_cmp.csv

for conc in "${CONCS[@]}"; do
    echo "--- c=$conc ---"

    pkill -9 redis_echo 2>/dev/null || true; sleep 1
    ${CORO_CHAIN} ${PORT} &
    sleep 2
    R1=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  coronet chain:   ${R1:-0} req/s"
    pkill -9 redis_echo 2>/dev/null || true; sleep 1

    ${CORO_PLAIN} ${PORT} &
    sleep 2
    R2=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  coronet plain:   ${R2:-0} req/s"
    pkill -9 redis_echo 2>/dev/null || true; sleep 1

    ${CO_CTX} ${PORT} &
    sleep 2
    R3=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  co_context chain: ${R3:-0} req/s"
    pkill -9 redis_echo 2>/dev/null || true; sleep 1

    echo "${conc},${R1:-0},${R2:-0},${R3:-0}" >> /tmp/chain_cmp.csv
    echo ""
done

echo "============================================"
echo "  Results"
echo "============================================"
printf "%-8s %16s %16s %16s\n" "c" "coronet chain" "coronet plain" "co_context chain"
echo "----------------------------------------------------------------"
tail -n +2 /tmp/chain_cmp.csv | while IFS=, read c r1 r2 r3; do
    printf "%-8s %16s %16s %16s\n" "$c" "$r1" "$r2" "$r3"
done
