#!/bin/bash
# Multi-threaded benchmark: coronet vs co_context vs ASIO (6 threads)
# All MT servers hardcode port 6379 — run sequentially.
# Usage: bash mt_bench.sh <build_dir_suffix> <label>
set -euo pipefail

CORONET_DIR="/mnt/d/dev/workspace/yidaoyun/coronet"
COCTX_DIR="/mnt/d/dev/workspace/yidaoyun/co_context"
BUILD_SUFFIX="$1"      # "release" or "clang"
LABEL="$2"              # "gcc" or "clang"

CORO_MT="$CORONET_DIR/build-${BUILD_SUFFIX}/bench/redis_echo_MT"
COCTX_MT="$COCTX_DIR/build-${BUILD_SUFFIX}/example/redis_echo_MT"
ASIO_MT="$CORONET_DIR/build-${BUILD_SUFFIX}/bench/redis_echo_asio_MT"

OUT="/tmp/bench_${LABEL}_mt.csv"
echo "server,env,c,requests_per_second" > "$OUT"
PORT=6379

run_mt() {
    local bin="$1" name="$2" extra="${3:-}"
    for c in 10 50 100 200 500; do
        $bin $PORT $extra &>/dev/null &
        local pid=$!
        sleep 4
        if ! kill -0 $pid 2>/dev/null; then
            echo "  [$name c=$c] CRASHED"
            echo "$name,$LABEL,$c,0" >> "$OUT"
            continue
        fi
        local rps
        rps=$(timeout 60 redis-benchmark -h 127.0.0.1 -p $PORT \
            -t ping -n 50000 -c $c -q 2>&1 \
            | sed -n 's/.*: \([0-9.]*\) requests per second.*/\1/p' | tail -1 || echo "0")
        echo "  [$name c=$c] rps=$rps"
        echo "$name,$LABEL,$c,$rps" >> "$OUT"
        kill $pid 2>/dev/null || true
        wait $pid 2>/dev/null || true
        sleep 2
    done
}

echo "=== $LABEL Multi-Threaded (6 threads, port $PORT) ==="
run_mt "$CORO_MT"  "coronet_mt"
run_mt "$COCTX_MT" "co_context_mt"
run_mt "$ASIO_MT"  "asio_mt" "6"

echo "=== $LABEL MT Done ==="
cat "$OUT"
