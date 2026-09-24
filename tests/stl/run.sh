#!/usr/bin/env bash
# tests/stl/run.sh —— **STL 库的常设验收**（一个库装所有动态容器；容器都建在池上）
#
# 判据：
#   ① 正例：每个容器一条「整行期望输出」（自带数字自证）
#   ② ASan 干净：容器路径不许有内存问题
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

run_case() {   # run_case <名字> <文件> <期望的一整行>
    local name=$1 f=$2 want=$3 out
    if ! out=$("$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if [ "$out" = "$want" ]; then echo "  ok   $name  ->  $out"
    else echo "  FAIL $name  ->  期望「$want」，得到「$out」"; fail=1; fi
}

echo "== vector<T>：翻倍扩容 / dense 连续 / shrink 降水位 / clear 留容量 =="
run_case vector  tests/stl/vector.extc  "cap0=4 n=9 cap=16 sum=36 shrink=9 pop=8 n=8 clear=0/9 tail=-9"

echo "== vector<T>：grow 摊还（1000 次 push 只搬 9 次）=="
run_case grow    tests/stl/vector_grow.extc    "caps=4,8,16,32,64,128,256,512,1024 n=1000"

echo "== vector<T>：churn 内存平（1e5 与 1e6 两轮，容量策略相同 ⇒ 峰值 RSS 相当）=="
TMPC=$(mktemp -d)
if "$EXTC" tests/stl/vector_churn_a.extc -o "$TMPC/a.c" >/dev/null 2>&1 \
   && "$EXTC" tests/stl/vector_churn_b.extc -o "$TMPC/b.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMPC/a" "$TMPC/a.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMPC/b" "$TMPC/b.c" >/dev/null 2>&1; then
    ka=$(/usr/bin/time -f %M "$TMPC/a" 2>&1 >/dev/null | tail -1)
    kb=$(/usr/bin/time -f %M "$TMPC/b" 2>&1 >/dev/null | tail -1)
    if [ "$kb" -le $(( ka * 2 )) ]; then
        echo "  ok   churn  ->  1e5: ${ka} KB · 1e6: ${kb} KB ⇒ 平"
    else
        echo "  FAIL churn  ->  1e5: ${ka} KB · 1e6: ${kb} KB ⇒ 涨了"; fail=1
    fi
else
    echo "  FAIL churn  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMPC"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
if "$EXTC" tests/stl/vector.extc -o "$TMP/v.c" >/dev/null 2>&1 \
   && gcc -std=c11 -g -fsanitize=address -o "$TMP/v" "$TMP/v.c" >/dev/null 2>&1 \
   && ! "$TMP/v" 2>&1 | grep -q Sanitizer; then
    echo "  ok   ASan  ->  干净"
else
    echo "  FAIL ASan  ->  报了内存问题"; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
