#!/usr/bin/env bash
# `@drop`：单所有者的类型在离开块、被覆盖时交还它的状态（docs/design/DESTRUCTORS.md §S1）。
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
one fields      "$(src fields.extc)"     "inner 1|inner 2|wrap 9|inner 3|inner 4|done"
one return_move "$(src return_move.extc)" "got 5|inner 5|close 5|done|close 5"
one explicit_drop "$(src explicit_drop.extc)" "before|drop 1|after|b=2|drop 3|drop 4|end|drop 2"
one revive_after_drop "$(src revive_after_drop.extc)" "drop 1|drop 2"
one param_owner "$(src param_owner.extc)" "n=0 leak=0 take=0"
one generic     "$(src generic.extc)"    "v=3|bye"
one global_exit "$(src global_exit.extc)" "body|close 9"
neg needs_nocopy "$(src errors/drop_needs_nocopy.extc)" '`@drop` needs a `@noCopy` struct'
neg two_hooks    "$(src errors/two_hooks.extc)"          'already has an `@drop` method'
neg drop_then_use "$(src errors/drop_then_use.extc)"      'was dropped here and cannot be read'
neg drop_twice   "$(src errors/drop_twice.extc)"          'was already dropped'
neg drop_no_hook "$(src errors/drop_no_hook.extc)"        'has no `@drop` method'
neg drop_borrowed "$(src errors/drop_borrowed.extc)"      'is dropped while'

quiet() { # name file
  local name="$1" s="$2"
  if ! "$EXTC" --no-line-map -o "$tmp/q.c" "$s" 2>"$tmp/qe"; then
    bad "$name" "编不过：$(head -1 "$tmp/qe")"; return; fi
  if grep -q "nothing in this function closes it" "$tmp/qe"; then
    bad "$name" "仍然报「打开了没关」（@drop 没被认成释放点）"; return; fi
  ok "$name" "不再报资源泄漏 ✓"
}

quiet resource_warning "$(src resource_warning.extc)"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" = 0 ]
