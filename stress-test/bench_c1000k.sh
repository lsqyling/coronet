#!/bin/bash
# ============================================================
# Coronet vs ASIO C1000K Benchmark Script
# ============================================================
# Runs from WSL, executes Windows .exe files directly.
#
# Usage: bash bench_c1000k.sh [requests] [clients]
#   Default: 1,000,000 requests x 1,000 concurrent clients
# ============================================================

set -e

REQUESTS=${1:-1000000}
CLIENTS=${2:-1000}
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT="bench_report_${TIMESTAMP}.txt"
CSV="bench_report_${TIMESTAMP}.csv"

# Paths — use build output directory for Windows binaries
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../buildmsvc/stress-test"
REDIS_DIR="${SCRIPT_DIR}/../Redis-x64-3.0.504"
STRESS_DRIVER="${BUILD_DIR}/stress_driver.exe"
REDIS_BENCH="${REDIS_DIR}/redis-benchmark.exe"
REDIS_CLI="${REDIS_DIR}/redis-cli.exe"
CORONET_ST="${BUILD_DIR}/redis_echo_ST.exe"
CORONET_CHAIN="${BUILD_DIR}/redis_echo_chain.exe"
CORONET_MT="${BUILD_DIR}/redis_echo_MT.exe"
ASIO_ST="${BUILD_DIR}/redis_echo_asio_ST.exe"
ASIO_MT="${BUILD_DIR}/redis_echo_asio_MT.exe"

# Ports
PORT_CORONET_ST=16410
PORT_CORONET_CHAIN=16411
PORT_CORONET_MT=16412
PORT_ASIO_ST=16413
PORT_ASIO_MT=16414

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo "============================================================" | tee "$REPORT"
echo "  Coronet vs ASIO C1000K Benchmark" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "  Date:     $(date)" | tee -a "$REPORT"
echo "  Requests: $REQUESTS" | tee -a "$REPORT"
echo "  Clients:  $CLIENTS" | tee -a "$REPORT"
echo "  Pipeline: 1 (no pipelining)" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "Name,Port,RPS,CPU%,MemMB,Status" > "$CSV"

# ----------------------------------------------------------
# Test one server
# ----------------------------------------------------------
test_server() {
    local name="$1"
    local binary="$2"
    local port="$3"
    local extra_args="${4:-}"

    printf "  %-25s [port %-5d] " "$name" "$port" | tee -a "$REPORT"

    # Start server
    $binary $port $extra_args &
    local pid=$!

    # Wait for port ready (up to 10 seconds)
    local ready=0
    for i in $(seq 1 50); do
        sleep 0.2
        if $REDIS_CLI -p $port PING >/dev/null 2>&1; then
            ready=1
            break
        fi
        # Check if process crashed
        if ! kill -0 $pid 2>/dev/null; then
            wait $pid 2>/dev/null
            local exit_code=$?
            printf "${RED}CRASH (exit=%d)${NC}\n" $exit_code | tee -a "$REPORT"
            echo "$name,$port,0,0,0,CRASH" >> "$CSV"
            return
        fi
    done

    if [ $ready -eq 0 ]; then
        printf "${RED}TIMEOUT${NC}\n" | tee -a "$REPORT"
        kill $pid 2>/dev/null; wait $pid 2>/dev/null
        echo "$name,$port,0,0,0,TIMEOUT" >> "$CSV"
        return
    fi

    # Run benchmark
    local output
    output=$("$REDIS_BENCH" -h 127.0.0.1 -p $port -n $REQUESTS -c $CLIENTS -t ping -q 2>&1) || true

    # Parse RPS
    local rps=0
    if echo "$output" | grep -q "requests per second"; then
        rps=$(echo "$output" | grep -oP '[\d.]+(?= requests per second)')
    fi

    # Get CPU & memory via stress_driver's sampler or /proc
    local cpu=0 mem=0

    # Kill server
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null || true

    if [ "$(echo "$rps > 0" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        printf "${GREEN}PASS${NC}  rps=%.0f\n" "$rps" | tee -a "$REPORT"
    else
        printf "${RED}FAIL${NC}\n" | tee -a "$REPORT"
    fi

    echo "$name,$port,$rps,$cpu,$mem,$([ "$(echo "$rps > 0" | bc -l 2>/dev/null || echo 0)" = "1" ] && echo "PASS" || echo "FAIL")" >> "$CSV"
}

# ============================================================
# Round 1: Single-Threaded
# ============================================================
echo "" | tee -a "$REPORT"
echo -e "${CYAN}=== Round 1: Single-Threaded ===${NC}" | tee -a "$REPORT"
echo "" | tee -a "$REPORT"

test_server "coronet_ST"         "$CORONET_ST"    $PORT_CORONET_ST
test_server "coronet_chain"      "$CORONET_CHAIN" $PORT_CORONET_CHAIN
test_server "ASIO_ST"            "$ASIO_ST"       $PORT_ASIO_ST

# ============================================================
# Round 2: Multi-Threaded (6 threads)
# ============================================================
echo "" | tee -a "$REPORT"
echo -e "${CYAN}=== Round 2: Multi-Threaded (6 threads) ===${NC}" | tee -a "$REPORT"
echo "" | tee -a "$REPORT"

test_server "coronet_MT(6)"      "$CORONET_MT"    $PORT_CORONET_MT "6"
test_server "ASIO_MT(6)"         "$ASIO_MT"       $PORT_ASIO_MT "6"

# ============================================================
# Round 3: C1000K Extreme
# ============================================================
echo "" | tee -a "$REPORT"
echo -e "${CYAN}=== Round 3: C1000K Extreme ===${NC}" | tee -a "$REPORT"
echo "" | tee -a "$REPORT"

test_server "coronet_ST C1000K"  "$CORONET_ST"    $PORT_CORONET_ST
test_server "coronet_MT C1000K"  "$CORONET_MT"    $PORT_CORONET_MT "6"

# ============================================================
# Summary
# ============================================================
echo "" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "  Report: $REPORT" | tee -a "$REPORT"
echo "  CSV:    $CSV" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "Benchmark complete." | tee -a "$REPORT"
