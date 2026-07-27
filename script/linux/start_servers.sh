#!/bin/bash
# start_servers.sh — 后台启动压测服务端并等待端口就绪
# Usage: start_servers.sh <mode> <port_base>
#   mode: st | mt | all
#
# 服务端在后台运行，PID 写入 /tmp/stress_<name>.pid
# 退出码 0 = 全部就绪, 非 0 = 启动超时

set -e
MODE="${1:-st}"
PORT_BASE="${2:-6380}"
MAX_WAIT=15  # 每个端口最多等 15 秒

start_one() {
    local name="$1" bin="$2" port="$3"
    echo -n "  $name [port $port] ... "
    if [ ! -x "$bin" ]; then
        echo "SKIP (binary not found: $bin)"
        return 0
    fi
    "$bin" "$port" &
    local pid=$!
    echo "$pid" > "/tmp/stress_${name}.pid"
    # 等待端口就绪 / Wait for port ready
    for i in $(seq 1 $MAX_WAIT); do
        if redis-cli -p "$port" ping 2>/dev/null | grep -q PONG; then
            echo "OK (pid=$pid, ${i}x0.2s)"
            return 0
        fi
        sleep 0.2
    done
    echo "TIMEOUT — killing $pid"
    kill "$pid" 2>/dev/null
    return 1
}

echo "=== Starting stress servers (mode=$MODE) ==="
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$MODE" = "st" ] || [ "$MODE" = "all" ]; then
    start_one "coronet_ST"    "$DIR/redis_echo_ST"      $((PORT_BASE))
    start_one "coronet_chain" "$DIR/redis_echo_chain"   $((PORT_BASE + 1))
    if [ -x "$DIR/redis_echo_asio_ST" ]; then
        start_one "asio_ST"       "$DIR/redis_echo_asio_ST" $((PORT_BASE + 2))
    fi
fi

if [ "$MODE" = "mt" ] || [ "$MODE" = "all" ]; then
    start_one "coronet_MT"    "$DIR/redis_echo_MT"      $((PORT_BASE + 10))
    if [ -x "$DIR/redis_echo_asio_MT" ]; then
        start_one "asio_MT"       "$DIR/redis_echo_asio_MT" $((PORT_BASE + 11))
    fi
fi

echo "=== All servers ready ==="
