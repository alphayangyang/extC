#!/usr/bin/env bash
# tests/iflet/run.sh —— 解析期糖的常设验收：`_` 兜底臂 + `if let` / `while let`
#
# 判据：
#   ① if let：some/none、success/failure、丢弃载荷 `some(_)`、无 else、else 分支、
#      else if let 链、嵌套 if let —— 都只是 match + 显式 `_` 兜底臂的脱糖，没有新语义 ✓
#   ② while let：每轮重新求值被测表达式；匹配就绑载荷跑体、不匹配就结束；
#      continue 回开头、break 提前退出 ✓
#   ③ `_` 整臂：显式写才免穷尽检查、必须是最后一条、不绑载荷；
#      载荷位写 `_` 是丢弃该位（和整臂 `_` 不是一回事）✓
#   ④ 七条反例（`_` 不在最后 · `_` 带载荷 · if let/while let 写成 `_` ·
#      未知变体 · 缺 `=` · 载荷位数不符）必须被编译期挡住，而且消息能认出问题 ✓
#
# 反例本体在 tests/errors/wildcard_*.extc / iflet_*.extc / whilelet_*.extc ——
# 那里是全局拒绝语料，tools/parrun.py 会自动收；本脚本把它们再钉一遍。
# 消息期望写在各自的 // expect: 行里，所以这里不重复一份字符串。
#
# EXTC=... ./tests/iflet/run.sh 可以换一个编译器来跑（证据用：同一套用例分别钉新旧两份）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
fail=0
pass=0
mkdir -p build

ok()  { echo "  ok   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1"; fail=1; }

run_case() {
    local f=$1 n out want p okk=1
    local -a parts
    n=$(basename "$f" .extc)
    if ! out=$("$EXTC" --run "$f" 2>&1); then
        bad "$n  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; return
    fi
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    if [ -z "$want" ]; then bad "$n  ->  文件里没有 // expect: 行"; return; fi
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || okk=0; done
    if [ "$okk" = 1 ]; then ok "$n  ->  $(echo "$out" | tr '\n' '|')"
    else bad "$n  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fi
}

check_err() {
    local f=$1 want out
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    if [ -z "$want" ]; then bad "$(basename "$f")  ->  文件里没有 // expect: 行"; return; fi
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        bad "$(basename "$f")  ->  编过了（应该报错）"; return
    fi
    if echo "$out" | grep -qF -- "$want"; then
        ok "$(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-88)"
    else
        bad "$(basename "$f")  ->  消息里没有「$want」"; echo "$out" | head -4 | sed 's/^/        /'
    fi
}

echo "== 1 if let：option / result / 丢弃载荷 / else if let 链 / 嵌套 =="
for f in tests/iflet/iflet_*.extc; do run_case "$f"; done

echo "== 2 while let：每轮重新求值 · continue · break =="
for f in tests/iflet/whilelet_*.extc; do run_case "$f"; done

echo "== 3 \`_\` 兜底臂：最后一条 · 免穷尽检查 · 载荷位丢弃 =="
for f in tests/iflet/wildcard_*.extc; do run_case "$f"; done

echo "== 4 反例（都必须编译期挡住）=="
for f in tests/errors/wildcard_*.extc tests/errors/iflet_*.extc tests/errors/whilelet_*.extc; do check_err "$f"; done

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ]
