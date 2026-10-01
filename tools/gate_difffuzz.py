#!/usr/bin/env python3
"""Gate 3 · 常驻差分 fuzz：同一段语义，extC 与等价 C 必须给出同样的结论。

这是审计 §7 闸门 ③。为什么用"自检程序 + 退出码"而不是比 stdout：
extC 的 `io::cout` 与 C 的 `printf` 在浮点格式上本来就可能不同，比格式会把真信号淹掉。
每个用例都是**断言式**程序：全部成立返回 0，否则返回非 0；两边都返回 0 才算一致。

用例是**固定表**（不是随机数），所以结果可复现、失败可定位；随机化只用在批量生成上
（`--fuzz N`，用固定种子，仍打印种子）。

用法：
    python3 tools/gate_difffuzz.py [--update-allowlist] [--seed N] [--fuzz N]

判据（tools/gate-diff-known-bad.txt 是基线）：
    · 新增 mismatch ⇒ 红；基线里已不再 mismatch 的条目 ⇒ 红（棘轮）。
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

ALLOW = os.path.join(G.ROOT, "tools", "gate-diff-known-bad.txt")

# 每个用例：名字、extC 源、C 源。两边的 "结论" 必须一致（都 0 = 都认为成立）。
CASES = [
    # --- 宽类型算术（回绕） ---
    ("i32_wrap", """
fn main() -> i32 {
    var a: i32 = 2147483647
    a = a + 1
    if a != -2147483647 - 1 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t a = 2147483647; a = a + 1;
    return a != (int32_t)(-2147483647 - 1); }"""),
    ("i64_wrap", """
fn main() -> i32 {
    var a: i64 = 9223372036854775807
    a = a + 1
    if a != 0 - 9223372036854775807 - 1 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int64_t a = INT64_MAX; a = a + 1;
    return a != INT64_MIN; }"""),
    ("u32_wrap", """
fn main() -> i32 {
    var a: u32 = 0
    a = a - 1
    if a != 4294967295 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { uint32_t a = 0; a = a - 1; return a != UINT32_MAX; }"""),
    # --- 除法与取余（负数、非零除数） ---
    ("i32_divmod_neg", """
fn main() -> i32 {
    var a: i32 = 0 - 7
    var b: i32 = 2
    if a / b != 0 - 3 { return 1 }
    if a % b != 0 - 1 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t a = -7, b = 2;
    return (a / b != -3) || (a % b != -1); }"""),
    ("i64_div_small", """
fn main() -> i32 {
    var a: i64 = 1000000007
    var b: i64 = 3
    if a / b != 333333335 { return 1 }
    if a % b != 2 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int64_t a = 1000000007, b = 3;
    return (a / b != 333333335) || (a % b != 2); }"""),
    # --- 移位（合法范围） ---
    ("shift_ok", """
fn main() -> i32 {
    var a: i32 = 3
    var b: i32 = 5
    if a << b != 96 { return 1 }
    if a >> i32(1) != 1 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t a = 3, b = 5;
    return (a << b != 96) || ((a >> 1) != 1); }"""),
    # --- 比较与混合宽度 ---
    ("widen_cmp", """
fn main() -> i32 {
    var a: i32 = 0 - 1
    var b: i64 = 0
    if i64(a) >= b { return 1 }
    var c: i32 = 2147483647
    if i64(c) <= i64(2147483647) == false { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t a = -1; int64_t b = 0;
    if ((int64_t)a >= b) return 1;
    int32_t c = INT32_MAX;
    if (!((int64_t)c <= (int64_t)INT32_MAX)) return 1;
    return 0; }"""),
    # --- 转换（都在范围内） ---
    ("conv_in_range", """
fn main() -> i32 {
    var a: i64 = 300
    var b: u8 = u8(a)
    if i64(b) != 300 { return 1 }
    var c: i32 = 0 - 5
    if i64(c) != 0 - 5 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int64_t a = 300; uint8_t b = (uint8_t)a;
    if ((int64_t)b != 300) return 1;
    int32_t c = -5;
    return (int64_t)c != -5; }"""),
    # --- 浮点：整数值字面量（审计 P0-12） ---
    ("f_literal_div", """
fn main() -> i32 {
    if 1.0 / 2.0 != 0.5 { return 1 }
    if 7.0 / 2.0 != 3.5 { return 1 }
    if 3.0 * 2.0 != 6.0 { return 1 }
    return 0
}""", """
int main(void) {
    if (1.0 / 2.0 != 0.5) return 1;
    if (7.0 / 2.0 != 3.5) return 1;
    if (3.0 * 2.0 != 6.0) return 1;
    return 0; }"""),
    ("f_neg_zero", """
fn main() -> i32 {
    var z: f64 = -0.0
    var one: f64 = 1.0
    if one / z > 0.0 { return 1 }
    return 0
}""", """
int main(void) { double z = -0.0, one = 1.0; return !(one / z < 0.0); }"""),
    ("f_literal_overflow", """
fn main() -> i32 {
    if 1000000000.0 * 1000000000.0 != 1.0e18 { return 1 }
    return 0
}""", """
int main(void) { return 1000000000.0 * 1000000000.0 != 1.0e18; }"""),
    ("f_big_literal", """
fn main() -> i32 {
    var x: f64 = 1000000000.0
    if x * 1000000000.0 != 1.0e18 { return 1 }
    return 0
}""", """
int main(void) { double x = 1000000000.0; return x * 1000000000.0 != 1.0e18; }"""),
    # --- 窄类型：按声明宽度回绕（审计 P0-18；修好前会在基线里） ---
    ("i8_wrap", """
fn main() -> i32 {
    var a: i8 = 100
    var x: i8 = a + a
    if x != 0 - 56 { return 1 }
    if (a + a) == x == false { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int8_t a = 100; int8_t x = (int8_t)(a + a);
    if (x != -56) return 1;
    if ((int8_t)(a + a) != x) return 1;
    return 0; }"""),
    ("u16_wrap", """
fn main() -> i32 {
    var u: u16 = 60000
    var y: u16 = u + u
    if y != 54464 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { uint16_t u = 60000; uint16_t y = (uint16_t)(u + u);
    return y != 54464; }"""),
    # --- 窄类型的其它形态：u16 移位、一元负、窄除法（审计 P0-18 的同族） ---
    ("narrow_ops", """
fn main() -> i32 {
    var a: u16 = u16(1) << u16(15)
    if a != 32768 { return 1 }
    var b: i16 = i16(30000) + i16(30000)
    if b != 0 - 5536 { return 1 }
    var c: i8 = i8(-128)
    if -c != 0 - 128 { return 1 }
    var d: u8 = u8(200) + u8(100)
    if d != 44 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) {
    uint16_t a = (uint16_t)((uint16_t)1 << (uint16_t)15);
    if (a != 32768) return 1;
    int16_t b = (int16_t)((int16_t)30000 + (int16_t)30000);
    if (b != -5536) return 1;
    int8_t c = -128;
    if ((int8_t)(-c) != -128) return 1;
    uint8_t d = (uint8_t)((uint8_t)200 + (uint8_t)100);
    if (d != 44) return 1;
    return 0; }"""),
    # --- 空切片/边界（合法侧） ---
    ("slice_len", """
fn main() -> i32 {
    var a: [4]i32 = [1, 2, 3, 4]
    let s = a[1..3]
    if s.len != 2 { return 1 }
    if s[0] != 2 { return 1 }
    if s[1] != 3 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t a[4] = {1,2,3,4};
    int32_t *d = a + 1; int64_t n = 2;
    if (n != 2) return 1;
    if (d[0] != 2) return 1;
    if (d[1] != 3) return 1;
    return 0; }"""),
    # --- 求值次数（审计 P0-7 的同族，但用 if 而不是 while） ---
    # --- 循环条件里的临时量不得提升到循环外（审计 P0-7） ---
    ("while_eq_temp", """
var g: i32 = 0
fn mk() -> [2]i32 { g = g + 1  return [g, 0] }
fn main() -> i32 {
    var a: [2]i32 = [1, 0]
    var n: i32 = 0
    while a == mk() { n = n + 1  if n > 100 { break } }
    if n != 1 { return 1 }
    return 0
}""", """
#include <stdint.h>
#include <string.h>
static int32_t g = 0;
typedef struct { int32_t v[2]; } arr2;
static arr2 mk(void) { g = g + 1; arr2 r; r.v[0] = g; r.v[1] = 0; return r; }
static int eq(arr2 x, arr2 y) { return memcmp(x.v, y.v, sizeof x.v) == 0; }
int main(void) {
    arr2 a; a.v[0] = 1; a.v[1] = 0;
    int32_t n = 0;
    while (eq(a, mk())) { n = n + 1; if (n > 100) break; }
    return n != 1; }"""),
    # --- 运行期：for + continue 必须仍然推进（审计 P0-15，修好前在基线里） ---
    ("for_continue", """
fn main() -> i32 {
    var i: i32 = 0
    var sum: i64 = 0
    for i in 0..5 { if i == 2 { continue }  sum = sum + i64(i) }
    if sum != 8 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int64_t sum = 0;
    for (int32_t i = 0; i < 5; i++) { if (i == 2) continue; sum += (int64_t)i; }
    return sum != 8; }"""),
    # 同族：C 风格与 for-in 形式的 `continue`（脱糖路径不同，各自要有探针）
    ("for_continue_cstyle", """
fn main() -> i32 {
    var sum: i64 = 0
    for (var i: i32 = 0; i < 5; i = i + 1) { if i == 2 { continue }  sum = sum + i64(i) }
    if sum != 8 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int64_t sum = 0;
    for (int32_t i = 0; i < 5; i = i + 1) { if (i == 2) continue; sum += (int64_t)i; }
    return sum != 8; }"""),
    ("for_continue_foreach", """
fn main() -> i32 {
    var xs: [5]i32 = [0, 1, 2, 3, 4]
    var sum: i64 = 0
    for x in xs { if x == 2 { continue }  sum = sum + i64(x) }
    if sum != 8 { return 1 }
    return 0
}""", """
#include <stdint.h>
int main(void) { int32_t xs[5] = {0, 1, 2, 3, 4}; int64_t sum = 0;
    for (int64_t i = 0; i < 5; i++) { int32_t x = xs[i]; if (x == 2) continue; sum += (int64_t)x; }
    return sum != 8; }"""),
    ("eval_once", """
var g: i32 = 0
fn bump() -> i32 { g = g + 1  return g }
fn main() -> i32 {
    var x: i32 = bump() + bump()
    if x != 3 { return 1 }
    if g != 2 { return 1 }
    return 0
}""", """
#include <stdint.h>
static int32_t g = 0;
static int32_t bump(void) { g = g + 1; return g; }
int main(void) { int32_t x = bump() + bump();
    if (x != 3) return 1;
    return g != 2; }"""),
]


def one_case(case):
    name, extc_src, c_src = case
    d = os.path.join(G.GATEDIR, "diff", name)
    os.makedirs(d, exist_ok=True)
    e_path, c_path = os.path.join(d, "t.extc"), os.path.join(d, "t.c")
    with open(e_path, "w", encoding="utf-8") as f:
        f.write(extc_src)
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(c_src)
    e_c = os.path.join(d, "t.extc.c")
    ok, diag = G.gen_c(e_path, e_c, timeout=60)
    if not ok:
        return (name, "extC 拒绝了: " + " ".join(diag.split())[:160])
    rc, log = G.compile_c(e_c, cc="gcc", sanitize=True)
    if rc != 0:
        return (name, "extC 产物编不过: " + " ".join(log.split())[-160:])
    # The C twin is compiled with the same flags the extC side got (sanitizers on).
    rc_c, log_c = G.run(["gcc", "-std=c11", "-O1", "-g", "-fwrapv", "-fsanitize=address,undefined",
                         "-fno-sanitize-recover=all", c_path, "-o", os.path.join(d, "cc")], timeout=120)
    if rc_c != 0:
        return (name, "C 对照编不过: " + " ".join(log_c.split())[-160:])
    r1, o1 = G.run([e_c[:-2]], timeout=10)
    r2, o2 = G.run([os.path.join(d, "cc")], timeout=10)
    if r1 != r2:
        return (name, f"结论不一致: extC rc={r1} vs C rc={r2} | extC: {' '.join(o1.split())[:80]}")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    G.ensure_gatedir()
    os.makedirs(os.path.join(G.GATEDIR, "diff"), exist_ok=True)
    fails = {}
    with G.ProcessPoolExecutor(max_workers=a.jobs or min(8, G.jobs())) as ex:
        for r in ex.map(one_case, CASES, chunksize=1):
            if r:
                fails[r[0]] = r[1]
    print(f"[diff] 跑了 {len(CASES)} 个差分用例（extC vs C，两边都必须成立）")
    if a.update_allowlist:
        G.write_allow(ALLOW, fails)
        return 0
    return G.report("diff", fails, G.load_allow(ALLOW), detail_lines=3,
                    eligible={c[0] for c in CASES})


if __name__ == "__main__":
    sys.exit(main())
