#!/usr/bin/env bash
# tests/impl/run.sh —— **`impl` 块（方法挂载点）的常设验收**
#
# 这一项不是 trait：没有要求集合、没有独立实现可匹配，方法进的是类型**唯一**的方法集，
# 派发仍然静态、逐实例检查照旧。所以判据的重点是两件事：
#   ① 挂载点确实通了（含**编译器内建标量** ⇒ `hashMap<i64, V>` 直接可用）
#   ② coherence 不许破（同名方法必须报错，而不是后者覆盖前者）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

check_pos() {
    local name=$1 f=$2 out want ok=1 p
    if ! out=$("$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $name  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $name  ->  输出对不上（期望「$want」）"; fail=1; fi
}

echo "== 正例：struct 的 impl + **内建标量**的 impl + hashMap<i64, V> 直接可用 =="
check_pos impl_basic tests/impl/basic.extc

# 试水：跨模块 `impl`（impl 块在 stl/stringio.extc，类型 string 在 stl/string.extc）
check_pos impl_stream_string tests/impl/stream_string.extc

echo "== 反例（都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err tests/impl/errors/dup_body_and_impl.extc 'already has a method named `sum`'
check_err tests/impl/errors/dup_two_impls.extc     'already has a method named `sum`'
# 同一条错误还要**指名先前那一处的位置**（'which of the two is the duplicate' 是读者的第一个问题）
check_err tests/impl/errors/dup_body_and_impl.extc 'the first declaration is at line 7'
check_err tests/impl/errors/unknown_type.extc      'unknown type `nope`'
check_err tests/impl/errors/on_enum.extc           'cannot own methods'
check_err tests/impl/errors/with_field.extc        'expected `fn` in the `impl` block'
check_err tests/impl/errors/generic_target.extc    '`impl` on a generic type is not supported yet'
check_err tests/impl/errors/annotation.extc        'no annotation applies to an `impl` block'

exit $fail
