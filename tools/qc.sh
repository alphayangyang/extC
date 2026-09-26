#!/usr/bin/env bash
# 一次跑完，直接把挂的文件和输出摆出来 —— 不用跑两遍、不用边跑边找。
#
#   tools/qc.sh                 跑全部套件；失败时自动为每个挂的用例给出完整诊断
#   tools/qc.sh <名字或路径>…   只诊断这些用例（固件名子串或 .extc 路径）
#
# 完整日志留在 $QC_LOG_DIR（默认 /tmp/qc）：<套件>.log，诊断产物 <用例>.c/.err/.gccerr
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG=${QC_LOG_DIR:-/tmp/qc}
mkdir -p "$LOG"
EXTC=$ROOT/build/extc

find_fixture() {   # $1 = 名字子串；回显第一个匹配的 .extc
    find "$ROOT/tests" "$ROOT/examples" "$ROOT/bench" -name "*$1*.extc" 2>/dev/null | head -1
}

diagnose() {       # $1 = 用例名或 .extc 路径
    local name=$1 fx
    if [ -f "$name" ]; then fx=$name; else fx=$(find_fixture "$name"); fi
    if [ -z "${fx:-}" ]; then echo "?? $name —— 找不到固件"; return; fi
    local base c err rc
    base=$(basename "${fx%.extc}"); c=$LOG/$base.c; err=$LOG/$base.err
    echo "── $name   ($fx)"
    rm -f "$c"
    "$EXTC" -w --no-line-map -o "$c" "$fx" > "$err" 2>&1; rc=$?
    if [ $rc -ne 0 ]; then
        echo "   extc 退出码 $rc："; sed 's/^/     /' "$err" | head -12; return
    fi
    echo "   extc 通过（0 诊断）· 生成物 $c"
    if ! gcc -std=c11 -fwrapv -Wall -Werror -o "$c.bin" "$c" 2> "$c.gccerr"; then
        echo "   gcc 失败："; sed 's/^/     /' "$c.gccerr" | head -12; return
    fi
    local out; out=$("$c.bin" 2>&1); rc=$?
    echo "   跑出：退出码 $rc · 输出[$(printf '%s' "$out" | head -3 | tr '\n' '|')]"
}

if [ $# -gt 0 ]; then for n in "$@"; do diagnose "$n"; done; exit 0; fi

run_suite() {      # $1 = 名字；其余 = 命令
    local name=$1; shift
    local log=$LOG/$name.log
    echo "── $name …（日志 $log）"
    ( cd "$ROOT" && "$@" ) > "$log" 2>&1
    local rc=$?
    grep -aE "FAIL|^通过|失败" "$log" | tail -5
    echo "   退出码 $rc"
    return $rc
}

fail=0
run_suite coro bash tests/coro/run.sh || fail=1
run_suite main bash tests/run.sh      || fail=1
run_suite full ./check.sh             || fail=1

# 一次跑完就把每个挂的用例展开（★ 不用再跑第二遍）
for f in $(grep -ah -oE "FAIL[[:space:]]+[A-Za-z0-9_.-]+" "$LOG"/*.log 2>/dev/null |
           awk '{print $2}' | sort -u); do
    diagnose "$f"
done
exit $fail
