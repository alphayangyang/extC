#!/usr/bin/env bash
# bench/httpd/vs-nginx.sh —— 用 **wrk**（多线程 C 客户端）对 nginx 量 extC 的 HTTP 服务器。
#
#   WRK=/path/to/wrk bash bench/httpd/vs-nginx.sh
#
# wrk 本机没有时可以不要 root 地取：apt-get download wrk libluajit-5.1-2 && dpkg-deb -x …
# 然后 WRK=<解包>/usr/bin/wrk LD_LIBRARY_PATH=<解包>/usr/lib/x86_64-linux-gnu
# nginx 不在就跳过那一档（`NGINX=/usr/sbin/nginx`）。
#
# 为什么要它：`client.py`/`bench.py` 是**判据**，不是量具 —— Python 客户端自己就是瓶颈
# （实测：同一个服务器，Python 客户端 39k req/s，wrk 183k req/s ✗⇒✓）。
set -u
cd "$(dirname "$0")/../.."
ROOT="$PWD/bench/httpd/www"
EXTC_PORT=${EXTC_PORT:-18088}
NGX_PORT=${NGX_PORT:-18089}
TINY=${TINY:-/small.txt}
BIG=${BIG:-/big.bin}
WRK=${WRK:-wrk}
NGINX=${NGINX:-/usr/sbin/nginx}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}

command -v "$WRK" >/dev/null || { echo "需要 wrk（见文件头取法）"; exit 0; }
run() { "$WRK" -t8 -c"$1" -d5s --latency "http://127.0.0.1:$2$3" 2>/dev/null; }
show() { grep -E "Requests/sec|Transfer/sec|Socket errors|^\s+50%|^\s+99%" | tr '\n' ' ' | sed 's/  */ /g'; echo; }

"$EXTC" -w --no-line-map -o build/httpd.c bench/httpd/httpd.extc || exit 1
gcc -O2 -std=c11 -fwrapv -o build/httpd build/httpd.c || exit 1

for n in 1 8; do
    pids=""; for i in $(seq "$n"); do ./build/httpd "$EXTC_PORT" "$ROOT" >/dev/null 2>&1 & pids="$pids $!"; done
    sleep 1
    echo "== extC（$n 个进程，SO_REUSEPORT 共用端口）"
    echo -n "  tiny "; run 256 "$EXTC_PORT" "$TINY" | show
    echo -n "  big  "; run 128 "$EXTC_PORT" "$BIG"  | show
    kill $pids 2>/dev/null; wait 2>/dev/null
done

if [ -x "$NGINX" ]; then
    mkdir -p /tmp/ngx-bench/logs
    cat > /tmp/ngx-bench/nginx.conf <<CONF
worker_processes 8;
error_log /tmp/ngx-bench/logs/error.log warn;
pid /tmp/ngx-bench/logs/nginx.pid;
events { worker_connections 8192; }
http {
    access_log off; sendfile on; tcp_nopush on; keepalive_timeout 65;
    server { listen $NGX_PORT reuseport; root $ROOT; }
}
CONF
    "$NGINX" -c /tmp/ngx-bench/nginx.conf -p /tmp/ngx-bench 2>/dev/null
    sleep 0.5
    echo "== nginx（8 worker）"
    echo -n "  tiny "; run 256 "$NGX_PORT" "$TINY" | show
    echo -n "  big  "; run 128 "$NGX_PORT" "$BIG"  | show
    "$NGINX" -s stop -c /tmp/ngx-bench/nginx.conf -p /tmp/ngx-bench 2>/dev/null
fi
