#!/usr/bin/env bash
# bench/zoneexit —— **一个地方建 N 个池再退出**的代价（染色第二步的前后对比）
#
# 形状：循环体 = 一个地方；每轮建 N 个池（各拿一小块板块）后出块 ⇒ zoneLeaveTo 回收。
# 判据：时间随 N 的走向 + 峰值 RSS 必须平（不漏）。染色第二步的目标是把"记录"那一段
# 从"逐池走链 drop"变成"翻一位 + 游标归零"。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc; CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for n in 1000 10000; do
    if ! "$EXTC" "bench/zoneexit/n$n.extc" -o "$TMP/n$n.c" >/dev/null 2>&1 \
       || ! "$CC" -O2 -std=c11 -o "bench/zoneexit/n$n.bin" "$TMP/n$n.c" >/dev/null 2>&1; then
        echo "FAIL n$n 编不过"; exit 1
    fi
    printf 'N=%-6s ' "$n"
    for k in 1 2 3; do printf '%s ' "$({ time "bench/zoneexit/n$n.bin" >/dev/null; } 2>&1 | grep real | sed 's/real//')"; done
    printf ' · 峰值 '
    /usr/bin/time -f '%M KB' "bench/zoneexit/n$n.bin" 2>&1 >/dev/null | tail -1
done
rm -f bench/zoneexit/*.bin
