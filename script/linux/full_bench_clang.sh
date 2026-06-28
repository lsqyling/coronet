#!/bin/bash
# Clang-specific: uses different BASE_PORT to avoid conflict with GCC
set -euo pipefail

CONCS=(10 50 100 200 500)
TOTAL=100000
BASE_PORT=7700  # different from GCC (7600)
TIMEOUT_SEC=60

CORONET_DIR="/mnt/d/dev/workspace/yidaoyun/coronet"
COCTX_DIR="/mnt/d/dev/workspace/yidaoyun/co_context"
CORONET_BUILD="$CORONET_DIR/build-clang/bench"
COCTX_BUILD="$COCTX_DIR/build-clang/example"
OUTPUT_CSV="/tmp/bench_clang.csv"

echo "============================================"
echo "  3-Environment Benchmark: CLANG"
echo "============================================"

echo "env,server,concurrency,rps" > "$OUTPUT_CSV"

run_test() {
    local label="$1"
    local server="$2"
    local port="$3"
    local is_mt="$4"
    local server_args="${5:-}"

    for c in "${CONCS[@]}"; do
        pkill -9 -f "$(basename "$server")" 2>/dev/null || true
        sleep 1

        if [ "$is_mt" = "mt" ]; then
            $server $port $server_args &>/dev/null &
        else
            $server $port &>/dev/null &
        fi
        SERVER_PID=$!
        sleep 3

        if ! kill -0 $SERVER_PID 2>/dev/null; then
            echo "  [$label c=$c] CRASHED"
            echo "clang,$label,$c,0" >> "$OUTPUT_CSV"
            continue
        fi

        RPS=$(timeout $TIMEOUT_SEC redis-benchmark -h 127.0.0.1 -p $port \
            -t ping -n $TOTAL -c $c -q 2>/dev/null \
            | grep -oP '[\d.]+(?=\s+requests\s+per\s+second)' | tail -1 || echo "0")

        echo "  [$label c=$c] RPS=${RPS:-0}"
        echo "clang,$label,$c,${RPS:-0}" >> "$OUTPUT_CSV"

        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
        sleep 1
    done
}

PORT=$BASE_PORT
echo "=== ST ==="
run_test "coronet_clang_st" "$CORONET_BUILD/redis_echo_coro" $PORT "st"; PORT=$((PORT + 1))
run_test "coronet_chain_clang_st" "$CORONET_BUILD/redis_echo_chain" $PORT "st"; PORT=$((PORT + 1))
run_test "coctx_clang_st" "$COCTX_BUILD/redis_echo" $PORT "st"; PORT=$((PORT + 1))
run_test "asio_clang_st" "$CORONET_BUILD/redis_echo_asio" $PORT "st"; PORT=$((PORT + 1))

echo "=== MT ==="
run_test "coronet_clang_mt" "$CORONET_BUILD/redis_echo_MT" $PORT "mt" "6"; PORT=$((PORT + 1))
run_test "coctx_clang_mt" "$COCTX_BUILD/redis_echo_MT" $PORT "mt"; PORT=$((PORT + 1))
run_test "asio_clang_mt" "$CORONET_BUILD/redis_echo_asio_MT" $PORT "mt"; PORT=$((PORT + 1))

echo "CLANG BENCHMARK COMPLETE"
echo "Results: $OUTPUT_CSV"
