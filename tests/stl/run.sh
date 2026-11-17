#!/usr/bin/env bash
# tests/stl/run.sh —— **STL 库的常设验收**（一个库装所有动态容器；容器都建在池上）
#
# 判据：
#   ① 正例：每个容器一条「整行期望输出」（自带数字自证）
#   ② ASan 干净：容器路径不许有内存问题
set -u
# LeakSanitizer 对这套内存模型没有意义（arena 到进程结束才释放），它在最小发行版里还会偶尔
# 在退出扫描时挂住 ⇒ 这里只抓越界与 use-after-free。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
# gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核会**偶发**在启动阶段自己 SEGV
# （stderr 只有 DEADLYSIGNAL，然后挂住）；实测 32 路并发 9/32 次，关掉地址随机化后 0/32。
# 原因与三条出路见 tests/asan-probe.sh。这里只对本套件关随机化。
if [ -z "${EXTC_ASAN_NO_SETARCH:-}" ] && command -v setarch >/dev/null 2>&1; then
    EXTC_ASAN_NO_SETARCH=1 exec setarch "$(uname -m)" -R bash "$0" "$@"
fi
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
BUSY=$(mktemp -d); trap 'rm -rf "$BUSY"' EXIT

run_case() {   # run_case <名字> <文件> <期望的一整行>
    local name=$1 f=$2 want=$3 out
    if ! out=$("$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if [ "$out" = "$want" ]; then echo "  ok   $name  ->  $out"
    else echo "  FAIL $name  ->  期望「$want」，得到「$out」"; fail=1; fi
}

traps() {   # traps <名字> <文件> <期望的运行期消息子串>
    local name=$1 f=$2 want=$3 out
    if out=$("$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  应当运行期失败却成功了：$(echo "$out" | tail -1)"; fail=1; return
    fi
    if echo "$out" | grep -qF -- "$want"; then
        echo "  ok   $name  ->  运行期 trap ✓"
    else
        echo "  FAIL $name  ->  消息不对：$(echo "$out" | tail -1 | cut -c1-70)"; fail=1
    fi
}

rejects() { # rejects <名字> <文件> <期望的诊断子串>
  if "$EXTC" -w --no-line-map -o "$BUSY/r.c" "$2" 2>"$BUSY/r.err"; then
    echo "  FAIL $1  ->  被接受了（应当报「$3」）"; fail=1
  elif grep -qF -- "$3" "$BUSY/r.err"; then
    echo "  ok   $1  ->  被拒 ✓"
  else
    echo "  FAIL $1  ->  理由不对：$(grep -m1 error "$BUSY/r.err" | cut -c1-70)"; fail=1
  fi
}

echo "== buf<T>：拥有存储（增长还旧板 / 离开作用域还全部）=="
run_case varArray_release tests/stl/varArray_release.extc "grown>0=true after<1024=true sum=0"
run_case buf     tests/stl/buf.extc           "len=1 cap=13 sum=45 c9=9 v0=100 leak=0 growsum=4999950000 gleak=0"
run_case buf_new tests/stl/buf_new.extc       "len=0 cap=42 sum=780 owned=true"
BX=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$BX/bx.c" tests/stl/buf_new_wrong_elem.extc 2>&1 | grep -q "initializer expects"; then
    echo "  ok   buf_new_wrong_elem  ->  元素类型不符被拒 ✓"
else
    echo "  FAIL buf_new_wrong_elem  ->  应当报 `initializer expects`"; fail=1
fi
rm -rf "$BX"

echo "== vector<T>：1.5 倍扩容 / dense 连续 / shrink 降水位 / clear 留容量 =="
run_case vector  tests/stl/vector.extc  "cap0=4 n=9 cap=9 sum=36 shrink=9 pop=8 n=8 clear=0/9 tail=-9"

echo "== copyInto<T>：一次带检查的整块搬移（memmove，区间可重叠）+ string::append 批量走它 =="
run_case copyInto tests/stl/copyInto.extc "n=5 b=12345 m=4 a=3456 slen=6 ssum=396"

echo "== sort<T>：introsort（显式栈）· 随机/已升/已降/全相等/风琴管/极小规模 + 结构体键 =="
run_case fromI64 tests/stl/fromI64.extc "min=-9223372036854775808 max=9223372036854775807
bad=0 accepted=0"
run_case from    tests/stl/from.extc  "s=5 e=0 v=3 w=2 sum=6"
run_case sort tests/stl/sort.extc "ok=11 sorted=1 sum=499500 xo=0 sm0=0 sm1=999 i64ok=1 ptok=1 pt0=1"

echo "== stl::seq：归约（空视图给 option · 从左到右顺序 · f64 与 i64 两种实例化）=="
run_case seq     tests/stl/seq.extc     "sum=10500000 max=4500000 min=1000000 isum=20 imax=7 imin=3 c=2 e=7 c0=0"

echo "== stl::tensor：二维 strided 视图（连续 / 转置 / 子矩阵 / 步长 / 失败路径）=="
run_case tensor  tests/stl/tensor.extc  "g=6 w=true g2=99 s=159 ts=159 rs=1 ss=141 bsum=-1 ior=false fs=42 as=183 cs=12 st=12"
run_case nd      tests/stl/nd.extc      "f=true s=true a=true x=true s1=24 s2=72 s3=96 s4=168 st=24 s5=12 bad=false s6=-1 cnt=16 c3=24"
run_case nd_sub  tests/stl/nd_sub.extc  "x=12 y=23 s=66 z=12 w=-5 q=3 r=-10"
run_case nd_sub3 tests/stl/nd_sub3.extc "r=120 w=4 p=123"
run_case nd_direct tests/stl/nd_direct.extc "q=50 r=52 s=122 z=9"
run_case nd_expr tests/stl/nd_expr.extc "add=0,22 44
sub=0,-18 -36
mul=0,40 160
sc=0,4 8
axpy=0,12 24
pe=0,14 28"
run_case split tests/stl/split.extc "n=4 total=6 e=0"
run_case string_ops tests/stl/string_ops.extc "n=3 j=[ a | b | c ] t1=[hi] t2=[x ] t3=[ y] e=0"
run_case string_tof64 tests/stl/string_tof64.extc "3.14 -0.5 42 1000 0.015 0.25 -1 -1 -1"
run_case string_plus tests/stl/string_plus.extc "abcde ababcde x#7"
run_case string_plus_num tests/stl/string_plus_num.extc "a=7 b=3 c=2.500000 d=0.500000 e=true
x7!"
run_case slice_eq_any_elem tests/stl/slice_eq_any_elem.extc "eq=true got=1"
run_case string_borrow tests/stl/string_borrow.extc "first=97 via=abc same=true"
run_case string_print tests/stl/string_print.extc "hi 3
second"
run_case string_build tests/stl/string_build.extc "n=-42 b=3.14 big=-9223372036854775808 | 1,2,3,4,5 | e=0"
run_case string_fmt tests/stl/string_fmt.extc "pi=[3.14] neg=[-0.500] big=[1234.5] i=[-42]"

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

TMPC3=$(mktemp -d)
echo "== buf<T>：增长峰值是"一份"不是"历代之和"（同一形状 vs arena 底）=="
# 判据：8e6 个 i64（活跃 64 MB）—— `buf` 扩容当场还旧板，`varArray` 的旧块只能等帧退出。
if "$EXTC" -w --no-line-map -o "$TMPC3/b.c" tests/stl/buf_rss.extc >/dev/null 2>&1 \
   && "$EXTC" -w --no-line-map -o "$TMPC3/a.c" tests/stl/buf_rss_arena.extc >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC3/b" "$TMPC3/b.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC3/a" "$TMPC3/a.c" >/dev/null 2>&1; then
    kb=$(/usr/bin/time -f %M "$TMPC3/b" 2>&1 >/dev/null | tail -1)
    ka=$(/usr/bin/time -f %M "$TMPC3/a" 2>&1 >/dev/null | tail -1)
    if [ "$kb" -lt $(( ka * 3 / 4 )) ]; then
        echo "  ok   buf rss  ->  buf ${kb} KB vs arena ${ka} KB（低 $(( (ka - kb) * 100 / ka ))%）"
    else
        echo "  FAIL buf rss  ->  buf ${kb} KB 没有明显低于 arena ${ka} KB"; fail=1
    fi
else
    echo "  FAIL buf rss  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMPC3"

echo "== buf<T>：对抗路径（搬家逐元素对位 · shrink 后重新长 · 两万个小缓冲区 churn · 冻结位）=="
run_case buf_adv    tests/stl/buf_adv.extc    "n=3 sum=44999850000 bad=0 v0=7 cap=4 empty=-1 leak=0 live=35651584"
run_case buf_struct tests/stl/buf_struct.extc "n=400000 bad=0 sum=79999800000 moves=1 capGE=true"
traps    buf_frozen tests/stl/buf_frozen.extc "this buffer is frozen by a live borrow"
run_case buf_fuzz  tests/stl/buf_fuzz.extc  "rounds=3000 bad=0 worst=0 leak=0"
run_case buf_huge  tests/stl/buf_huge.extc  "n=10 sum=45 max=134217728 reserved=1073741824 live=1073741824"
# 同一程序里有 `par`：发射器必须给 buf 基质带锁（EXTC_BUF_MT 1），判据是它照样跑对。
run_case buf_par   tests/stl/buf_par.extc   "len=100 sum=65280 live=2097152"

echo "== buf<T>：小缓冲区 churn 的预留账本（1e4 与 1e5 两轮，只差轮数）=="
# 判据：轮数 ×11 而峰值 RSS 不许翻倍 —— 预留必须被复用（分配器有一份空闲表），
# 否则 1e5 轮要 200 GiB 地址空间、RSS 也会跟着涨。
TMPC4=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$TMPC4/a.c" tests/stl/buf_churn_a.extc >/dev/null 2>&1 \
   && "$EXTC" -w --no-line-map -o "$TMPC4/b.c" tests/stl/buf_churn_b.extc >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC4/a" "$TMPC4/a.c" >/dev/null 2>&1 \
   && gcc -fwrapv -std=c11 -O2 -o "$TMPC4/b" "$TMPC4/b.c" >/dev/null 2>&1; then
    ka=$(/usr/bin/time -f %M "$TMPC4/a" 2>&1 >/dev/null | tail -1)
    kb=$(/usr/bin/time -f %M "$TMPC4/b" 2>&1 >/dev/null | tail -1)
    la=$("$TMPC4/a" 2>/dev/null); lb=$("$TMPC4/b" 2>/dev/null)
    if [ "$kb" -le $(( ka * 2 )) ] && [ "$la" = "rounds=10000 leak=0" ] && [ "$lb" = "rounds=100000 leak=0" ]; then
        echo "  ok   buf churn  ->  1e4: ${ka} KB · 1e5: ${kb} KB ⇒ 平（账本回零）"
    else
        echo "  FAIL buf churn  ->  1e4: ${ka} KB / ${la} · 1e5: ${kb} KB / ${lb}"; fail=1
    fi
else
    echo "  FAIL buf churn  ->  生成或编译失败"; fail=1
fi
rm -rf "$TMPC4"

echo "== 用户自定义池底容器（写在入口文件里，不是库模块）：板块 + 池记录 + 提权 =="
run_case custom  tests/stl/custom_pool.extc  "n=10 cap=13 v9=81 live=1 bytes=104"
run_case custom-grow tests/stl/custom_pool_grow.extc "n=2000000 cap=2362204 bytes=18897632 first=0 last=1999999 live_after=0"

echo "== varArray：唯一能装引用的容器（arena 底 ⇒ 元素地址永不移动/复用）=="
# 视图是"快照"而不是悬垂：扩容不释放旧块 ⇒ 读到旧值；带出调用安全（三条事实一次钉住）
rejects varArray_stale_view tests/stl/varArray_stale_view.extc "changes the storage that"
rejects varArray_stale_write tests/stl/varArray_stale_write.extc "changes the storage that"
# 写的一面：扩容后通过旧 mut 视图写 ⇒ 静默丢（新块看不到），ASan 证明不是内存不安全

run_case varref  tests/stl/vararray_ref.extc  "a_data=2 b_data=1 n=1"

echo "== string：连续字节串（append 触发 1.5 倍扩容 · view 连续可直印 · shrink 降水位）=="
echo "== 深拷贝：\`var b = a\` 不是深拷贝（共用板块）⇒ 要独立副本必须显式 a.clone() =="
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
run_case stringEdit  tests/stl/stringEdit.extc  "r1=2 r2=2 r3=2 erase=5 s=[ World! zz there.] rf=9/1 ff=9 first=true u=_ixed case 123 w=true bad=false eg=2 e=abef zc=1 z=ba"
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
if asan_ok tests/stl/buf_adv.extc ba && asan_ok tests/stl/buf_struct.extc bs && asan_ok tests/stl/buf_fuzz.extc bfz && asan_ok tests/stl/varArray_release.extc var && asan_ok tests/stl/vector.extc v && asan_ok tests/stl/setOrdered.extc so && asan_ok tests/stl/setStructKey.extc ssk && asan_ok tests/stl/stringOps.extc sop && asan_ok tests/stl/stringEdit.extc sedit && asan_ok tests/stl/stringFind.extc sfi; then
    echo "  ok   ASan  ->  varArray_release · vector · setOrdered · setStructKey · stringOps · stringFind 干净"
else
    echo "  FAIL ASan  ->  报了内存问题"; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
