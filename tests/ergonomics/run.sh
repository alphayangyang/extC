#!/usr/bin/env bash
# tests/ergonomics/run.sh —— 常用写法的手感回归（语法糖 + 库面）
#
# 为什么单独一套：这一批改动的共同点是"用户每天会写、但以前写不出来或要绕"的写法。
# 每一项都既钉**语义**（结果对），也钉**形状**（该拒绝的必须拒绝，且报文说人话）：
#   slice_methods   字面量是 `slice<u8>`，`"  x  ".trim()` / `"a,b".split(",")` 直接可用
#   string_methods  `string` 上同名方法（同一份实现，两种接收者）
#   struct_commas   结构体字段允许逗号（与结构体字面量同一规则）
#   numbers         `abs/min/max/clamp/toF64/toI32`（i64 · i32 · u64 · f64）
#   assert          `assert` / `assertMsg` / `unreachable`（库里写，`trap` 是语句）
#   io_println      `io::println` 与内建 `println` 是同一个打印器（数字/布尔/字符串/f-string）
#   view_len        视图的 `len` 不能写（`mut slice` 也不例外）→ 编译错误
#   assert_fail     断言失败 → 运行期 trap（带文件行号）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-14s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-14s %s\n' "$1" "$2"; }

build() { # name file -> 编出可执行文件，返回 0/1
    local name=$1 s=$2
    if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$s" 2>"$tmp/e"; then
        bad "$name" "编译失败：$(head -1 "$tmp/e")"; return 1; fi
    if ! gcc -O2 -std=c11 -fwrapv -o "$tmp/$name" "$tmp/$name.c" 2>"$tmp/ce"; then
        bad "$name" "产物编不过：$(head -1 "$tmp/ce")"; return 1; fi
    return 0
}

one() { # name file want
    local name=$1 s=$2 want=$3 got
    build "$name" "$s" || return
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

runfails() { # name file needle
    local name=$1 s=$2 needle=$3
    build "$name" "$s" || return
    if "$tmp/$name" >"$tmp/out" 2>"$tmp/err"; then
        bad "$name" "本该 trap，却正常退出：$(cat "$tmp/out")"; return; fi
    if grep -qF "$needle" "$tmp/err"; then
        ok "$name" "trap：$(head -1 "$tmp/err" | sed 's/^[^:]*:[0-9]*: //')"
    else
        bad "$name" "stderr 里没有 [$needle]：$(head -1 "$tmp/err")"
    fi
}

one slice_methods  tests/ergonomics/slice_methods.extc  "trim=[a b] start=[a b]   end=  [a b] split=3 second=b upper-ish=[A B] find=3 has=true false len=9 ints=42"
one string_methods tests/ergonomics/string_methods.extc "s=hi n=2 parts=2 first=x"
one struct_commas  tests/ergonomics/nb_struct_commas.extc "3 4 sum=7"
one numbers        tests/ergonomics/nb_numbers.extc "7 0 0 0 2.5 4 4 -4"
one assert         tests/ergonomics/nb_assert.extc "ok 7"
one impl_adds      tests/ergonomics/impl_adds.extc "eq=true len=3 trimmed=[ab]"
one io_println     tests/ergonomics/nb_io_println.extc "42
hi
true
size=5
n = 7"

negsrc view_len  tests/ergonomics/neg_view_len.extc  'cannot assign `len` on a view of type `mut slice<u8>`'
runfails assert_fail tests/ergonomics/neg_assert.extc "trap: assertion failed"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
