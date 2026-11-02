#!/usr/bin/env bash
# tests/defaults/run.sh —— **默认参数**（`fn f(a: i64, b: i64 = i64(10))`）
#
# 为什么单独一套：默认值会被**复制进每个省略实参的调用点**，所以两件事必须一起钉住 ——
#   ① 语义：省略的实参真的用了默认值，而且和显式写了同样值的结果一致；
#   ② 形状：默认值只允许编译期常量、且必须形成参数表的后缀，否则声明处就报错。
#
# 判据：
#   `basic`        省略 0 个/部分/全部实参（`111 103 6`）
#   `method`       方法带默认值（`12 8`：省略时 +5，给 1 时 +1）
#   `generic`      泛型函数带默认值（`7 30`）
#   `assoc`        关联函数带默认值（`5 12`）
#   `nested`       默认值参与嵌套调用与 `??`（`21 22`）
#   `const_neg`    负数字面量默认值（`-3`：`= i64(0) - i64(3)` 之外的 `= -i64(3)` 形状）
#   `few/many`     实参个数越界 → 编不过，且报文是"expects N to M / at most M"
#   `nonconst`     默认值读变量 → 编不过（"must be a compile-time constant"）
#   `gap`          默认值后面跟必填参数 → 编不过（"needs one too"）
#   `extern`       `extern!` 声明带默认值 → 编不过
#   `selfdefault`  `self` 带默认值 → 编不过
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-12s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-12s %s\n' "$1" "$2"; }

one() { # name file want
    local name=$1 s=$2 want=$3 got
    if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$s" 2>"$tmp/e"; then
        bad "$name" "编译失败：$(head -1 "$tmp/e")"; return; fi
    if ! gcc -O2 -std=c11 -fwrapv -o "$tmp/$name" "$tmp/$name.c" 2>"$tmp/ce"; then
        bad "$name" "产物编不过：$(head -1 "$tmp/ce")"; return; fi
    got=$("$tmp/$name")
    [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}

negsrc() { # name file needle
    local name=$1 s=$2 needle=$3
    if "$EXTC" -w --no-line-map -o "$tmp/neg.c" "$s" 2>"$tmp/e"; then
        bad "$name" "本该编不过，却通过了"; return; fi
    if grep -qF "$needle" "$tmp/e"; then
        ok "$name" "拒绝：$(head -1 "$tmp/e" | sed 's/^[^:]*:[0-9]*:[0-9]*: //')"
    else
        bad "$name" "报文里没有 [$needle]：$(head -1 "$tmp/e")"
    fi
}

one basic     tests/defaults/basic.extc      "111 103 6"
one method    tests/defaults/method.extc     "12 8"
one generic   tests/defaults/generic.extc    "0 7"
one assoc     tests/defaults/assoc.extc      "5 12"
one nested    tests/defaults/nested.extc     "21 7"
one const_neg tests/defaults/const_neg.extc  "-3 12"

negsrc few      tests/defaults/neg_few.extc      "expects 1 to 2 argument(s), got 0"
negsrc many     tests/defaults/neg_many.extc     "expects at most 1 argument(s), got 2"
negsrc nonconst tests/defaults/neg_nonconst.extc "must be a compile-time constant"
negsrc gap      tests/defaults/neg_gap.extc      "needs one too"
negsrc extdecl  tests/defaults/neg_extern.extc   "cannot have a default value"
negsrc selfdflt tests/defaults/neg_self.extc     '`self` cannot have a default value'

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
