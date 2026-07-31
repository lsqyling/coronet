#!/bin/bash
# ============================================================
# Coronet vs ASIO C1000K Benchmark Script (Linux / WSL)
# ============================================================
# Copy to build output dir during CMake build. Run directly:
#   cd build-release/stress-test
#   bash bench_c1000k.sh
#   bash bench_c1000k.sh 2000000 2000    # custom requests/clients
# ============================================================

set -e

REQUESTS=${1:-1000000}
CLIENTS=${2:-1000}
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT="bench_report_${TIMESTAMP}.txt"
CSV="bench_report_${TIMESTAMP}.csv"

# Script is copied to build/script/linux/ — find binaries in build/stress-test/
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Find build root: search upward for a directory containing "stress-test/"
BUILD_DIR="$SCRIPT_DIR"
while [ "$BUILD_DIR" != "/" ] && [ ! -d "$BUILD_DIR/stress-test" ]; do
    BUILD_DIR="$(dirname "$BUILD_DIR")"
done
# Binary directory is build/stress-test/
BIN_DIR="${BUILD_DIR}/stress-test"

# Find repo root by searching upward for "redistools"
REPO_ROOT="$BUILD_DIR"
while [ "$REPO_ROOT" != "/" ] && [ ! -d "$REPO_ROOT/redistools" ]; do
    REPO_ROOT="$(dirname "$REPO_ROOT")"
done

REDIS_DIR="${REPO_ROOT}/redistools"

# ---- redis-benchmark: prefer redistools, fallback to system PATH ----
REDIS_BENCH=""
if [ -f "${REDIS_DIR}/redis-benchmark" ]; then
    chmod +x "${REDIS_DIR}/redis-benchmark" 2>/dev/null || true
    REDIS_BENCH="${REDIS_DIR}/redis-benchmark"
elif command -v redis-benchmark >/dev/null 2>&1; then
    REDIS_BENCH="$(command -v redis-benchmark)"
else
    echo "ERROR: redis-benchmark not found (tried redistools/ and system PATH)"
    exit 1
fi

# ---- redis-cli: prefer redistools, fallback to system PATH ----
REDIS_CLI=""
if [ -f "${REDIS_DIR}/redis-cli" ]; then
    chmod +x "${REDIS_DIR}/redis-cli" 2>/dev/null || true
    REDIS_CLI="${REDIS_DIR}/redis-cli"
elif command -v redis-cli >/dev/null 2>&1; then
    REDIS_CLI="$(command -v redis-cli)"
else
    echo "ERROR: redis-cli not found (tried redistools/ and system PATH)"
    exit 1
fi

echo "Build dir:  $BUILD_DIR"
echo "Redis dir:  $REDIS_DIR"
echo ""

# Ports
PORT_CORONET_ST=16500
PORT_CORONET_CHAIN=16501
PORT_ASIO_ST=16502
PORT_CORONET_MT=16510
PORT_ASIO_MT=16511

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
echo "  Build:    $BUILD_DIR" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "Name,Port,RPS,CPU%,MemMB,Status,ElapsedSec" > "$CSV"

# ----------------------------------------------------------
# Test one server
# ----------------------------------------------------------
test_server() {
    local name="$1"
    local binary="$2"
    local port="$3"
    local extra_args="${4:-}"

    printf "  %-25s [port %-5d] " "$name" "$port" | tee -a "$REPORT"

    if [ ! -f "$binary" ]; then
        printf "SKIP (binary not found)\n" | tee -a "$REPORT"
        echo "$name,$port,0,0,0,SKIP,0" >> "$CSV"
        return
    fi

    # Start server
    $binary $port $extra_args &
    local pid=$!
    local start_ts=$(date +%s)

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
            wait $pid 2>/dev/null || true
            local exit_code=$?
            printf "${RED}CRASH (exit=%d)${NC}\n" $exit_code | tee -a "$REPORT"
            echo "$name,$port,0,0,0,CRASH,0" >> "$CSV"
            return
        fi
    done

    if [ $ready -eq 0 ]; then
        printf "${RED}TIMEOUT${NC}\n" | tee -a "$REPORT"
        kill $pid 2>/dev/null; wait $pid 2>/dev/null || true
        echo "$name,$port,0,0,0,TIMEOUT,0" >> "$CSV"
        return
    fi

    # Run benchmark
    local bench_start=$(date +%s%N)
    local output
    output=$("$REDIS_BENCH" -h 127.0.0.1 -p $port -n $REQUESTS -c $CLIENTS -t ping -q 2>&1) || true
    local bench_end=$(date +%s%N)
    local elapsed=$(echo "scale=1; ($bench_end - $bench_start) / 1000000000" | bc)

    # Parse RPS
    local rps=0
    if echo "$output" | grep -q "requests per second"; then
        rps=$(echo "$output" | grep -oP '[\d.]+(?= requests per second)' | head -1)
    fi

    # Sample CPU/memory
    local cpu=0 mem=0
    if [ -d /proc ]; then
        # RSS in MB
        mem=$(awk '/VmRSS/ {printf "%.0f", $2/1024}' /proc/$pid/status 2>/dev/null || echo 0)
        # CPU% via /proc — rough estimate
        if [ -f /proc/$pid/stat ]; then
            local utime=$(awk '{print $14}' /proc/$pid/stat 2>/dev/null)
            local stime=$(awk '{print $15}' /proc/$pid/stat 2>/dev/null)
            local total_time=$(($(date +%s) - start_ts))
            local clk_tck=$(getconf CLK_TCK 2>/dev/null || echo 100)
            if [ -n "$utime" ] && [ -n "$stime" ] && [ "$total_time" -gt 0 ]; then
                cpu=$(echo "scale=1; ($utime + $stime) / $clk_tck / $total_time * 100" | bc 2>/dev/null || echo 0)
            fi
        fi
    fi

    # Kill server
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null || true

    if [ -n "$rps" ] && awk "BEGIN { exit !($rps > 0) }" 2>/dev/null; then
        printf "${GREEN}PASS${NC}  rps=%s  cpu=%s%%  mem=%sMB  time=%ss\n" \
            "$(printf "%.0f" "$rps")" "$cpu" "$mem" "$elapsed" | tee -a "$REPORT"
    else
        printf "${RED}FAIL${NC} (elapsed=%ss)\n" "$elapsed" | tee -a "$REPORT"
    fi

    echo "$name,$port,$(printf "%.0f" "$rps"),$cpu,$mem,$([ -n "$rps" ] && awk "BEGIN { exit !($rps > 0) }" 2>/dev/null && echo "PASS" || echo "FAIL"),$elapsed" >> "$CSV"
}

# ============================================================
# Round 1: Single-Threaded
# ============================================================
echo "" | tee -a "$REPORT"
echo -e "${CYAN}=== Round 1: Single-Threaded ===${NC}" | tee -a "$REPORT"
echo "" | tee -a "$REPORT"

test_server "coronet_ST"         "${BIN_DIR}/redis_echo_ST"        $PORT_CORONET_ST
test_server "coronet_chain"      "${BIN_DIR}/redis_echo_chain"     $PORT_CORONET_CHAIN
test_server "ASIO_ST"            "${BIN_DIR}/redis_echo_asio_ST"   $PORT_ASIO_ST

# ============================================================
# Round 2: Multi-Threaded (6 threads)
# ============================================================
echo "" | tee -a "$REPORT"
echo -e "${CYAN}=== Round 2: Multi-Threaded (6 threads) ===${NC}" | tee -a "$REPORT"
echo "" | tee -a "$REPORT"

test_server "coronet_MT(6)"      "${BIN_DIR}/redis_echo_MT"  $PORT_CORONET_MT "6"
test_server "ASIO_MT(6)"         "${BIN_DIR}/redis_echo_asio_MT" $PORT_ASIO_MT "6"

# ============================================================
# Summary
# ============================================================
echo "" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "  Report: $REPORT" | tee -a "$REPORT"
echo "  CSV:    $CSV" | tee -a "$REPORT"
echo "============================================================" | tee -a "$REPORT"
echo "Benchmark complete." | tee -a "$REPORT"
