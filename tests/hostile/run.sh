#!/usr/bin/env bash
# tests/hostile/run.sh —— **不规范的 .so 攻击主程序**，判据是"怎么爆"（C-ABI.md §9.17）
#
# 每个攻击配一个宿主，run.sh 钉住**失败模式**，而不是"有没有崩"：
#   · 设计赢的：门挡住（forged）· 板内 containment（overrun_plate）· 硬缺页不是静默（keeper_plate）
#   · 攻击赢的：签名是信任不是证明（keeper_frame）· 一道门挡不住 remap · 表是可写的（tablewrite）
#               · 越界写栈（栈保护器终止）· `!` 不检查运行期（缺符号直接调 ⇒ 跳 null）
# 故意把"攻击赢"的那些也写成判据：模型的边界要写在明处，而不是假装没有。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

for m in overrun keeper forger remap table; do
    gcc -std=c11 -fPIC -shared -o "build/hostile-$m.so" "tests/hostile/mod_$m.c" 2>"$tmp/e" \
        || { echo "  FAIL 编不出 hostile-$m.so"; head -2 "$tmp/e"; fail=1; }
done
gcc -std=c11 -fPIC -shared -o build/hostile-empty.so tests/hostile/empty.c 2>/dev/null

# `run <宿主> <期望标记>`：跑起来（rc 必须是 0），输出里必须有那串
run() {
    local f=$1 want=$2 out
    if out=$("$EXTC" --run "tests/hostile/$f.extc" 2>&1); then
        if echo "$out" | grep -qF -- "$want"; then
            echo "  ok   $f  ->  $(echo "$out" | tr '\n' '|' | cut -c1-96)"
        else
            echo "  FAIL $f  ->  期望「$want」，实得：$(echo "$out" | tr '\n' '|' | cut -c1-70)"; fail=1
        fi
    else
        echo "  FAIL $f  ->  没跑起来"; echo "$out" | sed 's/^/        /' | head -3; fail=1
    fi
}

# `crash <宿主> <退出码> <崩溃前必须出现的标记>`：编出二进制直接跑（`--run` 只报驱动自己的码）
crash() {
    local f=$1 wantrc=$2 want=$3 out rc
    if ! "$EXTC" -w --no-line-map -o "$tmp/$f.c" "tests/hostile/$f.extc" >/dev/null 2>&1 ||
       ! gcc -std=c11 -fwrapv -o "$tmp/$f" "$tmp/$f.c" 2>"$tmp/e2"; then
        echo "  FAIL $f  ->  编不出来"; head -2 "$tmp/e2" 2>/dev/null | sed 's/^/        /'; fail=1; return
    fi
    set +e; out=$("$tmp/$f" 2>&1); rc=$?; set -e
    if [ "$rc" = "$wantrc" ] && { [ -z "$want" ] || echo "$out" | grep -qF -- "$want"; }; then
        echo "  ok   $f  ->  rc=$rc  $(echo "$out" | tr '\n' '|' | cut -c1-72)"
    else
        echo "  FAIL $f  ->  期望 rc=$wantrc 且含「$want」，实得 rc=$rc：$(echo "$out" | tr '\n' '|' | cut -c1-60)"; fail=1
    fi
}

echo "== 设计赢的那几个 =="
run   forger          'forged=rejected'          # 一次区间检查挡住伪造的板外指针
run   overrun_plate   'canary=ok'                # 越界写只能踩在板里，宿主帧的哨兵完好
crash keeper_plate 139 'closed'                  # 关板之后再写 = 硬缺页（不是静默）

echo "== 攻击赢的那几个（模型的边界，写在明处）=="
run   keeper_frame    'keep=write-succeeded'     # 签名是信任不是证明：食言的库写进了死掉的帧
crash overrun_stack 134 'stack smashing detected'  # `Addr=0` 只说"不留"，不说"不越界"
run   remap           'hijacked=65'              # munmap+同址重映射 ⇒ 区间检查照样通过
run   tablewrite      'slot-hijacked=1042'       # 表按 mut ref 交出去 ⇒ 模块能改槽
crash missym_call  139 ''                        # `!` 不检查运行期：缺符号直接调 ⇒ 跳 null

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
