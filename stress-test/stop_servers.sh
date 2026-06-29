#!/bin/bash
# stop_servers.sh — 停止所有由 start_servers.sh 启动的服务端
echo "=== Stopping stress servers ==="
for f in /tmp/stress_*.pid; do
    [ -f "$f" ] || continue
    pid=$(cat "$f" 2>/dev/null)
    name=$(basename "$f" .pid)
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null && echo "  stopped $name (pid=$pid)" || echo "  $name already gone"
    fi
    rm -f "$f"
done
echo "=== All servers stopped ==="
