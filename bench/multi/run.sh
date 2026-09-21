#!/usr/bin/env bash
# bench/multi —— **多语言横向对比**（mod / sieve / matmul × C / extC / JS / Python）
#
# 为什么有这份：bench/run.sh 只比 extC vs C；这份把 JS / Python 也拉进来，
# 用来回答"extC 到底站在哪一档"✓
#
# ⚠️ 两条诚实声明（不然表会被误读）：
#   · **Python 那三格都是不同规模/实现**（mod 用 3e6、sieve 用 1e6、matmul 走 numpy）——
#     原作者的取舍 ⇒ **不能跟别人直接比**，只看量级；表里会用 ✗Py 标出来 ✓
#   · 每个程序自己打印结果（求和/计数），跨语言必须**数字一致**才算对拍成功 ✓
set -u
cd "$(dirname "$0")/../.."        # 回到仓库根

EXTC=./build/extc
CC=${CC:-cc}
RUNS=${RUNS:-3}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

best_ms() {                       # $@ = 命令；跑 RUNS 次取最快，毫秒
    local best=999999999 t0 t1 dt i
    for ((i = 0; i < RUNS; i++)); do
        t0=$(date +%s%N)
        "$@" > /dev/null 2>&1 || { echo "ERR"; return; }
        t1=$(date +%s%N); dt=$(( (t1 - t0) / 1000000 ))
        (( dt < best )) && best=$dt
    done
    echo "$best"
}

out_of() {                        # 第一次运行的输出（对拍用）
    "$@" 2>&1 | tr '\n' ' ' | cut -c1-46
}

printf '%-10s %10s %10s %10s %10s   %s\n' 负载 extC C JS Python '对拍（第一次运行的输出）'
printf '%s\n' "--------------------------------------------------------------------------------"

for w in mod sieve matmul; do
    # ---- 各自准备一次可执行/入口 ----
    $EXTC  "bench/multi/$w.extc" -o "$TMP/$w.extc.c" >/dev/null 2>&1 \
        && $CC -O2 -w -o "$TMP/${w}_extc" "$TMP/$w.extc.c" 2>/dev/null
    $CC -O2 -w -o "$TMP/${w}_c" "bench/multi/$w.c" 2>/dev/null

    a=$(best_ms "$TMP/${w}_extc"); c=$(best_ms "$TMP/${w}_c")
    j=$(best_ms node "bench/multi/$w.js"); p=$(best_ms python3 "bench/multi/$w.py")

    ref=$(out_of "$TMP/${w}_extc")
    agree="extC=$ref"
    for pair in "C:$TMP/${w}_c" "JS:node bench/multi/$w.js" "Py:python3 bench/multi/$w.py"; do
        name=${pair%%:*}; cmd=${pair#*:}
        got=$(out_of $cmd)
        [ "$got" = "$ref" ] || agree="$agree  ✗$name=$got"
    done

    printf '%-10s %8s ms %8s ms %8s ms %8s ms   %s\n' "$w" "$a" "$c" "$j" "$p" "$agree"
done

echo
echo "（RUNS=$RUNS 次取最快；单位 ms；Python 的 mod 用的是 3e6，其余 3e7 ✓）"
