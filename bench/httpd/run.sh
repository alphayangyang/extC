#!/usr/bin/env bash
# bench/httpd/run.sh —— **最小 HTTP/1.1 静态服务器**的功能判据与数字。
#
# 三根柱子共用同一个判据客户端（纯 socket，不用 curl/ab）：
#   extc-httpd   bench/httpd/httpd.extc（epoll + 协程 + sendfile）
#   py-httpd     python3 -m http.server（最省事的对照；它是单线程 ThreadingHTTPServer）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
ROOT=bench/httpd/www
PORT=${PORT:-18080}
PY_PORT=$((PORT + 1))
fail=0

"$EXTC" -w --no-line-map -o build/httpd.c bench/httpd/httpd.extc || exit 1
gcc -O2 -std=c11 -fwrapv -o build/httpd build/httpd.c || exit 1

wait_port() {
    python3 - "$1" <<'PY'
import socket, sys, time
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

echo "== extc-httpd（epoll + 协程 + sendfile）=="
./build/httpd "$PORT" "$ROOT" & SRV=$!
if wait_port "$PORT"; then
    if python3 bench/httpd/client.py "$PORT" "$SRV" --strict; then :; else fail=1; fi
else
    echo "  FAIL 服务器没起来"; fail=1
fi
kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null

echo "== py-httpd（python3 -m http.server，同一份判据）=="
# `--protocol HTTP/1.1`：默认是 1.0 ⇒ 每条响应后关连接，keep-alive 判据没法比 ✓
( cd "$ROOT" && exec python3 -m http.server "$PY_PORT" --bind 127.0.0.1 --protocol HTTP/1.1 ) >/dev/null 2>&1 & PS=$!
if wait_port "$PY_PORT"; then
    python3 bench/httpd/client.py "$PY_PORT" "$PS" | sed 's/^/  /' || true
else
    echo "  （跳过：python3 -m http.server 没起来）"
fi
kill "$PS" 2>/dev/null; wait "$PS" 2>/dev/null
exit "$fail"
