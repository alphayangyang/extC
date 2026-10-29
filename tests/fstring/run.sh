#!/usr/bin/env bash
# tests/fstring/run.sh —— `f"…"` 的判据：
#   ① 输出正确（多槽 / 转义 / 每槽求值一次 / 无文本段）
#   ② **生成物与手写 `<<` 链逐字节相同** ← 零运行时成本 + "它就是语法糖"的最硬证据
#   ③ 反例：空槽 · 未闭合 · 多余 `}` · 只在 `<<` 右侧合法 · 格式说明符的 10 条语法/类型错误
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0

# ① 输出
"$EXTC" -w --run tests/fstring/main.extc > "$tmp/out" 2>&1
printf 'n=42 who=世界 b=true f=1.5\nesc {} and 42\n1 2 3\nonly text\n42 ← 单个槽（没有文本段）\n手写对照：n=42 who=世界\n' > "$tmp/want"
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

# ①c 格式说明符（v2）：宽度 / 填充 / 对齐 / 精度 / 类型码
"$EXTC" -w --run tests/fstring/spec.extc > "$tmp/spec" 2>&1
printf '[    hi][hi    ][**hi**]\n[-005][    -5][    -5]\n[3.14][    3.14][3.141590]\n[   42][hi  ]\nmixed 42 and 0042 and {}\n3|\n' > "$tmp/specwant"
if diff -q "$tmp/spec" "$tmp/specwant" >/dev/null 2>&1; then
    echo "  ok   spec-output       ->  左/右/居中 · 填充 · 零填充（符号在前）· 精度 · 宽度+精度"
    pass=$((pass+1))
else
    echo "  FAIL spec-output       ->  $(tr '\n' '|' < "$tmp/spec")"; fail=$((fail+1))
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
neg spec-align-unknown "$H
    let n: i64 = 1
    io::cout << f\"{n:q8}\"
    return 0
}" "unexpected"
neg spec-empty         "$H
    let n: i64 = 1
    io::cout << f\"{n:}\"
    return 0
}" "an empty format specifier"
neg spec-zero-narrow   "$H
    let n: i64 = 1
    io::cout << f\"{n:0}\"
    return 0
}" "needs a width"
neg spec-dot-nodigits  "$H
    let v: f64 = 1.5
    io::cout << f\"{v:.}\"
    return 0
}" "expected digits"
neg spec-width-huge    "$H
    let n: i64 = 1
    io::cout << f\"{n:9999999}\"
    return 0
}" "too large"
neg spec-prec-int      "$H
    let n: i64 = 1
    io::cout << f\"{n:.2}\"
    return 0
}" "applies to floats"
neg spec-code-mismatch "$H
    let s: slice<u8> = \"x\"
    io::cout << f\"{s:d}\"
    return 0
}" "says the value is"
neg spec-bool          "$H
    let b: bool = true
    io::cout << f\"{b:>4}\"
    return 0
}" "cannot take a format specifier"
neg spec-not-stream    "$H
    let n: i64 = 5
    n << f\"{n:8}\"
    return 0
}" "cannot take a format specifier"
neg spec-no-value      "$H
    io::cout << f\"{:>8}\"
    return 0
}" "no value before its format specifier"
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
