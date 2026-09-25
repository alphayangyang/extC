#!/usr/bin/env bash
# bench/zero/prof.sh —— **为什么 extC 比 C++ 慢**：callgrind 剖析（POOLS.md §7.4.3）
#
# 形状与主表一致（循环体 = 一个地方，每轮塞 per 条、然后清掉），两边都真的在做事，
# 输入相同 ⇒ 可以直接比"每轮多少条指令"。用 valgrind（本机没有 perf）。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
N=${N:-2000000}
PER=${PER:-8}
MODE=${MODE:-B}

./build/extc bench/zero/probe_epoch_vs_clear.extc -o build/prof-extc.c >/dev/null 2>&1 \
  && cc -O1 -g -std=c11 build/prof-extc.c -o build/prof-extc || { echo "extC 探针编不过"; exit 1; }
if command -v g++ >/dev/null 2>&1; then
    g++ -O1 -g -std=c++17 -o build/prof-cpp bench/zero/cpp/epoch_vs_clear.cpp || { echo "C++ 基线编不过"; exit 1; }
else
    echo "没有 g++（这一节跳过）"; exit 0
fi

echo "== callgrind：同一形状、同一输入，比每轮指令数 =="
for t in extc cpp; do
    bin=build/prof-$t
    out=$(valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.$t "$bin" "$N" "$PER" "$MODE" 2>&1 | sed -n 's/.*I   refs: *//p')
    ir=${out//,/}
    per=$(awk -v a="$ir" -v n="$N" 'BEGIN{printf "%.1f", a/n}')
    printf '  %-5s  %12s 条指令   %6s 条/轮\n' "$t" "$out" "$per"
done

echo
echo "== cachegrind：分支与预测失败 =="
for t in extc cpp; do
    bin=build/prof-$t
    valgrind --tool=cachegrind --branch-sim=yes --cachegrind-out-file=/dev/null "$bin" "$N" "$PER" "$MODE" 2>&1 \
      | sed -n 's/.*\(I refs:\|Branches:\|Mispredicts:\|Mispred rate:\).*/  '"$t"'  &/p'
done

echo
echo "== 多出来的指令花在哪类检查上（生成物里的静态计数）=="
for pat in extc_arena_release extc_checkedIndex extc_narrowI extc_pool_reset; do
    printf '  %-22s %s 处\n' "$pat" "$(grep -c "$pat" build/prof-extc.c)"
done
echo
echo "参考（2026-09-26 第十段实测，N=200 万）："
cat <<'TXT'
    extC B  261.6 条指令/轮 · 59.2 分支/轮 · 3.4% 预测失败
    C++  B  174.2 条指令/轮 · 38.0 分支/轮 · 5.4% 预测失败
    ⇒ extC 多 55% 指令，而墙钟 10.45 对 3.70 ns（2.8 倍）
    ⇒ 差在"指令条数 × 每条效率"：指令只多 1.55 倍，所以剩下那部分是每条指令更贵
      （同一份数据布局、同一访问模式 ⇒ 不是 cache/分支的锅，实测分支预测还更好）
    extc_pool_reset 单独占 8.04%（42M/522M 条，21 条/轮）
TXT
