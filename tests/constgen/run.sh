#!/usr/bin/env bash
# tests/constgen/run.sh —— `const` 参数（编译期长度）的行为判据
#
# 判据：
#   ① `dot`：同一函数按长度实例化两次，`N` 在体内当整数用（输出 14 / 41 / 3 / 2）
#   ② `mismatch`：长度不符在调用点报 `expects [3]f64, found [2]f64`
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

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
