#!/bin/bash
# Single-threaded benchmark: coronet vs co_context vs ASIO
# Usage: bash st_bench.sh <build_dir_suffix> <label> <base_port>
set -euo pipefail

CORONET_DIR="/mnt/d/dev/workspace/yidaoyun/coronet"
COCTX_DIR="/mnt/d/dev/workspace/yidaoyun/co_context"
BUILD_SUFFIX="$1"      # "release" or "clang"
LABEL="$2"              # "gcc" or "clang"
BASE_PORT="$3"

CORO="$CORONET_DIR/build-${BUILD_SUFFIX}/bench/redis_echo_coro"
CHAIN="$CORONET_DIR/build-${BUILD_SUFFIX}/bench/redis_echo_chain"
COCTX="$COCTX_DIR/build-${BUILD_SUFFIX}/example/redis_echo"
ASIO="$CORONET_DIR/build-${BUILD_SUFFIX}/bench/redis_echo_asio"

OUT="/tmp/bench_${LABEL}_st.csv"
echo "server,env,c,requests_per_second" > "$OUT"

run_test() {
    local bin="$1" name="$2" port="$3"
    for c in 10 50 100 200 500; do
        $bin $port &>/dev/null &
        local pid=$!
        sleep 3
        if ! kill -0 $pid 2>/dev/null; then
            echo "  [$name c=$c] CRASHED"
            echo "$name,$LABEL,$c,0" >> "$OUT"
            continue
        fi
        local rps
        rps=$(timeout 60 redis-benchmark -h 127.0.0.1 -p $port \
            -t ping -n 50000 -c $c -q 2>&1 \
            | sed -n 's/.*: \([0-9.]*\) requests per second.*/\1/p' | tail -1 || echo "0")
        echo "  [$name c=$c] rps=$rps"
        echo "$name,$LABEL,$c,$rps" >> "$OUT"
        kill $pid 2>/dev/null || true
        wait $pid 2>/dev/null || true
        sleep 2
    done
}

echo "=== $LABEL Single-Threaded ==="
run_test "$CORO"  "coronet"       $((BASE_PORT))
run_test "$CHAIN" "coronet_chain" $((BASE_PORT + 1))
run_test "$COCTX" "co_context"    $((BASE_PORT + 2))
run_test "$ASIO"  "asio"          $((BASE_PORT + 3))

echo "=== $LABEL ST Done ==="
cat "$OUT"
