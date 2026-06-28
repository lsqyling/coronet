#!/bin/bash
# Windows quick benchmark — run directly from Git Bash
# Usage: bash script/windows/quick_bench.sh
set -e

BUILD="build-windows"
CORO="$BUILD/bench/redis_echo_coro.exe"
ASIO="$BUILD/bench/redis_echo_asio.exe"
LOADGEN="$BUILD/bench/redis_loadgen.exe"
DATA="data"
TS=$(date +%Y%m%d_%H%M%S)
RESULT="$DATA/win_bench_$TS"
mkdir -p "$RESULT"

CONCS=(10 50 100 200 500 1000)
TOTAL=100000

echo "============================================"
echo "  coronet vs ASIO - Windows Benchmark"
echo "============================================"
echo "Timestamp: $TS"
echo "Results:   $RESULT"
echo ""

echo "concurrency,coronet,asio" > "$RESULT/results.csv"

for conc in "${CONCS[@]}"; do
    echo "--- Concurrency: $conc ---"

    # coronet
    taskkill //f //im redis_echo_coro.exe 2>/dev/null || true
    sleep 1
    "$CORO" 7600 &
    CPID=$!
    sleep 2
    if kill -0 $CPID 2>/dev/null; then
        crps=$("$LOADGEN" -c $conc -n $TOTAL -p 7600 -h 127.0.0.1 2>&1 | grep -oP 'RPS:\s*\K[\d.]+' || echo "0")
        echo "  coronet: ${crps:-0} req/s"
    else
        crps=0
        echo "  coronet: CRASHED"
    fi
    taskkill //f //im redis_echo_coro.exe 2>/dev/null || true
    wait $CPID 2>/dev/null || true
    sleep 1

    # ASIO
    taskkill //f //im redis_echo_asio.exe 2>/dev/null || true
    sleep 1
    "$ASIO" 8600 &
    APID=$!
    sleep 2
    if kill -0 $APID 2>/dev/null; then
        arps=$("$LOADGEN" -c $conc -n $TOTAL -p 8600 -h 127.0.0.1 2>&1 | grep -oP 'RPS:\s*\K[\d.]+' || echo "0")
        echo "  ASIO:    ${arps:-0} req/s"
    else
        arps=0
        echo "  ASIO:    CRASHED"
    fi
    taskkill //f //im redis_echo_asio.exe 2>/dev/null || true
    wait $APID 2>/dev/null || true
    sleep 1

    echo "$conc,${crps:-0},${arps:-0}" >> "$RESULT/results.csv"
    echo ""
done

echo "============================================"
echo "  Results"
echo "============================================"
column -t -s, "$RESULT/results.csv"
echo ""
echo "Results: $RESULT/results.csv"
