#!/bin/bash
# Linux MT loop verification — multiple rounds for statistical rigor
set -euo pipefail

CORONET_MT=/mnt/d/dev/workspace/yidaoyun/coronet/build-release/bench/redis_echo_MT
CO_CTX_MT=/mnt/d/dev/workspace/yidaoyun/co_context/build-release/example/redis_echo_MT
CONCS=(50 100 200 500)
TOTAL=100000
PORT=6379
ROUNDS=3

echo "============================================"
echo "  Linux MT Loop Verification ($ROUNDS rounds)"
echo "============================================"
echo ""

# Results accumulator
declare -A CORO_SUM CO_SUM CORO_CNT CO_CNT
for c in "${CONCS[@]}"; do CORO_SUM[$c]=0; CO_SUM[$c]=0; CORO_CNT[$c]=0; CO_CNT[$c]=0; done

for round in $(seq 1 $ROUNDS); do
    echo "======== Round $round / $ROUNDS ========"
    for conc in "${CONCS[@]}"; do
        echo -n "  c=$conc: "

        # coronet MT
        pkill -9 redis_echo_MT 2>/dev/null || true; sleep 1
        ${CORONET_MT} &
        sleep 3
        CR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
        CRPS=$(echo "${CR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
        pkill -9 redis_echo_MT 2>/dev/null || true; sleep 1

        # co_context MT
        pkill -9 redis_echo_MT 2>/dev/null || true; sleep 1
        ${CO_CTX_MT} &
        sleep 3
        AR=$(timeout 60 redis-benchmark -h 127.0.0.1 -p ${PORT} -t ping -n ${TOTAL} -c ${conc} -q 2>/dev/null) || true
        ARPS=$(echo "${AR}" | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
        pkill -9 redis_echo_MT 2>/dev/null || true; sleep 1

        echo "coronet=${CRPS:-0}  co_context=${ARPS:-0}"

        CORO_SUM[$conc]=$(echo "${CORO_SUM[$conc]} + ${CRPS:-0}" | bc)
        CO_SUM[$conc]=$(echo "${CO_SUM[$conc]} + ${ARPS:-0}" | bc)
        CORO_CNT[$conc]=$((CORO_CNT[$conc] + 1))
        CO_CNT[$conc]=$((CO_CNT[$conc] + 1))
    done
    echo ""
done

# Print averages
echo "============================================"
echo "  Average over $ROUNDS rounds"
echo "============================================"
printf "%-10s %15s %15s %10s\n" "Conc" "coronet MT" "co_context MT" "Ratio"
echo "----------------------------------------------------------"
for c in "${CONCS[@]}"; do
    CAVG=$(echo "scale=0; ${CORO_SUM[$c]} / ${CORO_CNT[$c]}" | bc)
    AAVG=$(echo "scale=0; ${CO_SUM[$c]} / ${CO_CNT[$c]}" | bc)
    if [ "${AAVG}" != "0" ]; then
        RATIO=$(echo "scale=2; ${CAVG} / ${AAVG}" | bc)
    else
        RATIO="N/A"
    fi
    printf "%-10s %15s %15s %10s\n" "$c" "$CAVG" "$AAVG" "$RATIO"
done
echo ""
echo "=== Done ==="
