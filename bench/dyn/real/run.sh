#!/usr/bin/env bash
# 【现实负载】dyn Trait：像真程序那样用 —— 派发只占每元素工作的一小部分。
#
# 模型：1024 个形状（rect/circle 交替）扫 P 遍；每元素 = 一次带参数的派发 + MIX 步混合 + 累加。
#   · dyn      extC：[1024]dyn Shape，下标遍历，派发时传参（bias）
#   · static   extC：**静态分发的等价写法** —— 两个具体数组成对处理
#              （顺序与 dyn 逐位一致：rect₀, circle₀, rect₁, circle₁…，且没有逐元素类型分支）
#   · cpp      C++：Shape* 数组 + 虚调用（同样传参）
#   · fptr     C：{函数指针, 载荷指针} 数组
#
# **两处防优化（本轮实测踩到过）**：
#   ① 形状字段来自不透明种子（两个 getpid 之差，运行期恒 0）⇒ 面积不是常量；
#   ② 每元素的混合输入**依赖运行中的累加器**（`total & 255`）⇒ 累加是真正递推的，
#      编译器既不能提升也不能闭式化（否则量出来是假的 0ns —— 静态那条路就被折叠过两次）。
set -u
cd "$(dirname "$0")/../../.."
P=${P:-4096}      # 扫描遍数；总派发次数 = 1024*P
MIX=${MIX:-8}     # 每元素混合步数：越大 ⇒ 派发占比越小
CC=${CC:-gcc}; CXX=${CXX:-g++}
mkdir -p build/dyn-real; D=$PWD/build/dyn-real

cat > "$D/dyn.extc" <<EOF
use std::io
extern!("libc") fn clock() -> i64
    effects Addr=0 Cont=0
extern!("libc") fn getpid() -> i32
    effects Addr=0 Cont=0

trait Shape { fn area(self: ref Self, bias: i64) -> i64 }
struct rect   { w: i64  h: i64 }
struct circle { r: i64 }
impl Shape for rect   { fn area(self: ref rect,   bias: i64) -> i64 { return self.w * self.h + bias } }
impl Shape for circle { fn area(self: ref circle, bias: i64) -> i64 { return i64(3) * self.r * self.r + bias } }

fn mix(v: i64) -> i64 {
    var h: i64 = v
    var k: i64 = i64(0)
    while k < i64($MIX) { h = (h * i64(6364136223846793005)) ^ (h >> i64(17)); k = k + i64(1) }
    return h
}

fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    var xs: [1024]dyn Shape
    var j: i64 = i64(0)
    while j < i64(1024) {
        if (j % i64(2)) == i64(0) { xs[j] = dyn Shape(rect   { w: seed + i64(3), h: seed + i64(4) }) }
        else                      { xs[j] = dyn Shape(circle { r: seed + i64(2) }) }
        j = j + i64(1)
    }
    var total: i64 = i64(0)
    let t0 = clock()
    var p: i64 = i64(0)
    while p < i64($P) {
        var i: i64 = i64(0)
        while i < i64(1024) {
            total = total + mix(xs[i].area(total & i64(255)))
            i = i + i64(1)
        }
        p = p + i64(1)
    }
    let t1 = clock()
    io::cout << "sum=" << total << " n=" << i64(1024) * i64($P) << " ns10=" << (t1 - t0) * i64(10000) / (i64(1024) * i64($P)) << "\n"
    return 0
}
EOF

cat > "$D/static.extc" <<EOF
use std::io
extern!("libc") fn clock() -> i64
    effects Addr=0 Cont=0
extern!("libc") fn getpid() -> i32
    effects Addr=0 Cont=0

struct rect   { w: i64  h: i64 }
struct circle { r: i64 }
impl rect   { fn area(self: ref rect,   bias: i64) -> i64 { return self.w * self.h + bias } }
impl circle { fn area(self: ref circle, bias: i64) -> i64 { return i64(3) * self.r * self.r + bias } }

fn mix(v: i64) -> i64 {
    var h: i64 = v
    var k: i64 = i64(0)
    while k < i64($MIX) { h = (h * i64(6364136223846793005)) ^ (h >> i64(17)); k = k + i64(1) }
    return h
}

fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    var rs: [512]rect
    var cs: [512]circle
    var j: i64 = i64(0)
    while j < i64(512) {
        rs[j] = rect   { w: seed + i64(3), h: seed + i64(4) }
        cs[j] = circle { r: seed + i64(2) }
        j = j + i64(1)
    }
    var total: i64 = i64(0)
    let t0 = clock()
    var p: i64 = i64(0)
    while p < i64($P) {
        var i: i64 = i64(0)
        while i < i64(512) {
            total = total + mix(rs[i].area(total & i64(255)))
            total = total + mix(cs[i].area(total & i64(255)))
            i = i + i64(1)
        }
        p = p + i64(1)
    }
    let t1 = clock()
    io::cout << "sum=" << total << " n=" << i64(1024) * i64($P) << " ns10=" << (t1 - t0) * i64(10000) / (i64(1024) * i64($P)) << "\n"
    return 0
}
EOF

cat > "$D/virtual.cpp" <<EOF
#include <cstdio>
#include <ctime>
#include <unistd.h>
struct Shape { virtual ~Shape() = default; virtual long area(long bias) const = 0; };
struct Rect : Shape { long w, h; long area(long bias) const override { return w * h + bias; } };
struct Circle : Shape { long r; long area(long bias) const override { return 3 * r * r + bias; } };
static long mix(long v) { long h = v; for (int k = 0; k < $MIX; k++) h = (h * 6364136223846793005L) ^ (h >> 17); return h; }
int main() {
    long seed = (long)getpid() - (long)getpid();
    Rect rs[512]; Circle cs[512];
    for (int j = 0; j < 512; j++) { rs[j].w = 3 + seed; rs[j].h = 4 + seed; cs[j].r = 2 + seed; }
    Shape *xs[1024];
    for (int i = 0; i < 1024; i++) xs[i] = (i % 2 == 0) ? (Shape *)&rs[i / 2] : (Shape *)&cs[i / 2];
    long total = 0; const long P = $P;
    std::clock_t t0 = std::clock();
    for (long p = 0; p < P; p++) for (int i = 0; i < 1024; i++) total += mix(xs[i]->area(total & 255));
    std::clock_t t1 = std::clock();
    long n = 1024L * P;
    printf("sum=%ld n=%ld ns10=%ld\n", total, n, (long)((t1 - t0) * 10000000000L / CLOCKS_PER_SEC / n));
    return 0;
}
EOF

cat > "$D/fptr.c" <<EOF
#include <stdio.h>
#include <time.h>
#include <unistd.h>
struct Shape { long (*area)(void *, long); void *p; };
static long rect_area(void *p, long bias)   { long *v = p; return v[0] * v[1] + bias; }
static long circle_area(void *p, long bias) { long *v = p; return 3 * v[0] * v[0] + bias; }
static long mix(long v) { long h = v; for (int k = 0; k < $MIX; k++) h = (h * 6364136223846793005L) ^ (h >> 17); return h; }
int main(void) {
    long seed = (long)getpid() - (long)getpid();
    long rs[512][2], cs[512][1];
    for (int j = 0; j < 512; j++) { rs[j][0] = 3 + seed; rs[j][1] = 4 + seed; cs[j][0] = 2 + seed; }
    struct Shape xs[1024];
    for (int i = 0; i < 1024; i++) {
        if (i % 2 == 0) { xs[i].area = rect_area;   xs[i].p = rs[i / 2]; }
        else            { xs[i].area = circle_area; xs[i].p = cs[i / 2]; }
    }
    long total = 0; const long P = $P;
    clock_t t0 = clock();
    for (long p = 0; p < P; p++) for (int i = 0; i < 1024; i++) total += mix(xs[i].area(xs[i].p, total & 255));
    clock_t t1 = clock();
    long n = 1024L * P;
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

for v in dyn static; do "$PWD/build/extc" "$D/$v.extc" -o "$D/$v.c" >/dev/null 2>&1 && $CC -O2 -fwrapv "$D/$v.c" -o "$D/$v" || { echo "$v 编不过"; exit 1; }; done
$CXX -O2 -std=c++17 -fwrapv "$D/virtual.cpp" -o "$D/virtual" || exit 1
$CC  -O2 -fwrapv "$D/fptr.c" -o "$D/fptr" || exit 1

printf "【现实负载】P=%s MIX=%s（每元素 1 次派发 + MIX 步混合；混合输入依赖累加器）  %s\n" "$P" "$MIX" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
printf "%-8s %-22s %10s %10s %10s %8s\n" 路 校验和 ns/元素 秒 峰值RSS 状态
rows=0
fails=0
base=""
for pair in "dyn:$D/dyn" "static:$D/static" "cpp:$D/virtual" "fptr:$D/fptr"; do
    name=${pair%%:*}; bin=${pair#*:}
    rows=$((rows+1))
    out=$("$bin"); rss=$( { /usr/bin/time -f "%M" "$bin" >/dev/null; } 2>&1 | tail -1 )
    ns10=$(best_ns "$bin")
    sum=$(echo "$out" | sed -n 's/.*sum=\([0-9-]*\).*/\1/p'); n=$(echo "$out" | sed -n 's/.*n=\([0-9]*\).*/\1/p'); ns10=$(echo "$out" | sed -n 's/.*ns10=\([0-9]*\).*/\1/p')
    [ -z "$base" ] && base=$sum
    st=ok; [ "$sum" != "$base" ] && { st="校验和不符"; fails=$((fails+1)); }
    printf "%-8s %-22s %10s %10s %10s %8s\n" "$name" "$sum" "$(awk -v x="$ns10" 'BEGIN{printf "%.1f", x/10}')" "$(awk -v s="$ns10" -v n="$n" 'BEGIN{printf "%.3f", s*n/1e10}')" "$rss" "$st"
done
printf "通过 %d，失败 %d\n" "$((rows - fails))" "$fails"
[ "$fails" = 0 ] || exit 1
