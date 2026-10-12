#!/usr/bin/env bash
# run.sh —— 编译并跑三个探针，只打印数字。
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
CC="${CC:-cc}"
FLAGS="-O2 -std=c11 -Wall -Wextra"
fail=0
for f in probe_sweep probe_coldhot probe_real sim; do
  if ! $CC $FLAGS -o "$f" "$f.c"; then echo "编译失败：$f.c"; fail=1; fi
done
[ "$fail" = 0 ] || exit 1

echo "==================== 探针 1：触发策略（垃圾量 vs 存活量）===================="
./probe_sweep

echo
echo "==================== 探针 2：停顿能不能切分（冷结构 + 小热区）===================="
echo "--- 只扫热区（冷结构各自成区、永不清扫）---"
for k in 1 4 16 64; do ./probe_coldhot region "$k" 20000 400000; done
echo "--- 单堆全局回收（每次都要追全部冷结构）---"
for k in 1 4 16 64; do ./probe_coldhot global "$k" 20000 400000; done

echo
echo "==================== 探针 3：真实负载（有界符号表：插/查/手术/换引用/改值）===="
for m in nocoll region region+ev global; do ./probe_real "$m" 256 8 400000 4096; done
echo "--- 放大到 200 万轮 ---"
for m in region+ev global; do ./probe_real "$m" 256 8 2000000 4096; done
echo
echo "==================== 探针 4：手写的「生成物」（真实运行时）===================="
for m in base solo sweep ev; do ./sim "$m" 400000; done
echo "--- 放大到 400 万轮 ---"
for m in base sweep ev; do ./sim "$m" 4000000; done
exit 0
