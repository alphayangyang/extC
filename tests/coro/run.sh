#!/usr/bin/env bash
# 协程原型（CONCURRENCY.md §4 第 1 步）：**手工写的状态机**，零编译器改动。
#
# 判据：RESP 风格协议按**任意切分**喂进去，产出必须与一次喂完逐字节相同；
# 且整段响应与黄金值一致（既判"可恢复"也判"语义对"）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
tmp=$(mktemp -d)

# 切片 B2a：spawn + 驱动（`while c.next() { c.value() }`）**端到端跑起来** ✓
b2=$tmp/coro_drive
if "$EXTC" -w --no-line-map -o "$b2.c" tests/coro/coro_drive.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$b2" "$b2.c" 2>"$tmp/b2c"; then
    out=$("$b2"); rc=$?
    if [ "$out" = "0 1 2 " ] && [ "$rc" = 3 ]; then
        echo "  ok   coro_drive         ->  spawn + next/value 跑出 [$out] 退出码 $rc（和 3 次 yield 一致）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_drive         ->  输出 [$out] 退出码 $rc"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_drive         ->  $(head -2 "$tmp/b2c" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 切片 B1：帧 + step 的**差分判据** —— 生成的 step 驱动出的序列必须与手写状态机逐字节相同 ✓
# （`-DEXTC_CORO_B1_HARNESS` 只用来绕过"调用协程"的守门：调用是 B2 的事 ✓ 定义侧的变换在这里验 ✓）
gen=$tmp/coro_step_gen.c
if "$EXTC" -w --no-line-map -o "$gen" tests/coro/coro_step.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -DEXTC_CORO_B1_HARNESS -fsyntax-only "$gen" 2>"$tmp/b1e"; then
    if gcc -std=c11 -fwrapv -Wall -Werror -DEXTC_CORO_B1_HARNESS -I "$tmp" \
           tests/coro/coro_step_harness.c -o "$tmp/b1h" 2>"$tmp/b1c" \
       && [ "$("$tmp/b1h")" = "0 1 2 " ]; then
        echo "  ok   coro_step_harness  ->  生成的 step 产出 $( "$tmp/b1h" | tr -d '\n' )（与手写状态机逐字节相同）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_step_harness  ->  harness：$(head -2 "$tmp/b1c" | tr '\n' ' ')"
        fail=$((fail+1))
    fi
else
    echo "  FAIL coro_step_harness  ->  生成物合同：$(head -2 "$tmp/b1e" | tr '\n' ' ')"
    fail=$((fail+1))
fi

# 协程 A2：帧布局必须是"跨挂起点存活的局部"（pc/ret + i ⇒ 参数 n 不进帧）
lay=$(EXTC_DBG_CORO=1 "$EXTC" tests/coro/coro_frame_layout.extc -o "$tmp/cl.c" 2>&1 | grep -o "frame = .*")
want_lay='frame = pc, ret, n: i64, i: i64   (2 fields: 1 param + 1 live-across-yield local)'
if [ "$lay" = "$want_lay" ]; then
    echo "  ok   coro_frame_layout   ->  $lay"
    pass=$((pass+1))
else
    echo "  FAIL coro_frame_layout   ->  期望 [$want_lay]，得到 [$lay]"
    fail=$((fail+1))
fi

# 手工调度器原型（CONCURRENCY.md §4 第 4 步前身）：帧是值、轮转可恢复
out=$("$EXTC" -w --run tests/coro/scheduler_prototype.extc 2>&1)
want_sc='t0=0,0,10,11,20,22, sum0=30 sum1=33'
if [ "$out" = "$want_sc" ]; then
    echo "  ok   scheduler_prototype ->  两份帧轮转交错推进、互不干扰：$out"
    pass=$((pass+1))
else
    echo "  FAIL scheduler_prototype ->  期望 [$want_sc]，得到 [$out]"
    fail=$((fail+1))
fi

# 闭包原型（CONCURRENCY.md §4 第 3 步）：捕获环境 = 显式结构体 ⇒ 既有逃逸规则直接管它
out=$("$EXTC" -w --run tests/coro/closure_prototype.extc 2>&1)
want_cl=$(printf 'sum=50\nfold=26\ntwo=14')
if [ "$out" = "$want_cl" ]; then
    echo "  ok   closure_prototype  ->  手工闭包（环境 struct + 自由函数 + tag 派发）：$(printf '%s' "$out" | tr '\n' ' ')"
    pass=$((pass+1))
else
    echo "  FAIL closure_prototype  ->  期望 [$(printf '%s' "$want_cl" | tr '\n' ' ')]，得到 [$(printf '%s' "$out" | tr '\n' ' ')]"
    fail=$((fail+1))
fi

# 迭代器协议原型（CONCURRENCY.md §4 第 2 步）：用户类型 + 三个方法（iter/next/value），
# 用**显式 while** 驱动 —— 先证明这个形状在今天的语言里写得出来、跑得对，
# 再谈让 `for x in c` 脱糖成它（今天 `for` 只走切片：`cannot slice a value of type ...`）。
ip=$("$EXTC" -w --run tests/coro/iterator_protocol.extc 2>&1)
want_it=$(printf 'sum=15 shown=5\nempty=0\nsteps=4\nfor:sum=15 shown=5\nfor:empty=0\nfor:steps=4\nfield:while=6 for=6\nderef:while=6 for=6')
if [ "$ip" = "$want_it" ]; then
    echo "  ok   iterator_protocol   ->  手写协议原型（iter/next/value + 显式 while）：$(printf '%s' "$ip" | tr '\n' ' ')"
    pass=$((pass+1))
else
    echo "  FAIL iterator_protocol   ->  期望 [$(printf '%s' "$want_it" | tr '\n' ' ')]，得到 [$(printf '%s' "$ip" | tr '\n' ' ')]"
    fail=$((fail+1))
fi

out=$("$EXTC" -w --run tests/coro/statemachine.extc 2>&1)
# 行尾是 CRLF（协议定义如此）；文本比对时去掉 \r，另用一条断言盯住 CRLF 本身。
got=$(printf '%s' "$out" | sed -n '/--- 一次喂完 ---/,$p' | tail -n +2 | head -9 | tr -d '\r')
want=$(printf '+PONG\n+OK\n$5\nhello\n$-1\n+OK\n$3\nxyz\n+PONG')
case "$out" in *$'+OK\r\n'*) crlf=1 ;; *) crlf=0 ;; esac

if [ "$got" = "$want" ] && [ "$crlf" = 1 ]; then
    echo "  ok   coro_sm_transcript  ->  响应逐行正确（PING/SET/GET/未命中/二次 SET · CRLF 也验了）"; pass=$((pass+1))
else
    echo "  FAIL coro_sm_transcript  ->  响应不符"; printf '%s\n' "$got" | sed 's/^/        /'; fail=$((fail+1))
fi
case "$out" in
    *"切分 1..16 与一次喂完一致：true"*)
        echo "  ok   coro_sm_chunking    ->  切分 1..16 字节与一次喂完结果逐字节相同（可恢复性）"; pass=$((pass+1)) ;;
    *)
        echo "  FAIL coro_sm_chunking    ->  切分不变性不成立"; fail=$((fail+1)) ;;
esac
# 合同：生成物必须过 -std=c11 零告警
if "$EXTC" -w --no-line-map -o "$tmp/sm.c" tests/coro/statemachine.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -fsyntax-only "$tmp/sm.c" 2>"$tmp/e"; then
    echo "  ok   coro_sm_contract    ->  生成物合同编译零告警（-std=c11 -fwrapv -Wall -Werror）"; pass=$((pass+1))
else
    echo "  FAIL coro_sm_contract    ->  $(head -2 "$tmp/e" | tr '\n' ' ')"; fail=$((fail+1))
fi
rm -rf "$tmp"
echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ] || exit 1
