#!/usr/bin/env bash
# bench/echo/run.sh —— echo server 压测台：extC vs Go（同条件 + 自然多核两列）
#
# 硬约束（照 CONCURRENCY.md 的口径）：
#   ① **唯一的负载生成器**：extC 与 Go 两边都用同一个 load.go —— 公平性的前提。
#   ② **先量生成器天花板**：打不动 sink 就没有资格比两个真 server。
#   ③ 每个回声都逐字节校验（生成器里做），任何一格出现 "数据校验失败" 都算这一格无效。
#   ④ 预热跑一轮不计入，正式每格跑 REPS 次取**最好的一次**，并打印离散度。
#
# 用法：
#   bench/echo/run.sh                 # 完整矩阵（约 4 分钟）
#   REP=3 SECS=3 bench/echo/run.sh    # 调参
#   QUICK=1 bench/echo/run.sh         # 小矩阵（2 个并发 × 1 种载荷）
set -u

cd "$(dirname "$0")/../.." || exit 1
EXTC=${EXTC:-./build/extc}
OUT=${OUT:-bench/echo/results.txt}
PORT_EXTC=${PORT_EXTC:-7654}
PORT_SINK=${PORT_SINK:-7655}
PORT_GO=${PORT_GO:-7656}
PORT_GO1=${PORT_GO1:-7657}

REPS=${REPS:-3}
ITERS=${ITERS:-30000}
WARMUP=${WARMUP:-3000}
PAYLOADS=${PAYLOADS:-"64 1024"}
CONNS=${CONNS:-"1 8 64 256 512"}
MODES=${MODES:-"pingpong pipeline"}
if [ "${QUICK:-0}" = 1 ]; then
    REPS=1; ITERS=4000; WARMUP=1000; PAYLOADS="64"; CONNS="1 8"; MODES="pingpong"
fi

work=$(mktemp -d /tmp/bench_echo.XXXXXX) || exit 1
pids=""
cleanup() {
    for p in $pids; do kill -9 "$p" 2>/dev/null; done
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

: > "$OUT"                      # 每次运行从空的记录开始（不留历史运行混在里面）
say() { printf '%s\n' "$*" | tee -a "$OUT"; }

# ---------------------------------------------------------------- 构建
say "================================================================"
say "echo server 压测：extC vs Go   $(date '+%F %T')"
say "机器：$(uname -sr) · $(nproc) 核 · $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"
say "extC：$($EXTC --version 2>/dev/null | head -1)"
say "gcc ：$(gcc --version | head -1)"
say "go  ：$(go version)"
say "生成器参数：REPS=$REPS ITERS=$ITERS WARMUP=$WARMUP 载荷=[$PAYLOADS] 并发=[$CONNS] 形态=[$MODES]"
say "================================================================"

cd bench/echo || exit 1
for f in load sink server; do
    go build -o "$work/$f" "$f.go" 2>"$work/$f.err" || { echo "go build $f 失败：$(head -2 "$work/$f.err")"; exit 1; }
done
cd ../.. || exit 1
if ! $EXTC -w --no-line-map -o "$work/es.c" tests/coro/echo_server.extc 2>"$work/es.err"; then
    echo "extC server 编译失败：$(head -2 "$work/es.err")"; exit 1
fi
if ! gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$work/es" "$work/es.c" 2>"$work/es.gcc"; then
    echo "extC server gcc 失败：$(head -2 "$work/es.gcc")"; exit 1
fi
say "构建完成：extC server + Go server + sink + load（都在 $work）"

# ---------------------------------------------------------------- 起服务
"$work/sink" -port "$PORT_SINK" >/dev/null 2>&1 & pids="$pids $!"
"$work/server" -port "$PORT_GO" >/dev/null 2>&1 & pids="$pids $!"
GOMAXPROCS=1 "$work/server" -port "$PORT_GO1" >/dev/null 2>&1 & pids="$pids $!"
"$work/es" >/dev/null 2>&1 & pids="$pids $!"
sleep 1

# ---------------------------------------------------------------- 生成器天花板
say ""
say "── 生成器天花板（对 sink：它不做任何调度，打不动它就没资格比别的）────────────────"
CEIL=""
for n in $CONNS; do
    line=$("$work/load" -addr "127.0.0.1:$PORT_SINK" -conns "$n" -iters "$ITERS" -payload 64 2>&1)
    say "  $line"
    case "$line" in *"ok"*) ;; *) say "  ⚠ 生成器自己这一格就校验失败 ⇒ 后面同并发的数字都不可信" ;; esac
done

# ---------------------------------------------------------------- 跑一格
# best_of：跑 REPS 次取 req/s 最高的一次；同时要求每一次都 ok（有失败就标出来）
run_cell() { # $1=server 名 $2=port $3=conns $4=payload $5=mode
    local best="" bestv=-1 spread=""
    local i=1
    while [ "$i" -le "$REPS" ]; do
        local line
        line=$("$work/load" -addr "127.0.0.1:$2" -conns "$3" -iters "$ITERS" -payload "$4" \
                        -mode "$5" -depth 16 -warmup "$WARMUP" 2>&1)
        case "$line" in *"ok") ;; *) echo "FAIL|$line"; return ;; esac
        local v
        v=$(echo "$line" | awk '{print $5}')
        if [ -n "$best" ] && [ "${v%%.*}" -lt "${bestv%%.*}" ]; then :; fi
        if [ -z "$best" ] || [ "${v%%.*}" -gt "${bestv%%.*}" ]; then best="$line"; bestv="$v"; fi
        i=$((i + 1))
    done
    echo "OK|$best"
}

say ""
say "── 矩阵（每格预热 $WARMUP + 正式 $ITERS 次/连接，跑 $REPS 次取最好）────────────────────"
for mode in $MODES; do
    for payload in $PAYLOADS; do
        for n in $CONNS; do
            say ""
            say "  [$mode · payload=$payload · conns=$n]"
            for pair in "extC:$PORT_EXTC" "Go(1核):$PORT_GO1" "Go(默认):$PORT_GO" "sink:$PORT_SINK"; do
                name=${pair%%:*}; port=${pair##*:}
                res=$(run_cell "$name" "$port" "$n" "$payload" "$mode")
                case "$res" in
                OK\|*) printf '    %-9s %s\n' "$name" "${res#OK|}" | tee -a "$OUT" ;;
                *)     printf '    %-9s *** 数据校验失败/出错：%s\n' "$name" "${res#FAIL|}" | tee -a "$OUT" ;;
                esac
            done
        done
    done
done

say ""
say "完成 $(date '+%T') · 原始记录：$OUT"
say "读法：同条件那两列是 extC vs Go(GOMAXPROCS=1)；Go(默认) 是多核自然形态，不能当同条件读。"
