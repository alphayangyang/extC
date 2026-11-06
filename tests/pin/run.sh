#!/usr/bin/env bash
# 视图与容器增长：视图只活到容器搬走存储为止（docs/design/TYPE-MODEL.md §六）。
#
# 静态那一半：同一块里"借出视图 → 容器增长 → 之后再读视图"必须编译期拒绝（这正是 ASan 在
# vector 上抓到的 heap-use-after-free）。动态那一半（跨函数/跨块）由 `buf::borrow()` 的守卫在
# 运行期 trap —— 计数在库里，编译器一条都没参与（`buf`、`vector`、`string` 各一套守卫）。
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

neg nested_view_struct    "$(src nested_view_struct.extc)"    'changes the storage that `h` views'
neg nested_view_container "$(src nested_view_container.extc)" 'changes the storage that `box` views'
one view_loop_reuse       "$(src view_loop_reuse.extc)"       'size=30'
neg view_arg_stored      "$(src view_arg_stored.extc)"      'whose storage moves when it grows'
one view_arg_snapshot    "$(src view_arg_snapshot.extc)"    'kept=7'
neg view_grow_across_call "$(src view_grow_across_call.extc)" 'changes the storage that `s` views'
one view_grow_across_ok   "$(src view_grow_across_call_ok.extc)" 's0=1 len=9'
neg many_views            "$(src many_views.extc)"            'changes the storage that `s16` views'

neg field_store_view     "$(src field_store_view.extc)"     'this field would hold a view of'
one field_store_snapshot "$(src field_store_snapshot.extc)" 's0=7 n=1 grew=25'
neg elem_store_view      "$(src elem_store_view.extc)"      'this element would hold a view of'
one elem_store_snapshot  "$(src elem_store_snapshot.extc)"  '7'
trap() { # name file want-substring
  local name="$1" s="$2" want="$3"
  if ! "$EXTC" -w --no-line-map -o "$tmp/t.c" "$s" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return; fi
  if ! gcc -O2 -std=c11 -fwrapv -o "$tmp/t" "$tmp/t.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return; fi
  local got; got=$("$tmp/t" 2>"$tmp/e3"); local rc=$?
  [ "$rc" -ne 0 ] && grep -qF -- "$want" "$tmp/e3" && ok "$name" "trap ✓" \
    || bad "$name" "rc=$rc err=$(head -1 "$tmp/e3" | cut -c1-48)"
}

one view_ok_borrow_release "$(src borrow_release.extc)" $'borrowed=7\ngrown=7'
trap borrow_freezes "$(src borrow_freezes.extc)" 'frozen by a live borrow'
trap vector_frozen         "$(src vector_frozen.extc)"         'frozen by a live borrow'
one view_ok_vector_release "$(src vector_frozen_release.extc)" $'borrowed len=4\ngrown=5'
trap string_frozen         "$(src string_frozen.extc)"         'frozen by a live borrow'
one view_ok_string_release "$(src string_frozen_release.extc)" $'borrowed=16\nafter=20'
one view_ok_inner_block "$(src view_ok_inner_block.extc)" $'1\ngrew'

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
