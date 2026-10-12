#!/usr/bin/env bash
# repro.sh —— 一次复跑 RECON.md 里的全部实测（只读仓库，写 /tmp 与 out/）。
#
# 前置：仓库根目录有 `build/extc`（修后）。修前的编译器由 `git archive 9305b17`
# 解到 /tmp 再 `make`（**不动工作树**，不改 src/）。
#
#   tests/reclaim-rootstack/repro.sh            # 全部
#   tests/reclaim-rootstack/repro.sh scan      # 只跑某一节（scan|cost|peak|target|gates）
#
# 产物落在 tests/reclaim-rootstack/out/。跑完 /tmp 的中间产物可留可清（见最后一行提示）。
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
OUT=$DIR/out
PRE_SRC=${PRE_SRC:-9305b17}          # 19fd567 的父提交 = 修前
PREFIX=${PREFIX:-/tmp/rs-prefix}
WORK=${WORK:-/tmp/rs-gen}
mkdir -p "$OUT"

build_pre() {
    [ -x "$PREFIX/build/extc" ] && return 0
    echo "== 取修前源码（git archive $PRE_SRC）并构建 =="
    mkdir -p "$PREFIX"
    (cd "$ROOT" && git archive "$PRE_SRC") | tar -x -C "$PREFIX"
    (cd "$PREFIX" && make -j8) >/dev/null 2>&1 || { echo "修前构建失败"; exit 1; }
}

gen_corpus() {
    [ -d "$WORK" ] && [ -n "$(ls -A "$WORK" 2>/dev/null)" ] && return 0
    "$DIR/gen-corpus.sh" "$PREFIX/build/extc" "$ROOT" "$WORK" tests bench examples stdlib
}

do_scan() {
    build_pre
    echo "== 问题 3：全量语料触发点普查（修前 vs 修后）=="
    python3 "$DIR/scan-triggers.py" --pre "$PREFIX/build/extc" --post "$ROOT/build/extc" \
        --root "$ROOT" --out "$OUT/triggers-per-program.csv" \
        --sites "$OUT/trigger-sites.csv" 2>/dev/null | tee "$OUT/scan-triggers.txt"
}

do_cost() {
    build_pre; gen_corpus
    echo "== 问题 2 / 5：生成物静态清点 =="
    python3 "$DIR/cost-model.py" --quiet "$WORK" 2>/dev/null | tee "$OUT/cost-model-all.txt"
    python3 "$DIR/cost-model.py" --quiet "$WORK"/tests__reclaim-shapes__*.c 2>/dev/null \
        | tee "$OUT/cost-model-shapes.txt"
    python3 "$DIR/cost-model.py" --quiet "$WORK"/bench__*.c 2>/dev/null | tee "$OUT/cost-model-bench.txt"
}

do_peak() {
    build_pre
    echo "== 问题 3：110 个程序的峰值回归 =="
    python3 "$DIR/peak-regression.py" --pre "$PREFIX/build/extc" --post "$ROOT/build/extc" \
        --root "$ROOT" --list "$OUT/triggers-per-program.csv" \
        --out "$OUT/peak-regression.csv" | tee "$OUT/peak-regression.txt"
}

do_target() {
    build_pre
    echo "== 问题 3：目标数字（stress.extc 的峰値）=="
    {
        echo '$ EXTC_RECLAIM=1 '"$PREFIX"'/build/extc -I stdlib bench/stress/stress.extc -o /tmp/stress-pre.c'
        echo '$ EXTC_RECLAIM=1 ./build/extc -I stdlib bench/stress/stress.extc -o /tmp/stress-post.c'
        echo '$ gcc -std=c11 -fwrapv -O1 ... -o /tmp/stress-{pre,post}'
        echo '$ /usr/bin/time -v /tmp/stress-{pre,post}  # Maximum resident set size'
        EXTC_RECLAIM=1 "$PREFIX/build/extc" -I "$ROOT/stdlib" "$ROOT/bench/stress/stress.extc" \
            -o /tmp/stress-pre.c 2>/dev/null
        EXTC_RECLAIM=1 "$ROOT/build/extc" -I "$ROOT/stdlib" "$ROOT/bench/stress/stress.extc" \
            -o /tmp/stress-post.c 2>/dev/null
        cc -std=c11 -fwrapv -O1 /tmp/stress-pre.c -o /tmp/stress-pre
        cc -std=c11 -fwrapv -O1 /tmp/stress-post.c -o /tmp/stress-post
        for b in pre post; do
            for i in 1 2 3; do
                printf '%s: ' "$b"
                /usr/bin/time -v /tmp/stress-$b 2>&1 >/dev/null | grep 'Maximum resident set size'
            done
        done
    } | tee "$OUT/target-stress.txt"
}

do_gates() {
    echo "== 问题 6：现有关卡的基线（修后）=="
    "$ROOT/tests/reclaim-shapes/run-switch.sh" >"$OUT/run-switch-baseline.txt" 2>&1
    echo "run-switch.sh exit=$? （$(tail -1 "$OUT/run-switch-baseline.txt")）"
    "$ROOT/tests/reclaim-perf/gate.sh" >"$OUT/reclaim-perf-gate-baseline.txt" 2>&1
    echo "reclaim-perf/gate.sh exit=$? （$(grep -m1 '实测' "$OUT/reclaim-perf-gate-baseline.txt")）"
}

case "${1:-all}" in
    scan)   do_scan ;;
    cost)   do_cost ;;
    peak)   do_peak ;;
    target) do_target ;;
    gates)  do_gates ;;
    all)    do_scan; do_cost; do_peak; do_target; do_gates ;;
    *) echo "unknown section: $1"; exit 2 ;;
esac
echo "（中间产物：$PREFIX 修前构建、$WORK 生成物；可 rm -rf 清掉）"
