#!/usr/bin/env bash
# tests/genmatrix/run.sh —— **泛型组合矩阵的常设验收**（PLAN #60/#61/#62 的验收表）
#
# 为什么有这个套件：泛型是这台编译器最薄的地方（§0.4 里 64 格有 22 格与泛型相关），
# 而盲区的成因是**覆盖太窄 + 绕开的写法被记成了"设计"** ⇒ 把「T 出现的位置 × 特性」
# 交叉成一张表，**每一格一个用例**，让"我没试过"变成"机器每次都试" ✓
#
# ⚠️⚠️ 铁律：判据必须**编译生成的 C** ⇒ 一律用 `--run`
#   （`#61`/`#62` 在 `extc f.extc -o out.c` 下**返回 0** ✗ —— 写这个套件时我自己先被骗过一次）
#
# 两半：
#   ① `tests/genmatrix/*.extc`      必须编过 + 跑对（`// expect:` 子串匹配）
#   ② `tests/canary-gaps/*.extc`    必须**仍然坏**，而且坏在**记着的那句话**上
#      （#61 / #62 已于 2026-09-24 修掉 ⇒ 它们从这一半搬到 ① 变成
#       t_variant_match.extc / t_two_param_slice.extc ✓）
#      ⇒ 哪天某一条**编过了**，这一节**当场 FAIL**，逼着去把它挪进 genmatrix/
#        并把账本（§0.4 的 ✅ 与计数）一起改 ✓
#        （规矩出自 tests/generics/run.sh：断言"不该存在的行为"时，行为一改好就必须删断言 ✓）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
TMO=${TMO:-60}
fail=0
n_ok=0

echo "== ① 矩阵格（T 出现的位置 × 特性）：必须编过 + 跑对 =="
for f in tests/genmatrix/*.extc; do
    t=$(basename "$f" .extc)
    if ! out=$(timeout "$TMO" "$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来（这就是缺口 ✗）"
        echo "$out" | sed 's/^/        /' | head -4
        fail=1
        continue
    fi
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    if [ -n "$want" ] && ! printf '%s' "$out" | grep -qF -- "$want"; then
        echo "  FAIL $t  ->  输出对不上：要「$want」，得到「$(printf '%s' "$out" | tr '\n' '|')」"
        fail=1
        continue
    fi
    echo "  ok   $t  ->  $(printf '%s' "$out" | tr '\n' '|')"
    n_ok=$((n_ok + 1))
done

echo "== ② 已知缺口（canary-gaps）：必须**仍然坏**，且坏在记着的那句话上 =="
# 文件 : 报错里必须出现的那句（跟着 §0.4 的原文走）
check_still_bad() {
    local f=$1 want=$2 num=$3 out rc
    out=$(timeout "$TMO" "$EXTC" --run "$f" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  FAIL $(basename "$f")  ->  **编过了**！说明缺口修好了 ⇒"
        echo "        去把它挪进 tests/genmatrix/、删掉 canary-gaps 里的副本，并把 §0.4 那一格划掉 ✗"
        fail=1
        return
    fi
    if ! printf '%s' "$out" | grep -qF -- "$want"; then
        echo "  FAIL $(basename "$f")  ->  还是坏的，但话变了（不再是「$want」）"
        printf '%s\n' "$out" | sed 's/^/        /' | head -4
        fail=1
        return
    fi
    echo "  ok   $(basename "$f")  ->  仍然坏在「$want」（#$num 未修 ✓）"
    n_ok=$((n_ok + 1))
}
check_still_bad tests/canary-gaps/generic_option_return.extc  'expects `option`'   60

echo "通过 $n_ok 项，失败 $fail 项（0 = 全过）"
[ "$fail" = 0 ]
