#!/usr/bin/env bash
# 【构造成本】"拷贝 vs 分配" —— N 次**存储值**构造 + 一次派发，两个口径一起量：
#   ① 墙钟（程序内 clock()）：ns/次；
#   ② valgrind callgrind（装了才跑）：**指令级**拆分 —— malloc/free 家族 vs extC 自身代码 vs 载荷拷贝。
#
# 为什么单独一个项目：`real/` 与 `stress/` 量的是**派发**；这里量的是**构造**（即"拷贝一个值进池"），
# 结论是"拷贝几乎免费、每次一个 malloc 才是成本" —— 这决定了"要不要为了省拷贝去设计借用视图"。
set -u
cd "$(dirname "$0")/../../.."
N=${N:-200000}
CC=${CC:-gcc}
mkdir -p build/dyn-alloc; D=$PWD/build/dyn-alloc

cat > "$D/put.extc" <<EOF
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
fn main() -> i32 {
    let seed = i64(getpid()) - i64(getpid())
    let n: i64 = i64($N)
    var total: i64 = i64(0)
    var i: i64 = i64(0)
    let t0 = clock()
    while i < n {
        /* 每轮一个**存储值**：拷进池 + 取用前校验 + 派发；循环体是一个地方 ⇒ 该池随本轮退出被丢。 */
        let d: dyn Shape = dyn Shape(rect { w: seed + i64(3), h: seed + i64(4) })
        total = total + d.area() + (total & i64(7))
        i = i + i64(1)
    }
    let t1 = clock()
    io::cout << "sum=" << total << " n=" << n << " ns10=" << (t1 - t0) * i64(10000) / n << "\n"
    return 0
}
EOF

"$PWD/build/extc" "$D/put.extc" -o "$D/put.c" >/dev/null 2>&1 && $CC -O2 -fwrapv "$D/put.c" -o "$D/put" \
  || { echo "put.extc 编不过"; exit 1; }
out=$("$D/put"); n=$(printf '%s' "$out" | sed -n 's/.*n=\([0-9]*\).*/\1/p'); ns10=$(printf '%s' "$out" | sed -n 's/.*ns10=\([0-9]*\).*/\1/p')
printf "【构造成本】N=%s（存储值构造 + 派发，每轮一个地方）\n" "$n"
printf "  墙钟：%s ns/次（best of 1）\n" "$(awk -v x="$ns10" 'BEGIN{printf "%.1f", x/10}')"

if command -v valgrind >/dev/null 2>&1 && command -v callgrind_annotate >/dev/null 2>&1; then
    $CC -O2 -g -fwrapv "$D/put.c" -o "$D/putg" 2>/dev/null
    valgrind --tool=callgrind --callgrind-out-file="$D/cg.put" "$D/putg" >/dev/null 2>&1
    callgrind_annotate --auto=no "$D/cg.put" 2>/dev/null | awk -v n="$n" '
      /PROGRAM TOTALS/ { gsub(/,/,"",$1); total=$1; next }
      /^[ ]*[0-9,]+ \([ 0-9.]+%\)/ {
          gsub(/,/,"",$1); v=$1+0; line=$0
          if (line ~ /malloc|free|realloc|calloc/)      alloc += v
          else if (line ~ /memcpy|memmove/)             cpy   += v
          else if (line ~ /extc_/)                      extc  += v
          else                                          other += v
      }
      END {
          printf "  指令/次（总 %d）：\n", total/n
          printf "    分配 malloc/free/realloc  %6.1f  (%.0f%%)\n", alloc/n, 100*alloc/total
          printf "    extC 自身代码             %6.1f  (%.0f%%)\n", extc/n, 100*extc/total
          printf "    载荷拷贝 memcpy           %6.1f  (%.1f%%)\n", cpy/n, (total>0)?100*cpy/total:0
          printf "    其它（含派发/循环）       %6.1f  (%.0f%%)\n", other/n, 100*other/total
      }'
    printf "  读数：**拷贝几乎免费，成本在"每个值一次 malloc"** ⇒ 想便宜地"全拷贝"，该优化的是分配器（池内 bump），不是去掉拷贝。\n"
else
    printf "  （未装 valgrind：跳过指令级拆分）\n"
fi
