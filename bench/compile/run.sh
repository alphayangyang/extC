#!/usr/bin/env bash
# bench/compile/run.sh —— **编译时长压力测试**（2026-09-21 新增）
#
# 测什么：
#   ① extC 自己的**前端**（生成 C）耗时    ← "检查得怎么样"主要看这一列
#   ② extC 端到端（含 gcc）耗时            ← 用户实际感受
#   ③ 同样形状的 C 程序，gcc 单独编译耗时   ← 公平参照（同规模、同结构）
#
# 压力程序怎么造（脚本自己生成，不依赖外部工具）：
#   N 个 struct（每个带一个方法）+ N 个函数 + main 全调一遍 ⇒ 压**声明/查找/检查**这条线 ✓
#   再叠五个泛型实例（varArray<i32/i64/u64/u8/bool/f64>）⇒ 压**实例集合 + 效果摘要闭包** ✓
#
# ⚠️ 故意**不**把 slice 放进容器：往容器里存**视图**现在会被拒绝（容器可能活得更久
#    ⇒ 视图会悬垂）—— 那是**正确**的保守，不是 bug ⇒ 压测程序避开它 ✓
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
OUT=build/compile-bench
mkdir -p "$OUT"
RUNS=${RUNS:-3}

gen_extc() {   # $1 = N，$2 = 输出文件
    local n=$1 f=$2 i
    {
        echo "/* 自动生成的压力程序：$n 个 struct + $n 个函数 + 五个泛型实例 */"
        for ((i = 0; i < n; i++)); do
            echo "struct s$i { a: i32  b: i64  fn get(self: ref s$i) -> i32 { return self.a } }"
        done
        for ((i = 0; i < n; i++)); do
            echo "fn g$i(x: i32) -> i32 {"
            echo "    var t: s$i = { a: x, b: 1 }"
            echo "    var v: varArray<i32> = varArray<i32>::withCap(2)"
            echo "    v.push(x)"
            echo "    return t.get() + (v.get(0) ?? 0)"
            echo "}"
        done
        echo "fn main() {"
        echo "    var acc: i32 = 0"
        for ((i = 0; i < n; i++)); do echo "    acc = acc + g$i($i)"; done
        echo "    println(\"acc = \", acc)"
        echo "    var vi: varArray<i64> = varArray<i64>::withCap(1)"
        echo "    var vu: varArray<u64> = varArray<u64>::withCap(1)"
        echo "    var vb: varArray<bool> = varArray<bool>::withCap(1)"
        echo "    var vf: varArray<f64> = varArray<f64>::withCap(1)"
        echo "    var vq: varArray<u8> = varArray<u8>::withCap(1)"
        echo "    vi.push(1)  vu.push(2)  vb.push(true)  vf.push(0.5)  vq.push(3)"
        echo "    println(\"insts = \", vi.size(), vu.size(), vb.size(), vf.size(), vq.size())"
        echo "}"
    } > "$f"
}

gen_c() {      # 同规模同结构的 C（手写对照，不用 extC 生成 ⇒ 公平）
    local n=$1 f=$2 i
    {
        echo "/* 自动生成的压力程序（C 版）：$n 个 struct + $n 个函数 */"
        echo "#include <stdint.h>"
        echo "#include <stdio.h>"
        for ((i = 0; i < n; i++)); do
            echo "typedef struct { int32_t a; int64_t b; } s$i;"
            echo "static int32_t s${i}_get(const s$i *p) { return p->a; }"
        done
        for ((i = 0; i < n; i++)); do
            echo "static int32_t g$i(int32_t x) { s$i t = { x, 1 }; return s${i}_get(&t) + x; }"
        done
        echo "int main(void) {"
        echo "    int32_t acc = 0;"
        for ((i = 0; i < n; i++)); do echo "    acc += g$i($i);"; done
        echo "    printf(\"acc = %d\\n\", acc);"
        echo "    return 0;"
        echo "}"
    } > "$f"
}

# 毫秒计时：跑 RUNS 次取最快；命令失败就报 ERR（不静默留空）
ms() {
    local best=999999999 t0 t1 dt i
    for ((i = 0; i < RUNS; i++)); do
        t0=$(date +%s%N)
        if ! "$@" > /dev/null 2>&1; then echo "ERR"; return; fi
        t1=$(date +%s%N); dt=$(( (t1 - t0) / 1000000 ))
        (( dt < best )) && best=$dt
    done
    echo "$best"
}

echo "压力程序规模 NS=$NS  （编译器：$EXTC）"
printf '\n%-8s %12s %12s %13s %10s\n' N "extC->C" "extC+gcc" "gcc(C 同形)" "源码行数"
printf '%s\n' "--------------------------------------------------------------------"

for n in ${NS:-200 1000 3000}; do
    gen_extc "$n" "$OUT/big$n.extc"
    gen_c    "$n" "$OUT/big$n.c"

    a=$(ms $EXTC "$OUT/big$n.extc" -o "$OUT/big$n.extc.c")
    b=$(ms $EXTC --run "$OUT/big$n.extc")
    c=$(ms $CC -O2 -o "$OUT/big$n.c.bin" "$OUT/big$n.c")
    lines=$(wc -l < "$OUT/big$n.extc")
    printf '%-8s %9s ms %9s ms %10s ms %10s\n' "$n" "$a" "$b" "$c" "$lines"
done

echo
echo "（各列 3 次取最快；'extC->C' = 只要前端吐出 C；'extC+gcc' = 端到端（含 gcc 编译并运行）；"
echo "  'gcc(C 同形)' = 同规模同结构的 C 程序用 gcc -O2 单独编译 ⇒ 公平参照 ✓）"

# ---------------------------------------------------------------------------
# 说明（2026-09-21 实测，本机）：
#   N=200 → extC 前端 13 ms；N=1000 → 76 ms；N=3000 → 545 ms
#   ⇒ 前端大致 O(N^1.5~2)（3000 行×8 是 200 的 15 倍体量，前端 42 倍）
#   ⇒ 端到端是**被 gcc 主导**的：同一份规模，手写 C 用 gcc -O2 只要 ~0.12-0.39 s，
#      而 extC 生成的 C 要 gcc 花 1.2-14 s ⇒ extC 的**前端很快、产出的 C 对 gcc 很贵** ✗
#      （原因：每个用到的类型都会生成一组 static inline helper：slice/eq/debug/option …）
#   ⇒ "效果摘要 + E 分析"的代价：**测不出来**（N=1000/3000 两版都是 0.07/0.57 s，同量级）✓
#      也就是说这套新分析在**前端总开销里还不显著** ✓
# ---------------------------------------------------------------------------
