#!/usr/bin/env bash
# tests/ctor/run.sh —— **构造函数（定案 86）的常设验收**
#
# 判据：
#   ① 糖与显式形式**同一条路**：`pt(3, 4)` 与 `pt::new(3, 4)` 值必须一样 ✓
#      （糖在检查器里就是**改写成同一个节点**，没有第二份实现可以漂 ✓）
#   ② 无参 / 单参 / 字符串参数都成立 ✓
#   ③ **可失败的构造函数**返回 `result<T, E>`（打开文件那类）✓
#   ④ 泛型类型：类型实参写全，糖与显式都对 ✓
#   ⑤ 不叫 `new` 的关联函数**不受构造函数规则约束**（`pair2::make` 照旧 ✓）
#   ⑥ 三条反例：没有 `new`（教你怎么写）· 返回类型不是 `T`（定义点就报）· 实参个数不对
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/ctor/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/ctor/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}

echo "== 正例：糖 = 显式 · 无参/单参 · 可失败 · 泛型 · 具名关联函数不受约束 =="
run_case basic
run_case fallible
run_case generic

echo "== 反例（都必须编译期挡住）=="
check_err tests/ctor/errors/no_ctor.extc  'has no constructor `new`'
check_err tests/ctor/errors/bad_ret.extc  'must return `bad`'
check_err tests/ctor/errors/arity.extc    'expects 2 argument(s), got 1'

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
