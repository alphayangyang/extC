#!/usr/bin/env bash
# lambda 字面量（docs/topics/LAMBDA.md，作者 v2 决策）：语法、捕获、调用、以及必须被拒的形状。
#
# 语义要点都钉在这里：
#   · 读外层变量 = **按值捕获** ⇒ 闭包看到的是建立它时的值（改外层不影响已建立的闭包）；
#   · 写外层变量必须显式 `[mut ref x]` ⇒ 写的是**外层那个变量**（测试里 hits 会变）；
#   · 返回类型可省 ⇒ 由体内第一个 `return <value>` 定下来；
#   · 闭包的**类型不可命名** ⇒ v1 里它只能活在写它的那个作用域（既不能声明也不能返回）。
#
# 走查器：13 个手写表达式走查器只走闭包的**环境字段值**（构造点那几次读），体归 `call` 方法
# 自己的按函数分析——于是 ALLOW 清空，`tools/check_walkers.py` 报 every kind ✓。
# 例外是模块改写（modules.c 的 rwExpr）：那是**改写**不是分析，必须进体，否则体内对同模块函数
# 的调用不会被 mangling，`call` 方法成空壳、静默算错（见 modcross 用例）。
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

one no_capture             "$(src no_capture.extc)"  "42"
one read_capture           "$(src read_capture.extc)"  "101"
one mut_capture            "$(src mut_capture.extc)"  "11 2"
one infer_ret              "$(src infer_ret.extc)"  "42"
one two_params             "$(src two_params.extc)"  "7"
one struct_field           "$(src struct_field.extc)"  "42"
one nested_in_fn           "$(src nested_in_fn.extc)"  "42"
one closed_over_view       "$(src closed_over_view.extc)"  "30"
one plain                  "$(src plain.extc)"  "42"
one control_flow           "$(src control_flow.extc)"  "5"
neg bad_capture            "$(src bad_capture.extc)"  'a capture list entry is `mut ref name`'
neg bad_capture2           "$(src bad_capture2.extc)"  'a capture list entry is `mut ref name`'
neg write_undeclared       "$(src write_undeclared.extc)"  "but the capture list does not name it"
neg capture_unused         "$(src capture_unused.extc)"  "is not an enclosing variable this body uses"
neg nested_lambda          "$(src nested_lambda.extc)"  "a lambda inside a lambda is not supported yet"
neg new_in_body            "$(src new_in_body.extc)"  "a lambda body may not allocate yet"
neg ret_no_value           "$(src ret_no_value.extc)"  "this lambda returns no value"
neg cannot_infer           "$(src cannot_infer.extc)"  "cannot infer the result type of this lambda"

# 跨模块：**模块文件**里的闭包，体内调用同模块函数（曾经静默返回 0，无诊断）
modcase() { # name dir want
  local name="$1" dir="$2" want="$3" got
  if ! got=$("$EXTC" --run "$here/$dir/main.extc" 2>"$tmp/m"); then
    bad "$name" "$(head -1 "$tmp/m")"; return; fi
  [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}
modcase mod-cross-module        modcross "42"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
