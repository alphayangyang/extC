#!/usr/bin/env bash
# tests/weak/run.sh —— 弱引用 `weak<T>`：拿得到、失效得掉、拷得走、非回收对象 trap
#
# 判据：
#   ① 每个程序的整行输出（`// expect:` 第一行自带数字自证；`1`/`0` 是 bool）
#   ② gcc -Werror 干净（生成 C 的告警是缺陷）
#   ③ `trap` 必须运行期停住：退出码非 0 且 stderr 含期望的 trap 文本（不是编译错，也不是静默通过）
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-12s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-12s %s\n' "$1" "$2"; }

build() { # name file -> tmp/name, sets rc
  if ! "$EXTC" -w --no-line-map -o "$tmp/$1.c" "$2" 2>"$tmp/e1"; then
    bad "$1" "extC: $(head -1 "$tmp/e1")"; return 1; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
        -o "$tmp/$1" "$tmp/$1.c" 2>"$tmp/e2"; then
    bad "$1" "gcc: $(head -1 "$tmp/e2")"; return 1; fi
}

one() { # name file
  local name="$1" f="$here/$2" want got
  want=$(head -1 "$f" | sed 's|^// expect: ||')
  build "$name" "$f" || return
  got=$("$tmp/$name")
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}

trapping() { # name file
  local name="$1" f="$here/$2" want err rc
  want=$(head -1 "$f" | sed 's|^// expect-trap: ||')
  build "$name" "$f" || return
  err=$("$tmp/$name" 2>&1 >/dev/null); rc=$?
  if [ "$rc" -eq 0 ]; then bad "$name" "trap 没有发生（退出码 0）"; return; fi
  case "$err" in
    *"$want"*) ok "$name" "→ trap: $want" ;;
    *) bad "$name" "trap 文本不符：[$err]" ;;
  esac
}

one basic basic.extc
one stale stale.extc
one many  many.extc
trapping trap trap.extc

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
