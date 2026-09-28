#!/usr/bin/env bash
# `ext f(x)` 第一步判据：**只做解析**（CONCURRENCY.md「`ext` 与调度域」）。
#
# 这一刀的验收：① 语法被接受、形状被钉住（是表达式，能绑、能当实参）；
# ② **域外 `ext` 是编译错误** —— 这条是 `ext` 设计的核心（失败发生在编译期，而不是运行期）。
# 域本身（注解 + 库里的块形式）与"起一份"是下一小步，所以"域内"那条目前还测不到 ✓。
#
# 对照用例证明 `ext` 是**整词**匹配：`extc_flag` / `extra` / `next_one` 这些名字照常工作。
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
src() { printf '%s/%s' "$here" "$1"; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-22s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-22s %s\n' "$1" "$2"; }

one() { # name file want
  local name="$1" s="$2" want="$3"
  if ! "$EXTC" -w --no-line-map -o "$tmp/a.c" "$s" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/a" "$tmp/a.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return; fi
  local got; got=$("$tmp/a")
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}
neg() { # name file want-substring
  local name="$1" s="$2" want="$3"
  if "$EXTC" -w --no-line-map -o "$tmp/n.c" "$s" 2>"$tmp/e"; then
    bad "$name" "被接受了（应被拒）"; return; fi
  if grep -qF -- "$want" "$tmp/e"; then ok "$name" "被拒 ✓"
  else bad "$name" "理由不对：$(grep -m1 error "$tmp/e" | cut -c1-60)"; fi
}

NODOM='there is no domain here'
neg ext-as-statement     "$(src stmt.extc)"      "$NODOM"
neg ext-is-expression    "$(src binding.extc)"   "$NODOM"
neg ext-nested-in-call   "$(src nested.extc)"    "$NODOM"
neg ext-in-plain-fn      "$(src plain_fn.extc)"  "$NODOM"
neg ext-needs-operand    "$(src bare.extc)"      'expected an expression'
one ext-does-not-swallow-prefixes "$(src control_ident.extc)" "12"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
