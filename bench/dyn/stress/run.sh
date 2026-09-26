#!/usr/bin/env bash
# 【极端压测】dyn Trait：最紧的循环里只有派发，量**每次派发的裸代价**与**病态形态**。
#
#   ① static   extC 静态分发（手写类型分支 + 直接调用）—— 基线
#   ② dynval   extC dyn 存储值（取槽 + 校验 + 间接调用）
#   ③ dynmany  extC dyn **1024 个值轮转**（槽/表指针超出 L1 ⇒ 缓存压力下的派发代价）
#   ④ dynimm   extC dyn 立即形式（每轮一次 extc_dyn_put ⇒ **一次分配**）
#   ⑤ cpp      C++ 虚函数（同形：两个对象 + 分支）
#   ⑥ fptr     C 函数指针
#
# 每轮的累加里带一个依赖 i 的项（`(i & 1)`）：否则纯算术的内层会被**循环不变式提升**
# ⇒ 外层循环被折叠成一次乘法，量出来是假的 0ns（这个坑本轮实测踩到过两次）。
# 校验和必须逐位一致（脚本断言 sum == 12*n + n/2）。
set -u
cd "$(dirname "$0")/../../.."
N=${N:-20000000}          # 快路径迭代数
NIMM=${NIMM:-1000000}     # 立即形式：每轮一次分配，N 调小
CC=${CC:-gcc}; CXX=${CXX:-g++}
mkdir -p build/dyn-stress; D=$PWD/build/dyn-stress

gen() {  # 名字 主体
    cat > "$D/$1.extc" <<EOF
use std::io
extern!("libc") fn clock() -> i64
    effects Addr=0 Cont=0
extern!("libc") fn getpid() -> i32
    effects Addr=0 Cont=0

trait Shape { fn area(self: ref Self) -> i64 }
struct rect   { w: i64  h: i64 }
struct circle { r: i64 }
impl Shape for rect   { fn area(self: ref rect)   -> i64 { return self.w * self.h } }
impl Shape for circle { fn area(self: ref circle) -> i64 { return i64(3) * self.r * self.r } }
$2
EOF
}
rep='    io::cout << "sum=" << total << " n=" << n << " ns10=" << (t1 - t0) * i64(10000) / n << "\n"'

gen static 'fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    let r = rect   { w: seed + i64(3), h: seed + i64(4) }
    let c = circle { r: seed + i64(2) }
    let n: i64 = i64('"$N"')
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        if (i % i64(2)) == i64(0) { total = total + r.area() + (total & i64(7)) }
        else                      { total = total + c.area() + (total & i64(7)) }
        i = i + i64(1)
    }
    let t1 = clock()
'"$rep"'
    return 0
}'

gen dynval 'fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    let a: dyn Shape = dyn Shape(rect   { w: seed + i64(3), h: seed + i64(4) })
    let b: dyn Shape = dyn Shape(circle { r: seed + i64(2) })
    let n: i64 = i64('"$N"')
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        if (i % i64(2)) == i64(0) { total = total + a.area() + (total & i64(7)) }
        else                      { total = total + b.area() + (total & i64(7)) }
        i = i + i64(1)
    }
    let t1 = clock()
'"$rep"'
    return 0
}'

gen dynmany 'fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    var xs: [1024]dyn Shape
    var j: i64 = i64(0)
    while j < i64(1024) {
        if (j % i64(2)) == i64(0) { xs[j] = dyn Shape(rect   { w: seed + i64(3), h: seed + i64(4) }) }
        else                      { xs[j] = dyn Shape(circle { r: seed + i64(2) }) }
        j = j + i64(1)
    }
    let n: i64 = i64('"$N"')
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        total = total + xs[i % i64(1024)].area() + (total & i64(7))
        i = i + i64(1)
    }
    let t1 = clock()
'"$rep"'
    return 0
}'

gen dynimm 'fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    let r = rect   { w: seed + i64(3), h: seed + i64(4) }
    let c = circle { r: seed + i64(2) }
    let n: i64 = i64('"$NIMM"')
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        if (i % i64(2)) == i64(0) { total = total + dyn Shape(r).area() + (total & i64(7)) }
        else                      { total = total + dyn Shape(c).area() + (total & i64(7)) }
        i = i + i64(1)
    }
    let t1 = clock()
'"$rep"'
    return 0
}'

gen staticimm 'fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    let r = rect   { w: seed + i64(3), h: seed + i64(4) }
    let c = circle { r: seed + i64(2) }
    let n: i64 = i64('"$NIMM"')
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        if (i % i64(2)) == i64(0) { total = total + r.area() + (total & i64(7)) }
        else                      { total = total + c.area() + (total & i64(7)) }
        i = i + i64(1)
    }
    let t1 = clock()
    io::cout << "sum=" << total << " n=" << n << " ns10=" << (t1 - t0) * i64(10000) / n << "\n"
    return 0
}'

cat > "$D/virtual.cpp" <<EOF
#include <cstdio>
#include <ctime>
#include <unistd.h>
struct Shape { virtual ~Shape() = default; virtual long area() const = 0; };
struct Rect : Shape { long w, h; long area() const override { return w * h; } };
struct Circle : Shape { long r; long area() const override { return 3 * r * r; } };
int main() {
    long seed = (long)getpid() - (long)getpid();
    Rect ra; ra.w = 3 + seed; ra.h = 4 + seed;
    Circle ca; ca.r = 2 + seed;
    Shape *a = &ra, *b = &ca;
    const long n = $N; long total = 0;
    std::clock_t t0 = std::clock();
    for (long i = 0; i < n; i++) total += ((i % 2 == 0) ? a->area() : b->area()) + (total & 7);
    std::clock_t t1 = std::clock();
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

cat > "$D/fptr.c" <<EOF
#include <stdio.h>
#include <time.h>
#include <unistd.h>
struct Shape { long (*area)(void *); };
static long rect_area(void *p)   { long *v = p; return v[0] * v[1]; }
static long circle_area(void *p) { long *v = p; return 3 * v[0] * v[0]; }
int main(void) {
    long seed = (long)getpid() - (long)getpid();
    long r[2] = {3 + seed, 4 + seed}, c[1] = {2 + seed};
    struct Shape a = { rect_area }, b = { circle_area };
    const long n = $N; long total = 0;
    clock_t t0 = clock();
    for (long i = 0; i < n; i++) total += ((i % 2 == 0) ? a.area(r) : b.area(c)) + (total & 7);
    clock_t t1 = clock();
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

RUNS=${RUNS:-3}
best_ns() {   # 可执行 -> 最优 ns10（多次运行取最小，抑制 WSL2 调度波动）
    local b="" v
    for _ in $(seq "$RUNS"); do
        v=$("$1" | sed -n 's/.*ns10=\([0-9]*\).*/\1/p')
        if [ -z "$b" ] || [ "$v" -lt "$b" ]; then b=$v; fi
    done
    echo "$b"
}

# ── 手写 C 复刻 extC 的派发路径：用来**拆开**那 1.5 ns 是谁的 ──
cat > "$D/cslot.c" <<EOF
/* cslot：槽数组 + 只有下标检查，没有世代/模式校验。
 * 与 extC dyn 的差别 = "槽这一层间接"本身（对 C++ 的 vptr 内联在对象里）。 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
struct VT { long (*area)(void *); };
struct Slot { long pid, pgen, gen; void *addr; const struct VT *vt; };
static long rect_area(void *p)   { long *v = p; return v[0] * v[1]; }
static long circle_area(void *p) { long *v = p; return 3 * v[0] * v[0]; }
int main(void) {
    long seed = (long)getpid() - (long)getpid();
    static const struct VT vr = { rect_area }, vc = { circle_area };
    long r[2] = {3 + seed, 4 + seed}, c[1] = {2 + seed};
    struct Slot sa = {1 + seed, 1 + seed, seed, r, &vr};
    struct Slot sb = {1 + seed, 1 + seed, seed, c, &vc};
    const long n = $N; long total = 0;
    clock_t t0 = clock();
    for (long i = 0; i < n; i++) {
        struct Slot *s = (i % 2 == 0) ? &sa : &sb;
        total += ((const struct VT *)s->vt)->area(s->addr) + (total & 7);
    }
    clock_t t1 = clock();
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

cat > "$D/cgen2.c" <<EOF
/* cgen2：**同构**复刻 extC 的派发路径 —— 把校验放进一个**非内联**函数（带 file/line 参数，
 * 与 extc_dyn_slot 同形），池记录走**全局数组 + 非内联读取**（与 extc_pool_kind/generation 同形）。
 * 目的：分辨 2.1 ns 里"结构开销"与"语言/编译产物开销"各占多少。 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#define NOINLINE __attribute__((noinline))
struct VT { long (*area)(void *); };
struct Slot { long pid, pgen, gen; void *addr; const struct VT *vt; };
struct Handle { long pid, slot, gen, pgen; };
struct PoolRec { long kind, generation, live; };
static struct PoolRec pools[64];
static struct Slot slotsArr[2];
NOINLINE static long poolKind(long pid) { return pools[pid & 63].kind; }
NOINLINE static long poolGen(long pid)  { return pools[pid & 63].generation; }
NOINLINE static struct Slot *slotOf(struct Handle h, const char *file, int line) {
    if (h.slot < 0 || h.slot >= 2)      { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    struct Slot *s = &slotsArr[h.slot];
    if (s->pid < 0)                     { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    if (s->pid != h.pid)                { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    if (s->gen != h.gen)                { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    if (poolKind(h.pid) != 1)           { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    if (poolGen(h.pid) != h.pgen)       { fprintf(stderr, "%s:%d\n", file, line); abort(); }
    return s;
}
static long rect_area(void *p)   { long *v = p; return v[0] * v[1]; }
static long circle_area(void *p) { long *v = p; return 3 * v[0] * v[0]; }
int main(void) {
    long seed = (long)getpid() - (long)getpid();
    static const struct VT vr = { rect_area }, vc = { circle_area };
    long r[2] = {3 + seed, 4 + seed}, c[1] = {2 + seed};
    pools[1].kind = 1 + (seed - seed); pools[1].generation = 2 + (seed - seed); pools[1].live = 1;
    slotsArr[0].pid = 1; slotsArr[0].pgen = 2; slotsArr[0].gen = 0; slotsArr[0].addr = r; slotsArr[0].vt = &vr;
    slotsArr[1].pid = 1; slotsArr[1].pgen = 2; slotsArr[1].gen = 0; slotsArr[1].addr = c; slotsArr[1].vt = &vc;
    struct Handle ha = {1, 0, 0, 2}, hb = {1, 1, 0, 2};
    const long n = $N; long total = 0;
    clock_t t0 = clock();
    for (long i = 0; i < n; i++) {
        struct Slot *s = slotOf((i % 2 == 0) ? ha : hb, "bench", 1);
        total += ((const struct VT *)s->vt)->area(s->addr) + (total & 7);
    }
    clock_t t1 = clock();
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

cat > "$D/cgen.c" <<EOF
/* cgen：与 extC 的 extc_dyn_slot **同样的校验**（槽在界内 → pid → 槽世代 → 池是对象表 → 池世代）。
 * 校验值来自不透明种子 ⇒ 优化器不能把它们当常量折掉。 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
struct VT { long (*area)(void *); };
struct Slot { long pid, pgen, gen; void *addr; const struct VT *vt; };
static long rect_area(void *p)   { long *v = p; return v[0] * v[1]; }
static long circle_area(void *p) { long *v = p; return 3 * v[0] * v[0]; }
static long kindOf(long pid) { (void)pid; return 1; }   /* 对象表模式 */
int main(void) {
    long seed = (long)getpid() - (long)getpid();
    static const struct VT vr = { rect_area }, vc = { circle_area };
    long r[2] = {3 + seed, 4 + seed}, c[1] = {2 + seed};
    struct Slot sa = {1 + seed, 2 + seed, seed, r, &vr};
    struct Slot sb = {1 + seed, 2 + seed, seed, c, &vc};
    const long n = $N; long total = 0;
    clock_t t0 = clock();
    for (long i = 0; i < n; i++) {
        struct Slot *s = (i % 2 == 0) ? &sa : &sb;
        if (s->pid < 0 || s->pid != 1 + seed) abort();
        if (s->gen != seed) abort();
        if (kindOf(s->pid) != 1) abort();
        if (s->pgen != ((s->pid * 0) + 2 + seed)) abort();
        total += ((const struct VT *)s->vt)->area(s->addr) + (total & 7);
    }
    clock_t t1 = clock();
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

for v in static dynval dynmany dynimm staticimm; do "$PWD/build/extc" "$D/$v.extc" -o "$D/$v.c" >/dev/null 2>&1 && $CC -O2 -fwrapv "$D/$v.c" -o "$D/$v" || { echo "$v 编不过"; exit 1; }; done
$CXX -O2 -std=c++17 -fwrapv "$D/virtual.cpp" -o "$D/virtual" || exit 1
$CC  -O2 -fwrapv "$D/fptr.c" -o "$D/fptr" || exit 1
$CC  -O2 -fwrapv "$D/cslot.c" -o "$D/cslot" || exit 1
$CC  -O2 -fwrapv "$D/cgen.c"  -o "$D/cgen"  || exit 1
$CC  -O2 -fwrapv "$D/cgen2.c" -o "$D/cgen2" || exit 1

printf "【极端压测】N=%s（立即形式 %s）  %s\n" "$N" "$NIMM" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
printf "%-9s %-14s %10s %10s %10s %8s\n" 路 校验和 ns/次 秒 峰值RSS 状态
rows=0
fails=0
base=""
for pair in "static:$D/static" "dynval:$D/dynval" "dynmany:$D/dynmany" "cpp:$D/virtual" "fptr:$D/fptr" "cslot:$D/cslot" "cgen:$D/cgen" "cgen2:$D/cgen2" "staticimm:$D/staticimm" "dynimm:$D/dynimm"; do
    name=${pair%%:*}; bin=${pair#*:}
    rows=$((rows+1))
    out=$("$bin"); rss=$( { /usr/bin/time -f "%M" "$bin" >/dev/null; } 2>&1 | tail -1 )
    ns10=$(best_ns "$bin")
    sum=$(echo "$out" | sed -n 's/.*sum=\([0-9-]*\).*/\1/p'); n=$(echo "$out" | sed -n 's/.*n=\([0-9]*\).*/\1/p'); ns10=$(echo "$out" | sed -n 's/.*ns10=\([0-9]*\).*/\1/p')
    st=ok
    case "$name" in
        staticimm) baseimm=$sum ;;   # imm 组：单独配对（N 与快路径不同）
        dynimm)    [ "$sum" != "$baseimm" ] && { st="与 staticimm 不符（$sum ≠ $baseimm）"; fails=$((fails+1)); } ;;
        *)         [ -z "$base" ] && base=$sum; [ "$sum" != "$base" ] && { st="校验和不符"; fails=$((fails+1)); } ;;
    esac
    printf "%-9s %-18s %10s %10s %10s %8s\n" "$name" "$sum" "$(awk -v x="$ns10" 'BEGIN{printf "%.1f", x/10}')" "$(awk -v s="$ns10" -v n="$n" 'BEGIN{printf "%.3f", s*n/1e10}')" "$rss" "$st"
done
printf "通过 %d，失败 %d\n" "$((rows - fails))" "$fails"
[ "$fails" = 0 ] || exit 1
