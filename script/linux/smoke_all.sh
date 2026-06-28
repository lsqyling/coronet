#!/bin/bash
# Smoke test all coronet examples (robust version)
set -euo pipefail
cd "$(dirname "$0")/../.."
BIN="build-release/examples"
PASS=0; FAIL=0

# Kill any leftover servers
pkill -f "example_echo" 2>/dev/null || true
pkill -f "example_httpd" 2>/dev/null || true
pkill -f "example_netcat" 2>/dev/null || true
sleep 2

# Test: process stays alive for duration
test_alive() {
    local name="$1" port="${2:-0}" extra="${3:-}"
    echo -n "  $name ... "
    if [ "$port" -ne 0 ]; then
        $BIN/example_$name $port $extra &>/dev/null & local pid=$!
    else
        $BIN/example_$name $extra &>/dev/null & local pid=$!
    fi
    sleep 3
    if kill -0 $pid 2>/dev/null; then
        echo "OK"; PASS=$((PASS+1))
    else
        echo "CRASH"; FAIL=$((FAIL+1))
    fi
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    sleep 1
}

# Test: network server responds to redis-benchmark
test_net() {
    local name="$1" port="$2" extra="${3:-}"
    echo -n "  $name (port $port) ... "
    pkill -f "example_$name" 2>/dev/null || true; sleep 1
    if [ -n "$extra" ]; then
        $BIN/example_$name $port $extra &>/dev/null & local pid=$!
    else
        $BIN/example_$name $port &>/dev/null & local pid=$!
    fi
    sleep 2
    if ! kill -0 $pid 2>/dev/null; then echo "CRASH"; FAIL=$((FAIL+1)); return; fi
    if redis-benchmark -h 127.0.0.1 -p $port -t ping -n 5 -c 1 -q 2>/dev/null | grep -q "requests per second"; then
        echo "OK"; PASS=$((PASS+1))
    else
        echo "FAIL"; FAIL=$((FAIL+1))
    fi
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    sleep 1
}

# Test: process completes with exit 0
test_exit() {
    local name="$1" extra="${2:-}"
    echo -n "  $name ... "
    if timeout 5 $BIN/example_$name $extra >/dev/null 2>&1; then
        echo "OK"; PASS=$((PASS+1))
    else
        echo "FAIL"; FAIL=$((FAIL+1))
    fi
    sleep 1
}

echo "=== coronet Linux Smoke Tests ==="
echo ""

# Quick exit tests
echo "--- Quick (expect exit) ---"
test_exit iota

# Network servers
echo "--- Network ---"
test_net echo_server 9701
test_net echo_server_MT 6379
mkdir -p public; echo "<h1>hi</h1>" > public/index.html
test_net httpd 9703
test_net httpd_MT 9704

# Long-running (check alive)
echo "--- Long-running (process alive check) ---"
test_alive timer
test_alive mutex
test_alive channel
test_alive cv_notify_all
test_alive sem
test_alive when_all
test_alive when_any
test_alive when_some
test_alive timer_accuracy
test_alive netcat -l 9705

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
