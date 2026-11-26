#!/usr/bin/env bash
# tests/constgen/run.sh —— `const` 参数（编译期长度）的行为判据
#
# 判据：
#   ① `dot`：同一函数按长度实例化两次，`N` 在体内当整数用（输出 14 / 41 / 3 / 2）
#   ② `mismatch`：长度不符在调用点报 `expects [3]f64, found [2]f64`
#   ③ `mdim`：多维长度 `[N, N]f64` 与 `[N][N]f64` 是同一种类型
#   ④ `const_global`：模块级 `const N: i64 = 15` 当 `[N]T` 的长度（结构体字段、二维、负数值）
#   ⑤ `const_no_init` / `const_not_literal`：const 不写值被拒；初值不是整数字面量时不能当长度
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-14s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-14s %s\n' "$1" "$2"; }

out=$("$EXTC" -w --no-line-map --run tests/constgen/dot.extc 2>"$tmp/e" | tr '\n' ' ')
if [ "$out" = "14 41 3 2 " ]; then ok dot "→ $out"; else bad dot "期望 [14 41 3 2] 实得 [$out] $(head -2 "$tmp/e")"; fi

if "$EXTC" -w --no-line-map -o "$tmp/m.c" tests/constgen/mismatch.extc 2>"$tmp/e"; then
    bad mismatch "应当被拒，却编过了"
elif grep -q 'argument expects `\[3\]f64`, found `\[2\]f64`' "$tmp/e"; then
    ok mismatch "长度不符在调用点报出来"
else
    bad mismatch "报文不对：$(head -2 "$tmp/e")"
fi

out=$("$EXTC" -w --no-line-map --run tests/constgen/mdim.extc 2>"$tmp/e" | tr '\n' ' ')
if [ "$out" = "5 15 6 " ]; then ok mdim "→ $out"; else bad mdim "期望 [5 15 6] 实得 [$out] $(head -2 "$tmp/e")"; fi

out=$("$EXTC" -w --no-line-map --run tests/constgen/const_global.extc 2>"$tmp/e" | tr '\n' ' ')
if [ "$out" = "1 2 -7 15x3 3.14 3 105 " ]; then
    ok const_global "模块级 const 当长度、当值、进结构体字段"
else
    bad const_global "期望 [1 2 -7 15x3 3.14 3 105] 实得 [$out] $(head -2 "$tmp/e")"
fi

if "$EXTC" -w --no-line-map -o "$tmp/ni.c" tests/constgen/const_no_init.extc 2>"$tmp/e"; then
    bad const_no_init "应当被拒（const 没有初值），却编过了"
elif grep -q '`const` needs an initializer' "$tmp/e"; then
    ok const_no_init "const 不写值被拒"
else
    bad const_no_init "报文不对：$(head -2 "$tmp/e")"
fi

if "$EXTC" -w --no-line-map -o "$tmp/nl.c" tests/constgen/const_not_literal.extc 2>"$tmp/e"; then
    bad const_not_literal "应当被拒（初值不是整数字面量），却编过了"
elif grep -q 'not an integer literal' "$tmp/e"; then
    ok const_not_literal "非字面量初值的 const 不能当长度"
else
    bad const_not_literal "报文不对：$(head -2 "$tmp/e")"
fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
