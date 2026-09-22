#!/usr/bin/env bash
# tests/generics/run.sh —— **泛型自由函数（PLAN #47）的常设验收**
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（推导 / 显式实参 / 推迟的 T: ==）=="
f=examples/generic-free-fn.extc
if ! out=$("$EXTC" --run "$f" 2>&1); then
    echo "  FAIL generic-free-fn  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
else
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    ok=1
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   generic-free-fn  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL generic-free-fn  ->  输出对不上"; fail=1; fi
fi

echo "== 反例（都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
# ⚠️ 2026-09-23：这里原来有一条
#     check_err tests/generics/errors/generic_calls_generic.extc "not concrete here yet"
# —— 它**断言的是 v1 的限制**。PLAN #50 修好之后「泛型体里调泛型函数」**合法**了，
# 所以那条反例删掉，同一个形状改成**正例**：`examples/generic-calls-generic.extc` ✓
# 教训：反例断言的是"不该存在的行为"时，行为一旦变好就**必须删它** ——
# 留着等于把旧限制焊死 ✗（这一条是 `./check.sh` 当场抓出来的 ✓）
check_err tests/generics/errors/needs_eq.extc             "needs to define"
check_err tests/generics/errors/cannot_infer.extc         "cannot infer"

exit $fail
