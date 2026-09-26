#!/usr/bin/env bash
# 每任务内存与 N 个活任务：extC 与 Go 1.26 同形对照（冷/热两批）。
#
#   bench/coro/run_liveN.sh [N ...]        默认 1000 10000
#
# extC 侧：extc 生成 liveN_gen.c，与 liveN_clock.c（时钟 + /proc/self/statm）一起编译。
# Go 侧：liveN.go，同样两批：spawn 之后等它们挂到无缓冲 channel 上再量内存。
set -u
cd "$(dirname "$0")/../.."
NS=${*:-"1000 10000"}
EXTC=${EXTC:-build/extc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

"$EXTC" -w --no-line-map -o "$TMP/liveN_gen.c" bench/coro/liveN.extc || exit 1
gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$TMP/liveN_c" "$TMP/liveN_gen.c" bench/coro/liveN_clock.c || exit 1
( cd bench/coro && go build -o "$TMP/liveN_go" liveN.go ) || exit 1

echo "每任务内存与 N 个活任务（$(nproc) 核 · $(gcc -dumpmachine) · $(go version | awk '{print $3}')）"
for n in $NS; do
    "$TMP/liveN_c" "$n"
    "$TMP/liveN_go" "$n"
done
