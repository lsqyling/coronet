#!/bin/bash
# Three-project performance comparison: coronet vs co_context vs ASIO
# Usage: bash perf_cmp.sh <gcc|clang> <output_csv>
set -uo pipefail

COMPILER="$1"
OUTPUT="$2"
CORONET_DIR="/mnt/d/dev/workspace/yidaoyun/coronet"
COCTX_DIR="/mnt/d/dev/workspace/yidaoyun/co_context"

if [ "$COMPILER" = "clang" ]; then
    CORO="$CORONET_DIR/build-clang/bench"
    COCTX="$COCTX_DIR/build-clang/example"
else
    CORO="$CORONET_DIR/build-release/bench"
    COCTX="$COCTX_DIR/build-release/example"
fi

CONCS=(10 50 100 200 500)
TOTAL=100000
TIMEOUT=60

echo "server,type,concurrency,rps,stable" > "$OUTPUT"

cleanup() { pkill -9 -f redis_echo 2>/dev/null || true; sleep 1; }

run_st() {
    local label="$1" bin="$2" port="$3"
    for c in "${CONCS[@]}"; do
        cleanup
        $bin $port &>/dev/null & local pid=$!; sleep 2
        if ! kill -0 $pid 2>/dev/null; then echo "  [$label ST c=$c] CRASH"; echo "$label,ST,$c,0,0" >> "$OUTPUT"; continue; fi
        local rps=$(timeout $TIMEOUT redis-benchmark -h 127.0.0.1 -p $port -t ping -n $TOTAL -c $c -q 2>/dev/null | grep -oP '[\d.]+(?=\s+requests)' | tail -1 || echo "0")
        local stable=1; kill -0 $pid 2>/dev/null || stable=0
        echo "  [$label ST c=$c] RPS=$rps stable=$stable"
        echo "$label,ST,$c,$rps,$stable" >> "$OUTPUT"
        kill $pid 2>/dev/null; wait $pid 2>/dev/null
    done
}

run_mt() {
    local label="$1" bin="$2" port="$3"
    for c in "${CONCS[@]}"; do
        cleanup
        $bin $port 6 &>/dev/null & local pid=$!; sleep 3
        if ! kill -0 $pid 2>/dev/null; then echo "  [$label MT c=$c] CRASH"; echo "$label,MT,$c,0,0" >> "$OUTPUT"; continue; fi
        local rps=$(timeout $TIMEOUT redis-benchmark -h 127.0.0.1 -p $port -t ping -n $TOTAL -c $c -q 2>/dev/null | grep -oP '[\d.]+(?=\s+requests)' | tail -1 || echo "0")
        local stable=1; kill -0 $pid 2>/dev/null || stable=0
        echo "  [$label MT c=$c] RPS=$rps stable=$stable"
        echo "$label,MT,$c,$rps,$stable" >> "$OUTPUT"
        kill $pid 2>/dev/null; wait $pid 2>/dev/null
    done
}

PORT=9400
echo "=== $COMPILER ST ==="
run_st "coronet_${COMPILER}"     "$CORO/redis_echo_coro"     $((PORT++))
run_st "coctx_${COMPILER}"       "$COCTX/redis_echo"         $((PORT++))
run_st "asio_${COMPILER}"        "$CORO/redis_echo_asio"      $((PORT++))

echo "=== $COMPILER MT ==="
# MT servers hardcode port 6379 for coronet/coctx, asio takes port arg
run_mt "coronet_${COMPILER}_mt"  "$CORO/redis_echo_MT"       6379
run_mt "coctx_${COMPILER}_mt"    "$COCTX/redis_echo_MT"      6379
run_mt "asio_${COMPILER}_mt"     "$CORO/redis_echo_asio_MT"  6379

echo "=== $COMPILER DONE ==="
cat "$OUTPUT"
