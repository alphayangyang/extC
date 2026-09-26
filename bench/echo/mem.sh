#!/usr/bin/env bash
# bench/echo/mem.sh —— 每连接内存：空转 vs 512 条活跃连接时两个 server 的常驻。
#
# 做法：起 server → 读一次 VmRSS（空转）→ 用 python 开 512 条连接**按住不放** → 再读一次 VmRSS。
# 差值除以连接数就是每连接成本的一个上界（含内核缓冲，两个 server 一样对待）。
#
# 输出（给 mk_results.py 消费）：idle / n512 / go_idle / go_n512 + RSS_KB
set -u
cd "$(dirname "$0")/../.." || exit 1
N=${N:-512}
PORT_EXTC=${PORT_EXTC:-7654}
PORT_GO=${PORT_GO:-7656}
work=$(mktemp -d /tmp/bench_mem.XXXXXX) || exit 1
pids=""
cleanup() { for p in $pids; do kill -9 "$p" 2>/dev/null; done; rm -rf "$work"; }
trap cleanup EXIT INT TERM

rss() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }

hold() { # $1=port  $2=seconds
    python3 - "$1" "$2" "$N" <<'PY' &
import socket, sys, time
port, secs, n = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
cs = []
for _ in range(n):
    try:
        cs.append(socket.create_connection(('127.0.0.1', port), timeout=3))
    except Exception:
        break
time.sleep(secs)
for c in cs:
    c.close()
PY
}

export EXTC=${EXTC:-./build/extc}
$EXTC -w --no-line-map -o "$work/es.c" tests/coro/echo_server.extc >/dev/null 2>&1 || { echo "extC server 编译失败"; exit 1; }
gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$work/es" "$work/es.c" 2>/dev/null || { echo "gcc 失败"; exit 1; }
( cd bench/echo && go build -o "$work/server" server.go ) || { echo "go build 失败"; exit 1; }

for who in extC go; do
    if [ "$who" = extC ]; then "$work/es" >/dev/null 2>&1 & port=$PORT_EXTC; else "$work/server" -port "$PORT_GO" >/dev/null 2>&1 & port=$PORT_GO; fi
    pid=$!
    pids="$pids $pid"
    sleep 1
    i=$(rss "$pid")
    hold "$port" 5
    h=$!
    sleep 2.5                     # 让连接全部建好、任务/协程都起来
    a=$(rss "$pid")
    wait "$h" 2>/dev/null
    key=$([ "$who" = extC ] && echo "" || echo "go_")
    echo "${key}idle $i"
    echo "${key}n512 $a"
    kill -9 "$pid" 2>/dev/null
done
