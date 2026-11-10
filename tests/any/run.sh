#!/usr/bin/env bash
# tests/any/run.sh —— 动态值 `any`：装箱、隐式拆箱、换类型、返回、容器、打印、相等、以及
# 唯一一条"可能运行期失败"的隐式转换（拆箱类型不符 ⇒ 带源码位置的 trap）。
#
# 判据：
#   ① 正例：写完立刻读回来的值必须逐字正确（含函数返回 any 再拆成具体类型）
#   ② 类型不符：编译通过、**运行时** trap，报文指出实际类型与期望类型（不是静默重解释）
#   ③ 打印/相等/容器：空槽、两个 any 比较、varArray<any>
#   ④ 产物在 `-Wall -Wextra -Werror` 下干净（`any` 的帮助函数有 unused 变体）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-22s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-22s %s\n' "$1" "$2"; }

build() { # name file -> sets bin, or reports
    local name=$1 s=$2
    bin="$tmp/$name"
    if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$s" 2>"$tmp/$name.e"; then
        bad "$name" "extc: $(head -1 "$tmp/$name.e")"; return 1; fi
    if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$bin" "$tmp/$name.c" 2>"$tmp/$name.ce"; then
        bad "$name" "gcc -Werror: $(head -1 "$tmp/$name.ce")"; return 1; fi
    return 0
}

dispatch_miss() { # name file receiver -- 期望"程序编得过、运行期 trap 说没有这个方法"
    local name=$1 out rc recv=${3:-line}
    build "$1" "$2" || return
    out=$("$bin" 2>&1); rc=$?
    [ "$rc" -ne 0 ] || { bad "$name" "没有 trap（rc=0）"; return; }
    if printf '%s' "$out" | grep -q "trap: no \`area\` for a \`$recv\`"; then
        ok "$name" "$(printf '%s' "$out" | tail -1)"
    else
        bad "$name" "trap 报文不对：$(printf '%s' "$out" | tail -1 | cut -c1-60)"
    fi
}

one() { # name file want
    local name=$1 want=$3 got
    build "$1" "$2" || return
    got=$("$bin")
    [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
}

trapcase() { # name file  -- 期望运行时 trap，报文含 "trap:" 与 "holds a"
    local name=$1 out rc
    build "$1" "$2" || return
    out=$("$bin" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then bad "$name" "没有 trap（rc=0）"; return; fi
    if printf '%s' "$out" | grep -q 'trap: this `any` holds'; then
        ok "$name" "$(printf '%s' "$out" | tail -1)"
    else
        bad "$name" "trap 报文不对：$(printf '%s' "$out" | tail -1 | cut -c1-60)"
    fi
}

one box_basic          tests/any/box_basic.extc          "3 7 7"
one rebind_string      tests/any/rebind_string.extc      "first=7
second=abc"
one print_eq_container tests/any/print_eq_container.extc "eq=true ne=false n=2
v=1/2"
one dyn_method         tests/any/dyn_method.extc          "a=12
a=12
k=2"
one own_payload        tests/any/own_payload.extc           "snapshot=ABCD live=ZBCD"
trapcase unbox_mismatch tests/any/unbox_mismatch.extc
dispatch_miss dyn_method_missing tests/any/dyn_method_missing.extc
one dyn_wide            tests/any/dyn_wide.extc            "acc=156"
one monotonic           tests/any/monotonic.extc           "sq=16 ci=15 bx=9"
one reflect_json        tests/any/reflect_json.extc        "json={\"name\":\"abc\",\"p\":{\"x\":1,\"y\":-2},\"ok\":true}
type=rec
scalar=7"
one box_ref_field       tests/any/box_ref_field.extc       "node { k: 1, next: <ref> }
json={\"k\":1,\"next\":null}
type=node
leaf=2"
dispatch_miss dyn_wide_missing tests/any/dyn_wide_missing.extc plain

# 反例：`@noCopy` 的值不能被装进 `any`（装箱是拷贝，会给出第二个所有者）。
if ./build/extc -w --no-line-map -o "$tmp/nb.c" tests/any/no_box_nocopy.extc 2>"$tmp/nb.e"; then
    bad "no_box_nocopy" "编过了（应该拒绝 ✗）"
elif grep -q 'is `@noCopy`, so it cannot be boxed' "$tmp/nb.e"; then
    ok "no_box_nocopy" "$(grep -m1 'error:' "$tmp/nb.e" | cut -c1-72)"
else
    bad "no_box_nocopy" "报文不对：$(head -1 "$tmp/nb.e")"
fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
