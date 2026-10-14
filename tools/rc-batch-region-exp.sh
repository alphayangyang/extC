#!/usr/bin/env bash
# tools/rc-batch-region-exp.sh —— 跑手工合成实验（tools/rc-batch-region-exp.c）。
#
# 先把仓库自己的发射口导出的运行期文本放到临时目录，再编两个变体跑 taskset 绑核。
# **只写 /tmp**；不碰编译器、不碰发射器（产物逐字节不变由生成物 cmp 独立证明）。
#
# 用法：tools/rc-batch-region-exp.sh [rounds] [reps]
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
ROUNDS=${1:-50}
REPS=${2:-5}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

cc -std=c11 -O1 -g -I "$ROOT/src/base" -I "$ROOT/src/back" \
   "$ROOT/tests/reclaim/emit_main.c" "$ROOT/src/back/reclaim.c" "$ROOT/src/base/base.c" \
   -o "$TMP/emit" || { echo "发射器编译失败" >&2; exit 2; }
"$TMP/emit" > "$TMP/rt.inc" || { echo "发射失败" >&2; exit 2; }
cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O2 -I "$TMP" \
   "$ROOT/tools/rc-batch-region-exp.c" -o "$TMP/exp" || { echo "实验编译失败" >&2; exit 2; }

echo "== 合成实验（rounds=$ROUNDS，每批 ${NODE_PER_BATCH:-4874}×80B + ${EDGE_PER_BATCH:-10840}×32B，活集 1614）=="
for v in A B C D; do
    for r in $(seq 1 "$REPS"); do
        taskset -c 6 "$TMP/exp" "$ROUNDS" "$v"
    done
done
