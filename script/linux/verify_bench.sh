#!/bin/bash
# Linux verification benchmark: coronet vs ASIO after cross-platform refactoring
set -euo pipefail

CORO=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_coro
ASIO=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_asio
CONCS=(10 50 100 200 500 1000)
TOTAL=100000

echo "============================================"
echo "  Linux Verification: coronet vs ASIO"
echo "============================================"
echo ""

echo "concurrency,coronet,asio" > /tmp/linux_verify.csv

for conc in "${CONCS[@]}"; do
    echo "--- c=$conc ---"

    # coronet
    pkill -9 redis_echo_coro 2>/dev/null || true; sleep 1
    ${CORO} 7200 &
    sleep 2
    CR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p 7200 -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
    CRPS=$(echo "${CR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  coronet: ${CRPS:-0} req/s"
    kill %1 2>/dev/null; wait 2>/dev/null || true; sleep 1

    # ASIO
    pkill -9 redis_echo_asio 2>/dev/null || true; sleep 1
    ${ASIO} 8200 &
    sleep 2
    AR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p 8200 -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
    ARPS=$(echo "${AR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
    echo "  ASIO:    ${ARPS:-0} req/s"
    kill %1 2>/dev/null; wait 2>/dev/null || true; sleep 1

    echo "${conc},${CRPS:-0},${ARPS:-0}" >> /tmp/linux_verify.csv
    echo ""
done

echo "============================================"
echo "  Results"
echo "============================================"
printf "%-10s %15s %15s %10s\n" "Conc" "coronet" "ASIO" "Ratio"
echo "----------------------------------------------------------"
tail -n +2 /tmp/linux_verify.csv | while IFS=, read c cr ar; do
    if [ "${ar}" != "0" ]; then
        ratio=$(awk "BEGIN {printf \"%.2f\", ${cr}/${ar}}")
    else
        ratio="N/A"
    fi
    printf "%-10s %15s %15s %10s\n" "${c}" "${cr}" "${ar}" "${ratio}"
done
