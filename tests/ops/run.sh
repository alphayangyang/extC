#!/usr/bin/env bash
# tests/ops/run.sh —— **运算符重载的常设验收**（docs/DECISIONS.md 定案 82）
#
# 判据：
#   ① 具体类型：方法名**就是**运算符 ⇒ `<` `>` `<=` `>=` `+` `-` `*` `/` `%` 都能用 ✓
#   ② **判据有牙**：方法故意返回错的答案 ⇒ 打印出来的是**用户的方法**，不是内建语义 ✓
#   ③ 泛型体里：`T` 上的运算符**定义时放行、实例化时检查**（有就过、没有就报错点名实例）✓
#   ④ 内建一字不改（`i32 < i32` 还是内建；`!=` 仍由 `==` 派生）✓
#   ⑤ 生成的 C 里运算符名被 mangle 成合法标识符（`vec2_lt` 而不是 `vec2_<`）✓
#   ⑥ 九条反例：缺运算符（比较/算术）· 不派生 `>` · 实例缺运算符 · `%` 实例化成 f64
#      或序枚举/bool（**都不许漏到 gcc** —— 生成物里的报错用户看不见自己的源码）·
#      签名写错（比较返回 void / 算术返回别的类型）✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
mkdir -p build

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/ops/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/ops/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}

echo "== 正例：具体类型（比较 + 算术）· 有牙 · 泛型体 · 内建不改 =="
run_case fnptr
run_case long_line
run_case slice_place
run_case conv_float
run_case concrete
run_case teeth
run_case generic
run_case builtin

echo "== ① 流运算符 << 与 >>：它们是两个 < / > 记号，却在可重载集合里 =="
run_case stream

echo "== ①b 有状态的流：<< / >> 可返回 mut ref T（借来的同一个对象，链式靠它）=="
run_case stream-ref

echo "== ② 一个运算符名可以重名，靠**右操作数类型**区分 =="
run_case hetero
# 名字面：两份重载在生成的 C 里必须是**两个函数**（后缀编码右操作数类型），否则重名就白放了 ✗
if "$EXTC" tests/ops/hetero.extc -o build/ops-hetero.c >/dev/null 2>&1 &&
   "$EXTC" tests/ops/stream.extc -o build/ops-stream.c >/dev/null 2>&1; then
    # 后缀规则（`opOverloadSuffix`）：运算符名在这个类型上**只有一个**实现 ⇒ 不加后缀
    # （`vec2_lt`，老名字不动 ✓）；**两个以上**才加**右操作数类型**后缀把它们分开 ✓
    # 所以这里两样都要钉：单一重载不带后缀 + 重名必须带后缀且两个名字不同 ✓
    bad=0
    for n in vec2_mul_f64 vec2_mul_i64 vec2_eq_vec2 vec2_eq_i64; do
        grep -qE "\b$n\b" build/ops-hetero.c || { echo "  FAIL 生成的 C 里没有 $n ⇒ 同名重载没分开"; fail=1; bad=1; }
    done
    grep -qE '\bvec2_lt\b' build/ops-hetero.c || { echo "  FAIL 生成的 C 里没有 vec2_lt（单一重载不该带后缀）"; fail=1; bad=1; }
    # 流运算符：同一个 `<<` 的两份重载 ⇒ `out_shl_i64` 与 `out_shl_slice_u8`（带右操作数类型后缀 ✓）
    grep -qE '\bout_shl_i64\b'      build/ops-stream.c || { echo "  FAIL 生成的 C 里没有 out_shl_i64"; fail=1; bad=1; }
    grep -qE '\bout_shl_slice_u8\b' build/ops-stream.c || { echo "  FAIL 生成的 C 里没有 out_shl_slice_u8 ⇒ 两份 \`<<\` 撞在同一个名字上了"; fail=1; bad=1; }
    if grep -qE '[A-Za-z0-9_]+_[<>+*/%]' build/ops-hetero.c build/ops-stream.c; then
        echo "  FAIL 生成的 C 里有没用 mangle 的运算符名（\`<<\`/\`>>\` 也得进 mangle 表）"; fail=1; bad=1
    fi
    [ "$bad" = 0 ] && echo "  ok   重名分派  ->  5 个运算符方法各自一个 C 函数 · 两份 << 是两个符号 ✓"
else
    echo "  FAIL 生成 C 失败（hetero / stream）"; fail=1
fi

echo "== 生成的 C：运算符名必须 mangle 成合法标识符 =="
if "$EXTC" tests/ops/concrete.extc -o build/ops-concrete.c >/dev/null 2>&1; then
    bad=0
    for n in lt gt le ge add sub mul div; do
        grep -qE "\bvec2_$n\b" build/ops-concrete.c || { echo "  FAIL 生成的 C 里没有 vec2_$n ⇒ mangle 表漏了这一项"; fail=1; bad=1; }
    done
    grep -qE '\bpt_rem\b' build/ops-concrete.c || { echo "  FAIL 生成的 C 里没有 pt_rem"; fail=1; bad=1; }
    # 反面：一个运算符字符都不许漏进**名字**里（`vec2_<` 这种）。
    # 模式是"标识符 + 下划线 + 运算符字符"：生成物里正常的 `a < b` 不会被误判，
    # 而 `#include <stdint.h>` 这类也不沾边 ✓
    if grep -qE '[A-Za-z0-9_]+_[<>+*/%]' build/ops-concrete.c; then
        echo "  FAIL 生成的 C 里有没用 mangle 的运算符名"; fail=1; bad=1
    fi
    [ "$bad" = 0 ] && echo "  ok   mangle  ->  vec2_{lt,gt,le,ge,add,sub,mul,div} · pt_rem（一个运算符字符都没漏进标识符 ✓）"
else
    echo "  FAIL 生成 C 失败"; fail=1
fi

echo "== 反例（都必须编译期挡住）=="
check_err tests/ops/errors/order_missing.extc          'does not define `<`'
check_err tests/ops/errors/arith_missing.extc          'does not define `+`'
check_err tests/ops/errors/derive_not_done.extc        'does not define `>`'
check_err tests/ops/errors/instance_missing.extc       'to define `<`'
check_err tests/ops/errors/modulo_float_instance.extc  'cannot apply `%` to `f64`'
check_err tests/ops/errors/sig_returns_void.extc       'must return `bool`'
check_err tests/ops/errors/sig_arith_wrong_ret.extc    'must return `bad`'
# 下面两条钉着一条修掉的缺陷：泛型实例的判据曾经比具体类型**松**（枚举/bool 都能序）✗
check_err tests/ops/errors/instance_enum_order.extc    'cannot apply `<` to `order`'
check_err tests/ops/errors/instance_bool_order.extc    'cannot apply `<` to `bool`'
# 流运算符的**引用结果**边界：它是借来的，不是值 ⇒ 存起来 / 传出去都必须挡住 ✓
check_err tests/ops/errors/ref_value.extc               'can only be chained'
check_err tests/ops/errors/ref_arg.extc                 'can only be chained'
# `fn(A) -> R`（裸函数指针，C-ABI.md 第 9 节第 1 步）的四条边界：零值 · 骗人的签名 ·
# 泛型裸名 · 结构体字段漏给。每一条都必须是**检查器/代码生成器**报的，不许漏到 gcc ✓
check_err tests/ops/errors/fn_no_init.extc               'it contains a `fn`'
check_err tests/ops/errors/fn_slice.extc                 'cannot cross the C boundary'
check_err tests/ops/errors/fn_generic.extc               'is generic, so its bare name is not a value'
check_err tests/ops/errors/fn_field_missing.extc         'must be given explicitly'

echo "== 那三条“实例化时才检查”的诊断不许来自 gcc（生成物里的报错用户看不见源码）=="
out=$("$EXTC" tests/ops/errors/instance_missing.extc -o /dev/null 2>&1)
if echo "$out" | grep -q 'C compiler failed'; then
    echo "  FAIL instance_missing  ->  漏到 gcc 了 ✗"; fail=1
else
    echo "  ok   instance_missing  ->  检查器报的（点名实例：$(echo "$out" | grep -m1 -o '`[a-z_]*` needs' | head -1)）"
fi
out=$("$EXTC" tests/ops/errors/modulo_float_instance.extc -o /dev/null 2>&1)
if echo "$out" | grep -q 'C compiler failed'; then
    echo "  FAIL modulo_float  ->  漏到 gcc 了 ✗（这条正是修之前的行为）"; fail=1
else
    echo "  ok   modulo_float  ->  检查器报的（不是 gcc 的 invalid operands ✓）"
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
