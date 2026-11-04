#!/usr/bin/env bash
# 视图与容器增长：视图只活到容器搬走存储为止（docs/TYPE-MODEL.md §六）。
#
# 冻结位（pin）还没实现，所以运行时那一半暂时只有静态这一半：同一块里"借出视图 → 容器增长 →
# 之后再读视图"必须编译期拒绝（这正是 ASan 在 vector 上抓到的 heap-use-after-free）。
# 视图在增长前用完、或活在内层块里，都必须编过。
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
  local got; got=$("$tmp/a")
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}
neg() { # name file want-substring
  local name="$1" s="$2" want="$3"
  if "$EXTC" -w --no-line-map -o "$tmp/n.c" "$s" 2>"$tmp/e"; then
    bad "$name" "被接受了（应被拒）"; return; fi
  if grep -qF -- "$want" "$tmp/e"; then ok "$name" "被拒 ✓"
  else bad "$name" "理由不对：$(grep -m1 error "$tmp/e" | cut -c1-64)"; fi
}

neg view_grow_vector "$(src view_grow_vector.extc)" 'changes the storage that `s` views'
neg view_grow_string "$(src view_grow_string.extc)" 'changes the storage that `view` views'
one view_ok_before   "$(src view_ok_before.extc)"   $'1\ngrew'
one view_ok_inner_block "$(src view_ok_inner_block.extc)" $'1\ngrew'

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
