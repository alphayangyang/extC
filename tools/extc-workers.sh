#!/bin/sh
# extc-workers.sh —— 一条命令起 N 个单线程 worker 进程（多进程形态的启动器）
#
#   用法: tools/extc-workers.sh N [起始核] -- 程序 [参数...]
#   例子: tools/extc-workers.sh 8 -- ./echo_server
#         tools/extc-workers.sh 4 4 -- ./http_server      # 用核 4..7
#
# 为什么要它：extC 的并行第一形态是**多进程**，配套的只有运行期的
# `extc_tcp_listen_shared`（= `extc_tcp_listen` + bind 之前设 SO_REUSEPORT）。内核按流哈希把新连接
# 分给这些进程，于是：不共享内存、不加锁、不改语言，`extc_pool_*` / `extc_task_*` 天然各进程一份，
# 一个 worker 崩了只带走它自己的连接。这与 nginx 的 master/worker 是同一个形状。
#
# 实测（2026-09-28 · 8 个 P 核 · loopback · 256 连接 · ping-pong）：
#   8 个 extC 进程 1,343,561 req/s（4.10x，8 核）/ 流水线 5,002 MB/s（4.45x）
#   Go 1.26 八个 goroutine 8 线程 304,764 req/s（0.93x，完全不动）
#   nginx 1.28.3 八个 worker（调优到牙齿）1,064,775 req/s / 249 MB RSS（HTTP 口径）
#
# 策略：fail-fast（任何一个 worker 退出 ⇒ 收掉全部并返回它的退出码）。要"崩了自动重启"，外面套一层
# `while :; do tools/extc-workers.sh 8 -- ./server; done` 即可 —— 启动器保持简单，重启策略留给调用者。
set -u

usage() {
    echo "用法: extc-workers.sh N [起始核] -- 程序 [参数...]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
N=$1
shift
case "$N" in ''|*[!0-9]*) usage;; esac
[ "$N" -ge 1 ] || usage

BASE=0
case "${1:-}" in
    ''|--) ;;
    *[!0-9]*) ;;
    *) BASE=$1; shift;;
esac
[ "${1:-}" = "--" ] && shift
[ $# -ge 1 ] || usage

PIDS=""
cleanup() {
    trap - INT TERM
    [ -n "$PIDS" ] && kill $PIDS 2>/dev/null
    wait 2>/dev/null
    echo "[workers] 已收工" >&2
    exit 0
}
trap cleanup INT TERM

i=0
while [ "$i" -lt "$N" ]; do
    cpu=$((BASE + i))
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "$cpu" "$@" &
    else
        "$@" &
    fi
    PIDS="$PIDS $!"
    i=$((i + 1))
done
echo "[workers] 起了 $N 个进程（核 $BASE..$((BASE + N - 1))）⇒ $*" >&2

# fail-fast：任何一个 worker 退出就把其余的收掉，并把它自己的退出码返回给调用者。
while :; do
    sleep 1
    for p in $PIDS; do
        if ! kill -0 "$p" 2>/dev/null; then
            wait "$p" 2>/dev/null
            rc=$?
            echo "[workers] 进程 $p 退出（rc=$rc）⇒ 全部收工" >&2
            kill $PIDS 2>/dev/null
            wait 2>/dev/null
            exit "${rc:-1}"
        fi
    done
done
