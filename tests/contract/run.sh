#!/usr/bin/env bash
# tests/contract/run.sh —— **契约验证**：把"签字说错 = UB"变成可证伪的判据（C-ABI.md §9.21）
#
# 与 tests/hostile 是一对孪生，问的不是同一个问题：
#   · hostile 问"不规范的 .so 打过来会怎么爆"        （威胁模型：假设对方会撒谎）
#   · contract 问"我们替库签的那句话，库认不认"      （一致性：把撒谎变成当场可证伪）
#
# 四条判据，缺一不可：
#   ① 诚实库 + 调用后隔离  ⇒ 干净通过（契约成立，隔离本身不误伤）；
#   ② 食言库 + 隔离        ⇒ 硬缺页 rc=139（契约被证伪，而且响亮）；
#   ③ 食言库 + 毒化        ⇒ 读到毒值、rc=0 + 打印 falsified（不崩的那条路，CI 能收集全部结论）；
#   ④ 食言库 + 什么都不做  ⇒ 一切正常（对照：没有工具时，谎言在行为上不可见）。
#
# 判据只用 139 / 0 与输出里的标记，不依赖时序：崩溃点由"隔离"这一动作确定。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"
fail=0; pass=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { pass=$((pass+1)); printf '  ok   %-18s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-18s %s\n' "$1" "$2"; }

for m in honest keeper; do
    if ! gcc -std=c11 -fPIC -shared -o "build/contract-$m.so" "tests/contract/mod_$m.c" 2>"$tmp/e"; then
        echo "  FAIL 编不出 contract-$m.so"; head -2 "$tmp/e"; exit 1
    fi
done

# `run <宿主> <期望标记>`：rc 必须是 0，输出里必须有那串。
run() {
    local f=$1 want=$2 out
    if out=$("$EXTC" --run "tests/contract/$f.extc" 2>&1); then
        if echo "$out" | grep -qF -- "$want"; then
            ok "$f" "$(echo "$out" | tr '\n' '|' | cut -c1-72)"
        else
            bad "$f" "期望「$want」，实得：$(echo "$out" | tr '\n' '|' | cut -c1-60)"
        fi
    else
        bad "$f" "没跑起来：$(echo "$out" | tr '\n' '|' | cut -c1-60)"
    fi
}

# `crash <宿主> <期望退出码> <崩溃前必须出现的标记>`。
crash() {
    local f=$1 wantrc=$2 want=$3 out rc
    if ! "$EXTC" -w --no-line-map -o "$tmp/$f.c" "tests/contract/$f.extc" >/dev/null 2>&1 ||
       ! gcc -std=c11 -fwrapv -o "$tmp/$f" "$tmp/$f.c" 2>"$tmp/e2"; then
        bad "$f" "编不出来：$(head -1 "$tmp/e2" 2>/dev/null)"; return
    fi
    set +e; out=$("$tmp/$f" 2>&1); rc=$?; set -e
    if [ "$rc" = "$wantrc" ] && echo "$out" | grep -qF -- "$want"; then
        ok "$f" "rc=$rc  $(echo "$out" | tr '\n' '|' | cut -c1-60)"
    else
        bad "$f" "期望 rc=$wantrc 且含「$want」，实得 rc=$rc：$(echo "$out" | tr '\n' '|' | cut -c1-50)"
    fi
}

echo "== ① 契约成立（诚实库 + 隔离）=="
run honest 'contract=holds'

echo "== ② 契约被证伪（食言库 + 隔离 ⇒ 响亮）=="
crash keeper 139 'contract=armed'

echo "== ③ 契约被证伪（食言库 + 毒化 ⇒ 可断言，不崩）=="
run keeper_poison 'contract=falsified'

echo "== ④ 对照：没有工具时，谎言不可见 =="
run keeper_silent 'silent=stale'

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
