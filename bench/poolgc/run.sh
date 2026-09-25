#!/usr/bin/env bash
# bench/poolgc —— **池容器的回收策略：shrink 到底有没有用**（作者 2026-09-26 问）
#
# 两组形状：
#   A 尖峰后闲置（涨到 1e6 然后清空/留 10 个）→ 看 shrink 能不能把**当前 RSS** 降下来
#   B 振荡（20 轮 × 插 100k、删 99k）→ 看"每轮都缩"的时间与内存代价
#
# ⚠️ 关键点：**峰值 RSS 看不出这件事**（缩之前就冲上去了），所以这里用 `sample.py`
# 从外面采样 `/proc/<pid>/statm`（每 20 ms 一次），报"峰值"和"结束前"两个数。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc; CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for t in c_clear_noshrink d_clear_shrink e_pop10_shrink f_osc_noshrink g_osc_shrink; do
    if ! "$EXTC" "bench/poolgc/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
       || ! "$CC" -O2 -std=c11 -o "bench/poolgc/$t.bin" "$TMP/$t.c" >/dev/null 2>&1; then
        echo "FAIL $t 编不过"; exit 1
    fi
done
echo "== A 尖峰后闲置：shrink 还 RSS 吗 =="
python3 bench/poolgc/sample.py c_clear_noshrink d_clear_shrink e_pop10_shrink
echo
echo "== B 振荡：每轮都缩的代价（时间 best of 2 + RSS）=="
for t in f_osc_noshrink g_osc_shrink; do
    printf '%-18s ' "$t"
    for r in 1 2; do printf '%s ' "$({ time "bench/poolgc/$t.bin" >/dev/null; } 2>&1 | grep real | sed 's/real//')"; done
    echo
done
python3 bench/poolgc/sample.py f_osc_noshrink g_osc_shrink
rm -f bench/poolgc/*.bin
