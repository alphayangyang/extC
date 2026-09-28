#!/usr/bin/env bash
# lambda 字面量的**语法与解析**判据（LAMBDA.md 第 6 节第 1 步）。
#
# 这一刀只做解析：语法必须被接受，然后由 checker 响亮拒绝（绝不是静默生成一个空结构体）。
# 因此每个正例的期望是"走到 checker 并报 not implemented yet" —— 它同时证明了解析器认这个形状、
# 而且拒绝的理由是我们写的那一条。反例是**语法**错误，必须在解析期就被挡下。
#
# 捕获列表只写"要写的那些"（作者决策），且顺序是 `fn(形参) [捕获] -> 返回 { 体 }`（LAMBDA.md §2）。
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
src() { printf '%s/%s' "$here" "$1"; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-30s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-30s %s\n' "$1" "$2"; }

# 必须被拒，且诊断里含指定子串；同时记录"被拒在哪一层"
neg() { # name file want
  local name="$1" s="$2" want="$3"
  if "$EXTC" -w --no-line-map -o "$tmp/n.c" "$s" 2>"$tmp/e"; then
    bad "$name" "被接受了（应被拒）"; return
  fi
  if grep -qF -- "$want" "$tmp/e"; then ok "$name" "被拒：$(grep -m1 -oF -- "$want" "$tmp/e")"
  else bad "$name" "理由不对：$(grep -m1 error "$tmp/e" | cut -c1-72)"; fi
}

# 编译并且运行、比对输出（确保新分支没破坏普通程序）
one() { # name file want
  local name="$1" s="$2" want="$3"
  if ! "$EXTC" -w --no-line-map -o "$tmp/a.c" "$s" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/a" "$tmp/a.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return; fi
  local got; got=$("$tmp/a")
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}

NOTYET='a lambda is not implemented yet'
neg lambda-no-capture      "$(src no_capture.extc)"      "$NOTYET"
neg lambda-read-capture    "$(src read_capture.extc)"    "$NOTYET"
neg lambda-mut-capture     "$(src mut_capture.extc)"     "$NOTYET"
neg lambda-no-param        "$(src no_param.extc)"        "$NOTYET"
neg lambda-called          "$(src called.extc)"          "$NOTYET"
neg lambda-nested-fn       "$(src nested_in_fn.extc)"    "$NOTYET"
neg capture-without-mutref "$(src bad_capture.extc)"     'a capture list entry is `mut ref name`'
neg capture-ref-only       "$(src bad_capture2.extc)"    'a capture list entry is `mut ref name`'
one plain-fn-still-works   "$(src plain.extc)"           "42"
one control-flow-no-lambda  "$(src control_flow.extc)"   "5"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
