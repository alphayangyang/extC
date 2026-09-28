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

echo "== copyInto<T>：一次带检查的整块搬移（memmove，区间可重叠）+ string::append 批量走它 =="
run_case copyInto tests/stl/copyInto.extc "n=5 b=12345 m=4 a=3456 slen=6 ssum=396"

echo "== sort<T>：introsort（显式栈）· 随机/已升/已降/全相等/风琴管/极小规模 + 结构体键 =="
run_case fromI64 tests/stl/fromI64.extc "min=-9223372036854775808 max=9223372036854775807
bad=0 accepted=0"
run_case from    tests/stl/from.extc  "s=5 e=0 v=3 w=2 sum=6"
run_case sort tests/stl/sort.extc "ok=11 sorted=1 sum=499500 xo=0 sm0=0 sm1=999 i64ok=1 ptok=1 pt0=1"

echo "== vector<T>：grow 摊还（1000 次 push 只搬 14 次 · 1.5 倍）=="
run_case grow    tests/stl/vector_grow.extc    "caps=4,6,9,13,19,28,42,63,94,141,211,316,474,711,1066 n=1000"

echo "== vector<T>：churn 内存平（1e5 与 1e6 两轮，容量策略相同 ⇒ 峰值 RSS 相当）=="
TMPC=$(mktemp -d)
if "$EXTC" tests/stl/vector_churn_a.extc -o "$TMPC/a.c" >/dev/null 2>&1 \
   && "$EXTC" tests/stl/vector_churn_b.extc -o "$TMPC/b.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC/a" "$TMPC/a.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC/b" "$TMPC/b.c" >/dev/null 2>&1; then
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
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC2/p" "$TMPC2/p.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC2/a" "$TMPC2/a.c" >/dev/null 2>&1; then
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

echo "== 用户自定义池底容器（写在入口文件里，不是库模块）：板块 + 池记录 + 提权 =="
run_case custom  tests/stl/custom_pool.extc  "n=10 cap=13 v9=81 live=1 bytes=104"

echo "== varArray：唯一能装引用的容器（arena 底 ⇒ 元素地址永不移动/复用）=="
# 视图是"快照"而不是悬垂：扩容不释放旧块 ⇒ 读到旧值；带出调用安全（三条事实一次钉住）
run_case varArray_snapshot tests/stl/varArray_snapshot.extc "snap=11,22 len=2 live=55,33 alen=3 lend=7,9 tlen=3"
# 写的一面：扩容后通过旧 mut 视图写 ⇒ 静默丢（新块看不到），ASan 证明不是内存不安全
run_case varArray_mutview_lost tests/stl/varArray_mutview_lost.extc "landed=77 live0=77 live1=22 stale=77,88 alen=3"
run_case varref  tests/stl/vararray_ref.extc  "a_data=2 b_data=1 n=1"

echo "== string：连续字节串（append 触发 1.5 倍扩容 · asSlice 连续可直印 · shrink 降水位）=="
echo "== 深拷贝：`var b = a` 不是深拷贝（共用板块）⇒ 要独立副本必须显式 a.clone() =="
echo "== 共享存储的警告：该报的要报（var b = a / return a），不该报的**一条都不许有** =="
check_copy_warn() {
    local f=$1 want=$2 out
    out=$("$EXTC" "$f" 2>&1 || true)
    local got=0
    echo "$out" | grep -q "shares storage" && got=$((got+1))
    echo "$out" | grep -q "copies a \`@sharesStorage\`" && got=$((got+1))
    if [ "$got" = "$want" ]; then
        echo "  ok   $(basename $f)  ->  警告 $got 条（期望 $want）"
    else
        echo "  FAIL $(basename $f)  ->  警告 $got 条（期望 $want）"; fail=1
    fi
}
check_copy_warn tests/stl/clone_warn.extc 2
check_copy_warn tests/stl/clone_ok.extc   0

run_case clone   tests/stl/clone.extc   "vec=3/4 vecOk=1,2,3 str=2/3 map=2/3 hm=1/2 set=1/2 lin=1/2 lset=1/2 hset=1/2 vecAfterRelease=4"

run_case string  tests/stl/string.extc  "len=5 cap=16 len2=44 cap2=54 shrink=44 text=hello, world! and more bytes to force growth t=abc(3) clear=0/44"

echo "== hashSetI64：无值 map（哈希 / 探测 / 墓碑只有一份实现 —— 建在 hashMapI64<u8> 上）=="
run_case hashset tests/stl/hashSet.extc     "new=2 len=2 again=0 len=2 has2=1 rm=1 gone=0 len=1 hits=200000 cap=16 cleared=0"

echo "== set<T>：**有序**集合（建在 map<T, u8> 上，B+ 树）—— 按序遍历 · put 新元素为真 · lowerBound 排名 =="
run_case setOrdered tests/stl/setOrdered.extc "order=1,2,3,4,5 len=5 dup=0 has3=1 lb3=2 lb6=5 rm=1 after=1,2,4,5 lb4=2"
run_case setStructKey tests/stl/setStructKey.extc "order=1,2,3 len=3 has2=1 lb2=1 first=1 last=3"

echo "== string：拼接（+ / +=）· 比较 · 查找 · 视图 · 当 hashMap 键 =="
run_case stringOps  tests/stl/stringOps.extc  "c=hello world|a=hello world|eq=true|lt=false|find=6|miss=-1|sw=true|ew=true|at=101|sub=world|cb=hello world!!!|hn=2|hv=20"

echo "== string::find：穷举对拍（{a,b} 上 1..4 的模式 × 0..10 的文本 vs 朴素查找）+ 逐条边界 =="
run_case stringFind tests/stl/stringFind.extc "checked=61410|edge=0|bad=0"

echo "== std::sys::mem：运行期原语的边界（空 hay · 空 needle · needle 等于整串 · 自重叠 · 零长视图）=="
run_case memFind tests/stl/memFind.extc "e0=0 e1=-1 e2=0 e3=-1 e4=0 e5=0 e6=2 e7=4 e8=-1 e9=5 z0=0 z1=-1 eq=1,0,1"

echo "== 非 GNU 回退（memchr + memcmp）：同一份生成的 C 撤掉 __linux__ 再编一次，输出必须逐字节相同 =="
# 判据是"两条路给同一个答案"，不是"回退那一段编得过"：`#else` 里是自己写的扫描循环，
# 最左匹配、空模式、越界这几条都要再走一遍（自重叠模式最容易在这种手写循环里写错）。
TMPFB=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$TMPFB/m.c" tests/stl/memFind.extc >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPFB/m" "$TMPFB/m.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -U__linux__ -o "$TMPFB/mfb" "$TMPFB/m.c" >/dev/null 2>&1 \
   && "$TMPFB/m" > "$TMPFB/a.out" 2>&1 && "$TMPFB/mfb" > "$TMPFB/b.out" 2>&1; then
    if cmp -s "$TMPFB/a.out" "$TMPFB/b.out"; then
        echo "  ok   memFind 回退  ->  两条路输出逐字节相同：$(cat "$TMPFB/a.out")"
    else
        echo "  FAIL memFind 回退  ->  非 GNU 回退与 memmem 不一致"; diff "$TMPFB/a.out" "$TMPFB/b.out" | head -4; fail=1
    fi
else
    echo "  FAIL memFind 回退  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMPFB"

echo "== string::find 走运行期 memmem（生成物断言：extc_memFind 出现、并且只在要它的程序里）=="
# 判据两头都要：① 用了 find 的程序，生成的 C 里**有** `extc_memFind`（不是又退回逐字节循环）；
# ② 从没用过 `std::sys::mem` 的程序，生成的 C 里**没有**（否则这条运行期是白带的）。
TMPF=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$TMPF/ops.c" tests/stl/stringOps.extc >/dev/null 2>&1 \
   && "$EXTC" -w --no-line-map -o "$TMPF/vec.c" tests/stl/vector.extc >/dev/null 2>&1; then
    if grep -q "extc_memFind" "$TMPF/ops.c" && ! grep -q "extc_memFind" "$TMPF/vec.c"; then
        echo "  ok   memFind  ->  stringOps 的 C 里有 extc_memFind · vector 的 C 里没有（按需发射 ✓）"
    else
        echo "  FAIL memFind  ->  stringOps=$(grep -c extc_memFind "$TMPF/ops.c") 处 · vector=$(grep -c extc_memFind "$TMPF/vec.c") 处"; fail=1
    fi
else
    echo "  FAIL memFind  ->  生成失败"; fail=1
fi
rm -rf "$TMPF"

echo "== string：churn 内存平（1e5 与 1e6 两轮）=="
TMP2=$(mktemp -d)
if "$EXTC" tests/stl/string_churn_a.extc -o "$TMP2/a.c" >/dev/null 2>&1 \
   && "$EXTC" tests/stl/string_churn_b.extc -o "$TMP2/b.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMP2/a" "$TMP2/a.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMP2/b" "$TMP2/b.c" >/dev/null 2>&1; then
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
      && gcc -fwrapv -std=c11 -g -fsanitize=address -o "$TMP/$2" "$TMP/$2.c" >/dev/null 2>&1 \
      && ! "$TMP/$2" 2>&1 | grep -q Sanitizer
}
if asan_ok tests/stl/varArray_snapshot.extc vas && asan_ok tests/stl/varArray_mutview_lost.extc vam && asan_ok tests/stl/vector.extc v && asan_ok tests/stl/setOrdered.extc so && asan_ok tests/stl/setStructKey.extc ssk && asan_ok tests/stl/stringOps.extc sop && asan_ok tests/stl/stringFind.extc sfi; then
    echo "  ok   ASan  ->  varArray_snapshot · varArray_mutview_lost · vector · setOrdered · setStructKey · stringOps · stringFind 干净"
else
    echo "  FAIL ASan  ->  报了内存问题"; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
