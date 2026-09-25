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

echo "== vector<T>：1.5 倍扩容 / dense 连续 / shrink 降水位 / clear 留容量 =="
run_case vector  tests/stl/vector.extc  "cap0=4 n=9 cap=9 sum=36 shrink=9 pop=8 n=8 clear=0/9 tail=-9"

echo "== vector<T>：grow 摊还（1000 次 push 只搬 14 次 · 1.5 倍）=="
run_case grow    tests/stl/vector_grow.extc    "caps=4,6,9,13,19,28,42,63,94,141,211,316,474,711,1066 n=1000"

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

TMPC2=$(mktemp -d)
echo "== vector<T>：池底扩容的峰值 RSS（同一形状 vs arena 切片 —— 这是池底化的判据）=="
# 判据：1e6 个 i64（活跃 8 MB）不许把历代旧副本留在 arena 里。
# 两个用例**同一形状**，只有存储来源不同 ⇒ 自己跟自己比，不依赖机器的绝对数字。
if "$EXTC" tests/stl/vector_rss_plate.extc -o "$TMPC2/p.c" >/dev/null 2>&1 \
   && "$EXTC" tests/stl/vector_rss_arena.extc -o "$TMPC2/a.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMPC2/p" "$TMPC2/p.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMPC2/a" "$TMPC2/a.c" >/dev/null 2>&1; then
    kp=$(/usr/bin/time -f %M "$TMPC2/p" 2>&1 >/dev/null | tail -1)
    ka=$(/usr/bin/time -f %M "$TMPC2/a" 2>&1 >/dev/null | tail -1)
    if [ "$kp" -lt $(( ka * 3 / 4 )) ]; then
        echo "  ok   plate rss  ->  池底 ${kp} KB vs arena ${ka} KB（低 $(( (ka - kp) * 100 / ka ))%）"
    else
        echo "  FAIL plate rss  ->  池底 ${kp} KB 没有明显低于 arena ${ka} KB"; fail=1
    fi
else
    echo "  FAIL plate rss  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMPC2"

echo "== string：连续字节串（append 触发 1.5 倍扩容 · asSlice 连续可直印 · shrink 降水位）=="
run_case string  tests/stl/string.extc  "len=5 cap=16 len2=44 cap2=54 shrink=44 text=hello, world! and more bytes to force growth t=abc(3) clear=0/44"

echo "== hashSetI64：无值 map（哈希 / 探测 / 墓碑只有一份实现 —— 建在 hashMapI64<u8> 上）=="
run_case hashset tests/stl/hashSet.extc     "new=2 len=2 again=0 len=2 has2=1 rm=1 gone=0 len=1 hits=200000 cap=16 cleared=0"

echo "== set<T>：**有序**集合（建在 map<T, u8> 上，B+ 树）—— 按序遍历 · put 新元素为真 · lowerBound 排名 =="
run_case setOrdered tests/stl/setOrdered.extc "order=1,2,3,4,5 len=5 dup=0 has3=1 lb3=2 lb6=5 rm=1 after=1,2,4,5 lb4=2"
run_case setStructKey tests/stl/setStructKey.extc "order=1,2,3 len=3 has2=1 lb2=1 first=1 last=3"

echo "== string：拼接（+ / +=）· 比较 · 查找 · 视图 · 当 hashMap 键 =="
run_case stringOps  tests/stl/stringOps.extc  "c=hello world|a=hello world|eq=true|lt=false|find=6|miss=-1|sw=true|ew=true|at=101|sub=world|cb=hello world!!!|hn=2|hv=20"

echo "== string::find 的穷举对拍（{a,b} 上 1..4 的模式 × 0..10 的文本 vs 朴素查找）+ 大输入 =="
run_case stringFind tests/stl/stringFind.extc "checked=61410|bad=0"

echo "== string：churn 内存平（1e5 与 1e6 两轮）=="
TMP2=$(mktemp -d)
if "$EXTC" tests/stl/string_churn_a.extc -o "$TMP2/a.c" >/dev/null 2>&1 \
   && "$EXTC" tests/stl/string_churn_b.extc -o "$TMP2/b.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMP2/a" "$TMP2/a.c" >/dev/null 2>&1 \
   && gcc -std=c11 -O2 -o "$TMP2/b" "$TMP2/b.c" >/dev/null 2>&1; then
    ka=$(/usr/bin/time -f %M "$TMP2/a" 2>&1 >/dev/null | tail -1)
    kb=$(/usr/bin/time -f %M "$TMP2/b" 2>&1 >/dev/null | tail -1)
    if [ "$kb" -le $(( ka * 2 )) ]; then
        echo "  ok   churn  ->  1e5: ${ka} KB · 1e6: ${kb} KB ⇒ 平"
    else
        echo "  FAIL churn  ->  1e5: ${ka} KB · 1e6: ${kb} KB ⇒ 涨了"; fail=1
    fi
else
    echo "  FAIL churn  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMP2"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
asan_ok() {   # 文件 可执行名
    "$EXTC" "$1" -o "$TMP/$2.c" >/dev/null 2>&1 \
      && gcc -std=c11 -g -fsanitize=address -o "$TMP/$2" "$TMP/$2.c" >/dev/null 2>&1 \
      && ! "$TMP/$2" 2>&1 | grep -q Sanitizer
}
if asan_ok tests/stl/vector.extc v && asan_ok tests/stl/setOrdered.extc so && asan_ok tests/stl/setStructKey.extc ssk && asan_ok tests/stl/stringOps.extc sop && asan_ok tests/stl/stringFind.extc sfi; then
    echo "  ok   ASan  ->  vector · setOrdered · setStructKey · stringOps · stringFind 干净"
else
    echo "  FAIL ASan  ->  报了内存问题"; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
