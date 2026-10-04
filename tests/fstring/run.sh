#!/usr/bin/env bash
# tests/fstring/run.sh —— `f"…"` 的判据：
#   ① 输出正确（多槽 / 转义 / 每槽求值一次 / 无文本段）
#   ② **生成物与手写 `<<` 链逐字节相同** ← 零运行时成本 + "它就是语法糖"的最硬证据
#   ③ 五条反例（空槽 · 未闭合 · 多余 `}` · 格式说明符是 v2 · 只在 `<<` 右侧合法）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0

# ① 输出
"$EXTC" -w --run tests/fstring/main.extc > "$tmp/out" 2>&1
printf 'n=42 who=世界 b=true f=1.5\nesc {} and 42\n3 2 1\nonly text\n42 ← 单个槽（没有文本段）\n手写对照：n=42 who=世界\n' > "$tmp/want"
if diff -q "$tmp/out" "$tmp/want" >/dev/null 2>&1; then
    echo "  ok   fstring-out       ->  多槽 · {{}} 转义 · 每槽求值一次 · 无文本段"
    pass=$((pass+1))
else
    echo "  FAIL fstring-out       ->  $(tr '\n' '|' < "$tmp/out")"; fail=$((fail+1))
fi

# ①b v2 的原语（宽度/填充/对齐/精度）
"$EXTC" -w --run tests/fstring/pad.extc > "$tmp/pad" 2>&1
printf 'abc...|\n   42|\n00042|\n-005|\n1.50|\n    3.14|\n  -2.0|\n中-|\n' > "$tmp/padwant"
if diff -q "$tmp/pad" "$tmp/padwant" >/dev/null 2>&1; then
    echo "  ok   pad-primitives    ->  左/右/居中 · 零填充（符号在前）· 精度 · 宽度+精度（v2 的展开目标）"
    pass=$((pass+1))
else
    echo "  FAIL pad-primitives    ->  $(tr '\n' '|' < "$tmp/pad")"; fail=$((fail+1))
fi

# ② 与手写链逐字节相同
"$EXTC" -w --no-line-map -o "$tmp/a.c" tests/fstring/main.extc >/dev/null 2>&1
"$EXTC" -w --no-line-map -o "$tmp/b.c" tests/fstring/plain.extc >/dev/null 2>&1
sed -i 's|tests/fstring/[a-z]*\.extc|SRC|g' "$tmp/a.c" "$tmp/b.c"
if diff -q "$tmp/a.c" "$tmp/b.c" >/dev/null 2>&1; then
    echo "  ok   fstring-sugar     ->  与手写 \`<<\` 链的生成物**逐字节相同**（零运行时成本）"
    pass=$((pass+1))
else
    echo "  FAIL fstring-sugar     ->  $(diff "$tmp/a.c" "$tmp/b.c" | head -3 | tr '\n' '|')"; fail=$((fail+1))
fi

# ③ 反例
neg() {   # $1=名字 $2=源码 $3=期望子串
    printf '%s\n' "$2" > "$tmp/n.extc"
    if out=$("$EXTC" -w --no-line-map -o "$tmp/n.c" "$tmp/n.extc" 2>&1); then
        echo "  FAIL $1 -> 编过了（应当被拒）"; fail=$((fail+1))
    elif echo "$out" | grep -q "$3"; then
        echo "  ok   $1 -> $(echo "$out" | head -1 | cut -c1-72)"; pass=$((pass+1))
    else
        echo "  FAIL $1 -> 报错不对：$(echo "$out" | head -1)"; fail=$((fail+1))
    fi
}
H='use std::io
fn main() -> i32 {'
neg fstring-empty-slot  "$H
    io::cout << f\"{}\"
    return 0
}" "cannot be empty"
neg fstring-unclosed    "$H
    io::cout << f\"{1\"
    return 0
}" "no matching"
neg fstring-stray-brace "$H
    io::cout << f\"a}b\"
    return 0
}" "no matching"
neg fstring-spec-v2     "$H
    let n: i64 = 1
    io::cout << f\"{n:>8}\"
    return 0
}" "not implemented yet"
neg fstring-not-shift   "$H
    let s: slice<u8> = f\"x\"
    return 0
}" "only valid as the right-hand side"
neg fstring-empty       "$H
    io::cout << f\"\"
    return 0
}" "nothing to splice"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
