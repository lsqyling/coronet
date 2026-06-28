#!/bin/bash
# Quick comparison benchmark: coronet vs ASIO
cd "$(dirname "$0")/.."
mkdir -p data
rm -f data/comparison_results.csv

echo "coronet vs ASIO Redis PING Benchmark"
echo "======================================"

for conc in 10 50 100 200; do
    echo ""
    echo "--- Concurrency: ${conc} (100000 req) ---"

    # Test coronet
    ./build-release/bench/redis_echo_coro 6600 2>/dev/null &
    CPID=$!
    sleep 2
    CRESULT=$(timeout 60 redis-benchmark -h 127.0.0.1 -p 6600 -t ping -n 100000 -c ${conc} -q 2>/dev/null) || true
    CRPS=$(echo "${CRESULT}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' || echo "0")
    [ -z "${CRPS}" ] && CRPS=0
    kill $CPID 2>/dev/null || true
    wait $CPID 2>/dev/null || true
    echo "  coronet: ${CRPS} req/s"

    # Test ASIO
    ./build-release/bench/redis_echo_asio 6601 2>/dev/null &
    APID=$!
    sleep 2
    ARESULT=$(timeout 60 redis-benchmark -h 127.0.0.1 -p 6601 -t ping -n 100000 -c ${conc} -q 2>/dev/null) || true
    ARPS=$(echo "${ARESULT}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' || echo "0")
    [ -z "${ARPS}" ] && ARPS=0
    kill $APID 2>/dev/null || true
    wait $APID 2>/dev/null || true
    echo "  ASIO:    ${ARPS} req/s"

    echo "${conc},${CRPS},${ARPS}" >> data/comparison_results.csv
done

echo ""
echo "=========== Results ==========="
printf "%-10s %15s %15s\n" "Conc" "coronet" "ASIO"
echo "-------------------------------------"
while IFS=, read -r c cr ar; do
    printf "%-10s %15s %15s\n" "$c" "$cr" "$ar"
done < data/comparison_results.csv
