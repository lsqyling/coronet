#!/bin/bash
# Three-environment comprehensive benchmark: GCC + Clang (sequential)
# coronet ST/chain, co_context ST, ASIO ST + MT variants
set -euo pipefail

CORONET_DIR="/mnt/d/dev/workspace/yidaoyun/coronet"
COCTX_DIR="/mnt/d/dev/workspace/yidaoyun/co_context"
CONCS=(10 50 100 200 500)
TOTAL=100000
TIMEOUT_SEC=60

run_one() {
    local label="$1" server="$2" port="$3" is_mt="$4" extra="${5:-}"
    local csv="/tmp/bench_${label%%_*}.csv"

    for c in "${CONCS[@]}"; do
        # Kill by port (precise, no cross-conflict)
        fuser -k ${port}/tcp 2>/dev/null || true
        sleep 1

        # Start server with stdout/stderr to /dev/null
        if [ "$is_mt" = "mt" ]; then
            $server $port $extra &>/dev/null &
        else
            $server $port &>/dev/null &
        fi
        PID=$!
        sleep 3

        if ! kill -0 $PID 2>/dev/null; then
            echo "  [${label}] c=$c CRASH"
            echo "${label},${c},0" >> "$csv"
            continue
        fi

        RPS=$(timeout $TIMEOUT_SEC redis-benchmark -h 127.0.0.1 -p $port \
            -t ping -n $TOTAL -c $c -q 2>/dev/null \
            | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")
        echo "  [${label}] c=$c  RPS=${RPS:-0}"
        echo "${label},${c},${RPS:-0}" >> "$csv"

        kill $PID 2>/dev/null || true
        wait $PID 2>/dev/null || true
        sleep 1
    done
}

echo "============================================"
echo "  GCC BENCHMARK"
echo "============================================"

CORO_BIN="$CORONET_DIR/build-release/bench"
COCTX_BIN="$COCTX_DIR/build-release/example"

echo "server,concurrency,rps" > /tmp/bench_gcc.csv
echo "server,concurrency,rps" > /tmp/bench_clang.csv

# GCC ST
run_one "coronet_gcc_st"      "$CORO_BIN/redis_echo_coro"        7701 "st"
run_one "coronet_chain_gcc_st" "$CORO_BIN/redis_echo_chain"      7702 "st"
run_one "coctx_gcc_st"        "$COCTX_BIN/redis_echo"            7703 "st"
run_one "asio_gcc_st"         "$CORO_BIN/redis_echo_asio"        7704 "st"

# GCC MT
run_one "coronet_gcc_mt"      "$CORO_BIN/redis_echo_MT"          7705 "mt" "6"
run_one "coctx_gcc_mt"        "$COCTX_BIN/redis_echo_MT"         7706 "mt"
run_one "asio_gcc_mt"         "$CORO_BIN/redis_echo_asio_MT"     7707 "mt"

echo ""
echo "============================================"
echo "  CLANG BENCHMARK"
echo "============================================"

CORO_BIN="$CORONET_DIR/build-clang/bench"
COCTX_BIN="$COCTX_DIR/build-clang/example"

# Clang ST
run_one "coronet_clang_st"      "$CORO_BIN/redis_echo_coro"      7801 "st"
run_one "coronet_chain_clang_st" "$CORO_BIN/redis_echo_chain"    7802 "st"
run_one "coctx_clang_st"        "$COCTX_BIN/redis_echo"          7803 "st"
run_one "asio_clang_st"         "$CORO_BIN/redis_echo_asio"      7804 "st"

# Clang MT
run_one "coronet_clang_mt"      "$CORO_BIN/redis_echo_MT"        7805 "mt" "6"
run_one "coctx_clang_mt"        "$COCTX_BIN/redis_echo_MT"       7806 "mt"
run_one "asio_clang_mt"         "$CORO_BIN/redis_echo_asio_MT"   7807 "mt"

echo ""
echo "============================================"
echo "  ALL BENCHMARKS COMPLETE"
echo "  GCC: /tmp/bench_gcc.csv"
echo "  CLANG: /tmp/bench_clang.csv"
echo "============================================"
