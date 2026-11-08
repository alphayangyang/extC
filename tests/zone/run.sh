#!/usr/bin/env bash
# tests/zone/run.sh —— 池层级判定的常设验收：**同类问题只有一种答案**
#
# 判据：
#   ① 循环里构造的池值，无论写成 `f(x)` / `recv.m(x)` / `a + b` / `a ?? b` / 结构体字面量，
#      存进活在循环外面的池都必须通过，且运行时值正确（`pool_forms`）✓
#   ② 反方向不许放宽：循环里 `new` 出来的缓冲区视图、循环里声明的定长数组视图，
#      存进外层池必须**在检查期**被拒（两条 neg 用例）✓
#
# 背景：`promoteInto2` 里"能不能按目的地降池层级"曾经是手写 kind 枚举，`a + b` 漏在里面，
# 于是 method 写法能过、运算符写法报 "borrowed from depth 4"（提交 14093e6 修好，
# `tools/check_call_sites.py` 防止再长出第二份枚举）。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例：所有调用形状在循环里构造池值再存入外层池 =="
if out=$("$EXTC" --run tests/zone/pool_forms.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/zone/pool_forms.extc | sed 's|// expect: *||' | head -1)
    ok=1
    for p in ${want//|/ }; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   pool_forms  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL pool_forms  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL pool_forms  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -8; fail=1
fi

echo "== 已知缺口 G2（docs/design/POOL-LEVELS.md）：按值读回仍被拒（假阳性）=="
if out=$("$EXTC" --run tests/zone/pool_readback.extc 2>&1); then
    echo "  ok   pool_readback  ->  现在编过了：G2 已修，请把这条改成正例判据（$(echo "$out" | tr '\n' '|')）"
else
    echo "$out" | grep -qF "would hold a reference to a local variable that dies first" \
        && echo "  ok   pool_readback  ->  仍按 G2 记录的方式被拒（读回的层级取得过深）" \
        || { echo "  FAIL pool_readback  ->  拒绝理由变了（不再是 G2 的层级假阳性）"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; }
fi

echo "== 反例：循环内的存储视图不得存进外层池 =="
for f in tests/zone/neg_loop_new_view.extx tests/zone/neg_loop_array_view.extx; do
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; continue
    fi
    if echo "$out" | grep -qF -- "$want"; then
        echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
    else
        echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; echo "$out" | head -4 | sed 's/^/        /'; fail=1
    fi
done


echo "== 现状：包装句柄被放弃时，任务仍会漏（红线上限）=="
# 栈帧形式的协程已经在块退出时结束任务（2026-11-76）；句柄形式不行：句柄可以移动，任务该由
# 最后一个所有者结束，编译器不知道是谁。这里把"今天还漏"钉住：修好那天把 50 改成 0。
ha=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$ha/h.c" tests/zone/handle_abandon.extc >/dev/null 2>&1 \
   && cc -O2 -std=c11 -fwrapv -o "$ha/h" "$ha/h.c" 2>"$ha/e"; then
    out=$("$ha/h" 2>&1)
    if [ "$out" = "before=0 after=50" ]; then
        echo "  ok   handle_abandon     ->  50 个被放弃的句柄任务：池计数 0→50（已知缺口，见 docs/plans/TODO.md）"
    else
        echo "  FAIL handle_abandon     ->  输出=$out（期望 before=0 after=50：要么漏得更多，要么已经修好了 —— 后者就把判据改成 after=0）"
        fail=1
    fi
else
    echo "  FAIL handle_abandon     ->  $(head -2 "$ha/e" | tr '\n' ' ')"; fail=1
fi

if [ "$fail" = 0 ]; then echo "  zone: 通过"; else echo "  zone: 失败"; fi
exit "$fail"
