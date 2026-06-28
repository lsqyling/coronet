#!/bin/bash
# Build + run ALL coronet tests on Linux
set -euo pipefail
cd "$(dirname "$0")/../.."
PASS=0; FAIL=0

run_test() {
    local name="$1" bin="$2" timeout_sec="${3:-10}"
    echo -n "  $name ... "
    if timeout $timeout_sec $bin >/dev/null 2>&1; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "FAIL"; FAIL=$((FAIL+1))
    fi
}

echo "=== Building ==="
cmake --build build-release 2>&1 | grep -c "error:" || true

echo "=== GCC Tests ==="
for t in task_gtest generator_gtest proactor_gtest channel_gtest shared_task_gtest \
         ft_task coro_lifetime generator_test move_shared_task stress_test; do
    bin="build-release/test/$t"
    [ -f "$bin" ] && run_test "$t" "$bin" 15 || echo "  $t SKIP (not built)"
done

echo "=== echo_server (GCC) ==="
build-release/test/echo_server 9090 &
PID=$!; sleep 1
if kill -0 $PID 2>/dev/null; then echo "  echo_server PASS"; PASS=$((PASS+1))
else echo "  echo_server FAIL"; FAIL=$((FAIL+1)); fi
kill $PID 2>/dev/null; wait $PID 2>/dev/null

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
