#!/bin/bash
# Full comparison: coronet vs ASIO
set -e
cd "$(dirname "$0")/.."
rm -f data/final_comparison.csv

CORO=./build-release/bench/redis_echo_coro
ASIO=./build-release/bench/redis_echo_asio

echo "============================================"
echo "  coronet vs ASIO - Final Comparison"
echo "============================================"
echo "concurrency,coronet_rps,asio_rps" > data/final_comparison.csv

for conc in 10 50 100 200 500 1000; do
    echo ""
    echo "--- Concurrency: ${conc} (100000 req) ---"

    # coronet
    ${CORO} $((7000 + conc)) 2>/dev/null &
    CPID=$!
    for i in $(seq 1 10); do
        timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/$((7000+conc))" 2>/dev/null && break
        sleep 0.5
    done
    CR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p $((7000+conc)) -t ping -n 100000 -c ${conc} -q 2>/dev/null || true)
    CRPS=$(echo "${CR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | head -1 || echo "0")
    kill ${CPID} 2>/dev/null; wait ${CPID} 2>/dev/null || true
    echo "  coronet: ${CRPS:-0} req/s"

    # ASIO
    ${ASIO} $((8000 + conc)) 2>/dev/null &
    APID=$!
    sleep 2
    AR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p $((8000+conc)) -t ping -n 100000 -c ${conc} -q 2>/dev/null || true)
    ARPS=$(echo "${AR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | head -1 || echo "0")
    kill ${APID} 2>/dev/null; wait ${APID} 2>/dev/null || true
    echo "  ASIO:    ${ARPS:-0} req/s"

    echo "${conc},${CRPS:-0},${ARPS:-0}" >> data/final_comparison.csv
    sleep 1
done

echo ""
echo "=========== Final Results ==========="
printf "%-10s %15s %15s %10s\n" "Conc" "coronet (rps)" "ASIO (rps)" "Ratio"
echo "----------------------------------------------------"
while IFS=, read c cr ar; do
    ratio="N/A"
    [ "${ar}" != "0" ] && ratio=$(awk "BEGIN {printf \"%.2f\", ${cr}/${ar}}")
    printf "%-10s %15s %15s %10s\n" "${c}" "${cr}" "${ar}" "${ratio}"
done < data/final_comparison.csv

echo ""
echo "Results: data/final_comparison.csv"
