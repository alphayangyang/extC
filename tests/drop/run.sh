#!/usr/bin/env bash
# `@drop`：单所有者的类型在离开块、被覆盖时交还它的状态（docs/DESTRUCTORS.md §S1）。
#
# 这一版只有**块尾**与**覆盖赋值**两个时机：块里 `return` 提前离开时不跑（漏释放是可恢复的
# 一侧，放两次是 use-after-free），S2 再把跳出路径补齐。
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
src() { printf '%s/%s' "$here" "$1"; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-24s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-24s %s\n' "$1" "$2"; }

one() { # name file want
  local name="$1" s="$2" want="$3"
  if ! "$EXTC" -w --no-line-map -o "$tmp/a.c" "$s" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/a" "$tmp/a.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return; fi
  local got; got=$("$tmp/a" | tr '\n' '|' | sed 's/|$//')
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}
neg() { # name file want-substring
  local name="$1" s="$2" want="$3"
  if "$EXTC" -w --no-line-map -o "$tmp/n.c" "$s" 2>"$tmp/e"; then
    bad "$name" "被接受了（应被拒）"; return; fi
  if grep -qF -- "$want" "$tmp/e"; then ok "$name" "被拒 ✓"
  else bad "$name" "理由不对：$(grep -m1 error "$tmp/e" | cut -c1-64)"; fi
}

one order       "$(src order.extc)"      "in block|drop 2|after inner|drop 1|done"
one overwrite   "$(src overwrite.extc)"  "drop 1|now a = 2|drop 2"
one no_hook     "$(src no_hook.extc)"    "plain 7"
one early_return "$(src early_return.extc)" "rc=early|drop 1|7|done"
one break_continue "$(src break_continue.extc)" "continue from 0|drop 0|break at 1|drop 1|done"
one generic     "$(src generic.extc)"    "v=3|bye"
neg needs_nocopy "$(src errors/drop_needs_nocopy.extc)" '`@drop` needs a `@noCopy` struct'
neg two_hooks    "$(src errors/two_hooks.extc)"          'already has an `@drop` method'

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" = 0 ]
