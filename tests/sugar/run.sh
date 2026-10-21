#!/usr/bin/env bash
# tests/sugar/run.sh —— 解析期糖的常设验收：一条声明多个名字 + 并行赋值（不新增 AST 节点）
#
# 判据：
#   ① 多名字声明：一条声明共享一个标注、N 个名字对 N 个值、不写值就零初始化、
#      不写标注就每个名字各自推断（var a, b = 1, 2.5 是 i32 和 f64）、顶层 global var 同形状 ✓
#   ①' 类型标注是逐名字的：各写各的 / 只写在最后一个名字后（统一所有没写标注的名字）/
#      混写（自带的优先，末尾标注填给前面没写的）/ 零初始化也逐名字 ✓
#   ② 并行赋值：右值全部先求值（从左到右），然后才从左到右写回 —— 交换不需要临时量 ✓
#   ③ 目标可以是变量 / 字段 / 下标 / 星号解引用 / 载荷 v! ✓
#   ④ 两种糖都能当作 match 臂的单语句体（不带花括号）和 for (init; cond; step) 的 init / step ✓
#   ⑤ 九条反例（名字数≠值数：共享标注与逐名字标注各一条 · 目标数≠值数 · 重名 ·
#      逐名字写初始化式 · 复合赋值带多目标 · @overwrite 带多名字 · 既不写类型也不写值 ·
#      共享标注与值类型不符）必须被编译期挡住，而且消息能认出问题 ✓
#
# 反例本体在 tests/errors/multi_*.extc —— 那里是全局拒绝语料，tools/parrun.py 会自动收；
# 本脚本把它们再钉一遍，好让「只跑这一套」也能验收糖的完整边界。消息期望写在各自的
# // expect: 行里，所以这里不重复一份字符串。
#
# EXTC=... ./tests/sugar/run.sh 可以换一个编译器来跑（证据用：同一套用例分别钉新旧两份）。
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

echo "== 1 多名字声明：共享标注 · 零初始化 · let · 逐名字推断 · 逐名字标注 · 顶层 global =="
for f in tests/sugar/decl_*.extc; do run_case "$f"; done

echo "== 2 并行赋值：交换 · 三路置换 · 字段/下标/deref/载荷 · 先求值后写 =="
for f in tests/sugar/assign_*.extc; do run_case "$f"; done

echo "== 3 糖在 match 臂体与 for 头里 =="
for f in tests/sugar/sugar_*.extc; do run_case "$f"; done

echo "== 4 反例（都必须编译期挡住）=="
for f in tests/errors/multi_*.extc; do check_err "$f"; done

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ]
