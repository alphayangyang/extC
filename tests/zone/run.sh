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

echo "== 按值读回：存进去之后从同一个池读出来（曾经是假阳性 G2）=="
if out=$("$EXTC" --run tests/zone/pool_readback.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/zone/pool_readback.extc | sed 's|// expect: *||' | head -1)
    ok=1
    for p in ${want//|/ }; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   pool_readback  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL pool_readback  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL pool_readback  ->  编不过（G2 回归？）"; echo "$out" | head -4 | sed 's/^/        /'; fail=1
fi

echo "== 反例：循环内的存储视图 / 循环局部池读出的值，都不得存进外层池 =="
for f in tests/zone/neg_loop_new_view.extx tests/zone/neg_loop_array_view.extx \
         tests/zone/neg_pool_read_escape.extx; do
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
    if [ "$out" = "before=0 after=0" ]; then
        echo "  ok   handle_abandon     ->  50 个被放弃的句柄任务：池计数 0→0（块退出结束自己能看见的任务）"
    else
        echo "  FAIL handle_abandon     ->  输出=$out（期望 before=0 after=0：不逃逸的句柄必须不泄露）"
        fail=1
    fi
else
    echo "  FAIL handle_abandon     ->  $(head -2 "$ha/e" | tr '\n' ' ')"; fail=1
fi


echo "== 红线：句柄逃逸进容器后，任务仍会漏（整池回收落地后变 0）=="
# 句柄存进容器之后编译器叫不出"最后一个所有者"，所以被驱动过一次就丢弃的任务会把它的 place 与
# 池留到进程结束。这里钉住今天的数字：after=1；整池回收（docs/design/POOL-LEVELS.md）落地那天
# 把 1 改成 0。
he=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$he/h.c" tests/zone/handle_escape_pool.extc >/dev/null 2>&1 \
   && cc -O2 -std=c11 -fwrapv -o "$he/h" "$he/h.c" 2>"$he/e"; then
    out=$("$he/h" 2>&1)
    if [ "$out" = "before=0 after=1" ]; then
        echo "  ok   handle_escape_pool ->  逃逸句柄：池计数 0→1（红线，整池回收后变 0）"
    else
        echo "  FAIL handle_escape_pool ->  输出=$out（期望 before=0 after=1）"
        fail=1
    fi
else
    echo "  FAIL handle_escape_pool ->  $(head -2 "$he/e" | tr '\n' ' ')"; fail=1
fi

if [ "$fail" = 0 ]; then echo "  zone: 通过"; else echo "  zone: 失败"; fi
exit "$fail"
