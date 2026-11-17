#!/usr/bin/env bash
# tests/dense/run.sh —— 容器去池化的地基：`block<T>`（拥有的类型化块）与 `slots<T>`（稠密槽存储）
#
# 判据：
#   ① 每个程序的整行输出（`// expect:` 第一行自带数字自证）
#   ② gcc -Werror 干净（生成 C 的告警是缺陷）
#   ③ 存储回到基线：`leak` 用 `bufLive` 账本判「到零」，不是靠肉眼看 RSS
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-12s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-12s %s\n' "$1" "$2"; }

one() { # name file
  local name="$1" f="$here/$2" want got
  want=$(head -1 "$f" | sed 's|^// expect: ||')
  if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$f" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
        -o "$tmp/$name" "$tmp/$name.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return; fi
  got=$("$tmp/$name")
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}

one basic basic.extc
one stale stale.extc
one leak  leak.extc
one grow  grow.extc
one order order.extc

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]