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
check_err tests/impl/trait_collide_inherent.extc 'has duplicate method `tag`'
check_err tests/impl/trait_collide_cross.extc    'has duplicate method `tag`'
check_err tests/impl/trait_self_outside.extc     'unknown type `Self`'
# 孤儿规则：`impl` 的模块既没声明 trait 也没声明类型（trait/类型都来自另一个模块）
check_err tests/impl/orphan_impl.extc            'is an orphan'

# 静态方法表：① 槽位顺序 = trait 声明顺序；② 表存在但**没有被调用**（第一期无动态分发）；
# ③ 生成物满足**文档化合同**：-std=c11 -fwrapv，且零告警（驱动自己带 -std=c11，但 --check-c 用 -w
#    静音了警告，所以这里取出生成物独立编译一次）。这三条挡的是一整类错误：本会话实测过
#    `typeof`（严格模式下须写 `__typeof__`）、表文本被插进按偏移重写的区域（未闭合注释）。
echo "== 静态方法表（声明顺序、无间接调用、生成物合同）=="
tmp=$(mktemp -d)
if "${EXTC:-./build/extc}" -w --no-line-map -o "$tmp/tt.c" tests/impl/trait_table.extc >/dev/null 2>&1; then
    # 阶段 3 起表的形状是"每 trait 一份统一签名 struct + thunk"：
    #   ① struct 定义里的**字段顺序**必须 = trait 声明顺序（zeta 在前，与字母序相反）；
    #   ② 实例的**初始化式顺序**必须与之相同（thunk 名同序）。
    sline=$(grep -m1 'struct extc_vt\$Both_t {' "$tmp/tt.c")
    iline=$(grep -m1 'extc_vt\$Both\$box = {' "$tmp/tt.c")
    order=0
    printf '%s' "$sline" | grep -q '\*zeta)(void \*).*\*alpha)(void \*)' \
        && printf '%s' "$iline" | grep -q 'extc_th\$Both\$box\$zeta, extc_th\$Both\$box\$alpha' \
        && order=1
    if [ "$order" = 1 ]; then
        echo "  ok   vtable_order    ->  字段顺序 = 声明顺序（zeta 在前，与字母序相反）"
    else
        echo "  FAIL vtable_order    ->  $sline | $iline"; fail=1
    fi
    # **不用 dyn ⇒ 零开销**：生成物里既没有表，也没有任何间接调用。
    # （原判据只数"经表的调用"，但夹具后来故意做了动态派发 ⇒ 口径改为"纯静态的翻译单元"。）
    if ! "$EXTC" -w --no-line-map -o "$tmp/st.c" tests/impl/trait_static_only.extc >/dev/null 2>&1; then
        echo "  FAIL vtable_no_call  ->  夹具 trait_static_only.extc 编译失败"; fail=1
    fi
    calls=$(grep -c 'extc_vt[^ ]*[[:space:]]*(' "$tmp/st.c" 2>/dev/null || true)
    tabs=$(grep -c 'struct extc_vt' "$tmp/st.c" 2>/dev/null || true)
    calls=${calls:-无}; tabs=${tabs:-无}
    if [ "$calls" = 0 ] && [ "$tabs" = 0 ]; then
        echo "  ok   vtable_no_call  ->  不用 dyn ⇒ 无表、无间接调用（零开销）"
    else
        echo "  FAIL vtable_no_call  ->  表 $tabs 处 · 调用 $calls 处（都应为 0）"; fail=1
    fi
    if gcc -std=c11 -fwrapv -Wall -Werror -fsyntax-only "$tmp/tt.c" 2>"$tmp/err"; then
        echo "  ok   vtable_contract ->  生成物在 -std=c11 -fwrapv -Wall -Werror 下零告警"
    else
        echo "  FAIL vtable_contract ->  合同编译失败"
        head -3 "$tmp/err" | sed 's/^/        /'; fail=1
    fi
else
    echo "  FAIL vtable          ->  trait_table.extc 编译失败"; fail=1
fi
rm -rf "$tmp"

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
