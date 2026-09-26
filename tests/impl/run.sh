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
check_pos impl_file_string tests/impl/file_string.extc

# `@private` 成员（方法级 + 字段级）：同模块可用（正例）
check_pos impl_privacy_ok tests/impl/privacy_ok.extc


# stdin 驱动的正例：`cin >> string`（链式、去换行、替换语义）
echo "== 正例：跨模块挂的运算符 `cin >> string`（stdin 两行）=="
if out=$(printf 'hello world\nsecond line\n' | "$EXTC" --run tests/impl/cin_string.extc 2>&1); then
    ok=1
    for p in "a=[hello world]" "la=11" "b=[second line]" "lb=11"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   impl_cin_string  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL impl_cin_string  ->  输出对不上"; fail=1; fi
else echo "  FAIL impl_cin_string  ->  跑不起来"; fail=1; fi

echo "== 反例（都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err tests/impl/errors/dup_body_and_impl.extc 'has duplicate method `sum`'
check_err tests/impl/errors/dup_two_impls.extc     'has duplicate method `sum`'
# `@private` 成员：跨模块读字段 / 调方法 / 字面量写入，三条路径全挡
check_err tests/impl/privacy_field.extc   'field `seen` of `privmod::counter` is private to module `privmod`'
check_err tests/impl/privacy_method.extc  '`zero` is private to module `privmod`'
check_err tests/impl/privacy_literal.extc 'field `seen` of `privmod::counter` is private to module `privmod`'

# trait 第一期：声明 + `impl … for` + 直接调用（静态分发）；三条反例（未知 trait / 实现不齐 / 重复实现）
check_pos impl_trait_ok tests/impl/trait_ok.extc
check_err tests/impl/trait_unknown.extc '`impl` on unknown trait `Nope`'
check_err tests/impl/trait_missing.extc 'is missing `other`'
check_err tests/impl/trait_dup.extc     'is already implemented for `box`'
check_err tests/impl/trait_sig_arity.extc 'does not match the signature the trait declares'
check_err tests/impl/trait_sig_type.extc  'but the trait declares `i64`'

# 同一条错误还要**指名先前那一处的位置**（'which of the two is the duplicate' 是读者的第一个问题）
check_err tests/impl/errors/dup_body_and_impl.extc 'the first declaration is at line 7'
check_err tests/impl/errors/unknown_type.extc      'unknown type `nope`'
# impl 方法只有 import 了那个模块才存在 ⇒ 报错必须说"该 import 谁"
check_err tests/impl/errors/method_needs_import.extc 'declared by module `stl::hashMap` -- add `use stl::hashMap`'
check_err tests/impl/errors/on_enum.extc           'cannot own methods'
check_err tests/impl/errors/with_field.extc        'expected `fn` in the `impl` block'
check_err tests/impl/errors/generic_target.extc    '`impl` on a generic type is not supported yet'
check_err tests/impl/errors/annotation.extc        'no annotation applies to an `impl` block'

exit $fail
