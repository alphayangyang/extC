#!/usr/bin/env bash
# tests/elemrefs/run.sh — element references into fixed-size arrays.
#
# Criterion: `ref a[i]` is a place. Reads, writes, aliases and passing the reference to a
# function all work without an extra indirection, and the checks that already guard bindings
# apply unchanged:
#   1. a reference taken out of the frame that owns the array is a compile error;
#   2. the bounds check runs where the reference is taken (out of range traps there);
#   3. an element of a view is still refused, with a note that says why.
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
mkdir -p build

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/elemrefs/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/elemrefs/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

trap_case() {
    local t=$1 out want
    want=$(grep -o '// expect-trap:.*' "tests/elemrefs/$t.extc" | sed 's|// expect-trap: *||' | head -1)
    if out=$("$EXTC" --run "tests/elemrefs/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  没有 trap"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $t  ->  trap 文本里没有「$want」"; fail=1; return; }
    echo "  ok   $t  ->  $(echo "$out" | grep -m1 'trap:' | cut -c1-80)"
}

check_err() {
    local t=$1 want=$2 out
    if out=$("$EXTC" "tests/elemrefs/$t.extc" -o /dev/null 2>&1); then
        echo "  FAIL $t  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $t  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $t  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}

echo "== 正例：读、写、别名、传参、循环里重取、本帧 new 的视图 =="
run_case basic
run_case loop
run_case slice_new

echo "== 正例：借用窗口里的元素引用 =="
run_case borrow

echo "== 判据有牙：窗口内不许 resize；越界在取引用处 trap；跨帧返回、容器视图被拒 =="
check_err borrow_no_resize "no method"
check_err borrow_addressed "cannot borrow"
check_err escape_unsupported "this return value would hold a reference to a local variable that dies first"
trap_case oob
check_err view_unsupported "cannot take a reference to an element of this view"

echo
if [ "$fail" = 0 ]; then echo "elemrefs: ok"; else echo "elemrefs: FAIL"; exit 1; fi
