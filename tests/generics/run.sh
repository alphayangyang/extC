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
check_err tests/generics/errors/generic_calls_generic.extc "not concrete here yet"
check_err tests/generics/errors/needs_eq.extc             "needs to define"
check_err tests/generics/errors/cannot_infer.extc         "cannot infer"

exit $fail
