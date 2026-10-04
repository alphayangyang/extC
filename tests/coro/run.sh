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

# N 个活任务的内存：10000 个挂起的协程 ≤ 8 MB 常驻（曾经是 41.8 MB —— 每块下限 4096 字节）
mm=$tmp/coro_mem
if "$EXTC" -w --no-line-map -o "$mm.c" tests/coro/coro_mem.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$mm" "$mm.c" 2>"$tmp/mme"; then
    /usr/bin/time -q -f "%M" -o "$tmp/mm.rss" "$mm" >/dev/null 2>&1; rc=$?
    peak=$(cat "$tmp/mm.rss" 2>/dev/null)
    if [ "$rc" = 0 ] && [ -n "$peak" ] && [ "$peak" -le 8192 ]; then
        echo "  ok   coro_mem            ->  10000 个活任务：常驻 ${peak} KB（≤ 8192），全部活着"
        pass=$((pass+1))
    else
        echo "  FAIL coro_mem            ->  退出码 $rc，常驻 ${peak:-?} KB（上限 8192）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_mem            ->  $(head -2 "$tmp/mme" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 块里 new + 挂起 + 恢复后用（规则③在协程里已取消）。期望 6 + ASan 干净
nb=$tmp/coro_new_in_block
if "$EXTC" -w --no-line-map -o "$nb.c" tests/coro/coro_new_in_block.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$nb" "$nb.c" 2>"$tmp/nbe"; then
    "$nb"; rc=$?
    if [ "$rc" = 6 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$nb.asan" "$nb.c" 2>/dev/null; then
            aout=$("$nb.asan" 2>&1); arc=$?
            if [ "$arc" = 6 ] && [ -z "$aout" ]; then
                echo "  ok   coro_new_in_block   ->  块里 new + 挂起 + 恢复后用（6），ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_new_in_block   ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_new_in_block   ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_new_in_block   ->  退出码 $rc（期望 6）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_new_in_block   ->  $(head -2 "$tmp/nbe" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 协程里 `new`（跨挂起使用）+ ASan：帧携带的块 arena 就是为此。期望 4
nw=$tmp/coro_new
if "$EXTC" -w --no-line-map -o "$nw.c" tests/coro/coro_new.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$nw" "$nw.c" 2>"$tmp/nwe"; then
    "$nw"; rc=$?
    if [ "$rc" = 4 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$nw.asan" "$nw.c" 2>/dev/null; then
            aout=$("$nw.asan" 2>&1); arc=$?
            if [ "$arc" = 4 ] && [ -z "$aout" ]; then
                echo "  ok   coro_new            ->  协程里 new 的存储跨挂起活着（4），ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_new            ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_new            ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_new            ->  退出码 $rc（期望 4）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_new            ->  $(head -2 "$tmp/nwe" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 长任务：协程里每轮分配 64 KB × 20000 轮（1.28 GB 的分配量）⇒ 常驻必须有界，块释放真的生效
al=$tmp/coro_alloc_loop
if "$EXTC" -w --no-line-map -o "$al.c" tests/coro/coro_alloc_loop.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$al" "$al.c" 2>"$tmp/ale"; then
    /usr/bin/time -q -f "%M" -o "$tmp/al.rss" "$al" >/dev/null 2>&1; rc=$?
    peak=$(cat "$tmp/al.rss" 2>/dev/null)
    if [ "$rc" = 32 ] && [ -n "$peak" ] && [ "$peak" -le 32768 ]; then
        echo "  ok   coro_alloc_loop     ->  每轮 64 KB × 20000 ⇒ 常驻 ${peak} KB（≤ 32768）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_alloc_loop     ->  退出码 $rc，常驻 ${peak:-?} KB（期望 32 / ≤ 32768）"
        fail=$((fail+1))
    fi
else
    echo "  FAIL coro_alloc_loop     ->  $(head -2 "$tmp/ale" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 规则④：复制句柄内存安全（两个副本一个驱动一个读）。期望 (0+1+2)×2 = 6
cp=$tmp/coro_copy
if "$EXTC" -w --no-line-map -o "$cp.c" tests/coro/coro_copy.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$cp" "$cp.c" 2>"$tmp/cpe"; then
    "$cp"; rc=$?
    if [ "$rc" = 6 ]; then
        echo "  ok   coro_copy           ->  复制句柄：一个驱动一个读，内存安全（退出码 6）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_copy           ->  退出码 $rc（期望 6）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_copy           ->  $(head -2 "$tmp/cpe" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 规则②：驱动**过期**句柄（副本）必须大声 trap —— 退出码 70 且 stderr 带源码位置
ce=$tmp/coro_copy_expired
if "$EXTC" -w --no-line-map -o "$ce.c" tests/coro/coro_copy_expired.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ce" "$ce.c" 2>"$tmp/cee"; then
    cerr=$("$ce" 2>&1 >/dev/null); rc=$?
    if [ "$rc" = 70 ] && echo "$cerr" | grep -q "trap:.*task has already ended"; then
        echo "  ok   coro_copy_expired   ->  过期副本驱动 ⇒ 大声 trap（70 + 位置）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_copy_expired   ->  退出码 $rc，stderr[$cerr]"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_copy_expired   ->  $(head -2 "$tmp/cee" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 规则②的另一半：`value()` 也必须验活（曾经直接读已回收的帧 = use-after-free）
ve=$tmp/coro_value_expired
if "$EXTC" -w --no-line-map -o "$ve.c" tests/coro/coro_value_expired.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ve" "$ve.c" 2>"$tmp/vee"; then
    verr=$("$ve" 2>&1 >/dev/null); rc=$?
    if [ "$rc" = 70 ] && echo "$verr" | grep -q "trap:.*task has already ended"; then
        echo "  ok   coro_value_expired  ->  过期句柄读 value() ⇒ 大声 trap（70 + 位置）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_value_expired  ->  退出码 $rc，stderr[$verr]"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_value_expired  ->  $(head -2 "$tmp/vee" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 装箱点：字段 / 元素 / 已存在句柄变量三处（欠账第 1 条的补验）。期望 4
bx=$tmp/coro_boxing
if "$EXTC" -w --no-line-map -o "$bx.c" tests/coro/coro_boxing.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$bx" "$bx.c" 2>"$tmp/bxe"; then
    "$bx"; rc=$?
    if [ "$rc" = 4 ]; then
        echo "  ok   coro_boxing        ->  装进字段/元素/已有句柄变量都成立（退出码 4）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_boxing        ->  退出码 $rc（期望 4）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_boxing        ->  $(head -2 "$tmp/bxe" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 流式 echo server（TCP loopback）：多批数据 + 部分写补齐 + 半关收尾 + close。期望 50 + ASan 干净
ec=$tmp/coro_echo
if "$EXTC" -w --no-line-map -o "$ec.c" tests/coro/coro_echo.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$ec" "$ec.c" 2>"$tmp/ece"; then
    /usr/bin/time -q -f "%M" -o "$tmp/ec.rss" "$ec" >/dev/null 2>&1; rc=$?
    peak=$(cat "$tmp/ec.rss" 2>/dev/null)
    if [ "$rc" = 50 ] && [ -n "$peak" ] && [ "$peak" -le 16384 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -O2 -o "$ec.asan" "$ec.c" 2>/dev/null; then
            aout=$("$ec.asan" 2>&1); arc=$?
            if [ "$arc" = 50 ] && [ -z "$aout" ]; then
                echo "  ok   coro_echo           ->  流式回显两批数据 + 半关收尾 + close（50），常驻 ${peak} KB，ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_echo           ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_echo           ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_echo           ->  退出码 $rc（期望 50；90=有任务没完，91=回声缺，9x=连接失败），常驻 ${peak:-?} KB"
        fail=$((fail+1))
    fi
else
    echo "  FAIL coro_echo           ->  $(head -2 "$tmp/ece" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 可增长任务表（逃逸规则精度）：泛型容器包着 vector，方法里往表里 push 句柄。期望 41
tt=$tmp/coro_table
if "$EXTC" -w --no-line-map -o "$tt.c" tests/coro/coro_table.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -O2 -o "$tt" "$tt.c" 2>"$tmp/tte"; then
    "$tt" >/dev/null 2>&1; rc=$?
    if [ "$rc" = 41 ]; then
        echo "  ok   coro_table          ->  泛型容器包 vector + 方法里 push 句柄（41）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_table          ->  退出码 $rc（期望 41）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_table          ->  $(head -2 "$tmp/tte" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 一个 listener accept 出 N=40 条连接（容量 32 ⇒ 必须复用槽位）。期望 52 + ASan 干净
ac=$tmp/coro_accept
if "$EXTC" -w --no-line-map -o "$ac.c" tests/coro/coro_accept.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ac" "$ac.c" 2>"$tmp/ace"; then
    /usr/bin/time -q -f "%M" -o "$tmp/ac.rss" "$ac" >/dev/null 2>&1; rc=$?
    peak=$(cat "$tmp/ac.rss" 2>/dev/null)
    if [ "$rc" = 52 ] && [ -n "$peak" ] && [ "$peak" -le 16384 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$ac.asan" "$ac.c" 2>/dev/null; then
            aout=$("$ac.asan" 2>&1); arc=$?
            if [ "$arc" = 52 ] && [ -z "$aout" ]; then
                echo "  ok   coro_accept         ->  listener accept 40 条（跑完的行就地复用），常驻 ${peak} KB，ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_accept         ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_accept         ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_accept         ->  退出码 $rc（期望 52；93=回声缺，94=没accept够，95=槽位没复用）"
        fail=$((fail+1))
    fi
else
    echo "  FAIL coro_accept         ->  $(head -2 "$tmp/ace" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 真事件源：N=8 条 AF_UNIX 连接、单线程、一个 epoll 循环。期望 36，并要求 ASan 干净
ep=$tmp/coro_epoll
if "$EXTC" -w --no-line-map -o "$ep.c" tests/coro/coro_epoll.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ep" "$ep.c" 2>"$tmp/epe"; then
    "$ep"; rc=$?
    if [ "$rc" = 36 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$ep.asan" "$ep.c" 2>/dev/null; then
            aout=$("$ep.asan" 2>&1); arc=$?
            if [ "$arc" = 36 ] && [ -z "$aout" ]; then
                echo "  ok   coro_epoll          ->  8 条连接单线程跑完 epoll 循环（36），ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_epoll          ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_epoll          ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_epoll          ->  退出码 $rc（期望 36）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_epoll          ->  $(head -2 "$tmp/epe" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 调度器：一组句柄 + 脚本化事件源（确定性），交替推进两个任务。期望 36
sc=$tmp/coro_sched
if "$EXTC" -w --no-line-map -o "$sc.c" tests/coro/coro_sched.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$sc" "$sc.c" 2>"$tmp/sce"; then
    "$sc"; rc=$?
    if [ "$rc" = 36 ]; then
        echo "  ok   coro_sched          ->  调度器握住一组句柄按事件源推进（退出码 36）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_sched          ->  退出码 $rc（期望 36）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_sched          ->  $(head -2 "$tmp/sce" | tr '\n' ' ')"; fail=$((fail+1))
fi

# `send`：驱动器送进去的值，协程在 `var x = yield e` 的绑定里收到。期望 17
sd=$tmp/coro_send
if "$EXTC" -w --no-line-map -o "$sd.c" tests/coro/coro_send.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$sd" "$sd.c" 2>"$tmp/sde"; then
    "$sd"; rc=$?
    if [ "$rc" = 17 ]; then
        echo "  ok   coro_send          ->  yield 收值：send(7) 真的被收到（退出码 17）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_send          ->  退出码 $rc（期望 17）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_send          ->  $(head -2 "$tmp/sde" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 统一句柄：coroutine<T> 是普通存储类型，标注处装箱（帧进任务 place），句柄穿过函数边界驱动。
# 期望 99/9 = 11；并要求 ASan 干净。
ch=$tmp/coro_handles
if "$EXTC" -w --no-line-map -o "$ch.c" tests/coro/coro_handles.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ch" "$ch.c" 2>"$tmp/che"; then
    "$ch"; rc=$?
    if [ "$rc" = 11 ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$ch.asan" "$ch.c" 2>/dev/null; then
            aout=$("$ch.asan" 2>&1); arc=$?
            if [ "$arc" = 11 ] && [ -z "$aout" ]; then
                echo "  ok   coro_handles       ->  句柄进容器并穿过函数边界：跑出 11，ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_handles       ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_handles       ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_handles       ->  退出码 $rc（期望 11）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_handles       ->  $(head -2 "$tmp/che" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 零分配：不外逃的协程**不该**碰任务表（帧留在调用者栈上）
zd=$tmp/zeroalloc
if "$EXTC" -w --no-line-map -o "$zd.c" tests/coro/coro_drive.extc >/dev/null 2>&1; then
    if grep -q "extc_task_begin\|extc_task_alloc" "$zd.c"; then
        echo "  FAIL coro_zero_alloc    ->  不外逃的协程也碰了任务表"; fail=$((fail+1))
    else
        echo "  ok   coro_zero_alloc    ->  不外逃 ⇒ 生成物里没有 extc_task_begin/alloc（零分配）"
        pass=$((pass+1))
    fi
else
    echo "  FAIL coro_zero_alloc    ->  生成失败"; fail=$((fail+1))
fi

# 切片 C 验收项：**体顶层建池（vector）、跨挂起点继续用** ✓ —— 池的 plate 生在任务自己的 place 里，
# 活过每一次挂起；任务跑完一次性回收。三件事都要成立：接受 ✓ 跑对（0+1+2+3=6）✓ ASan 干净 ✓
cpo=$tmp/coro_pool
if "$EXTC" -w --no-line-map -o "$cpo.c" tests/coro/coro_pool.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$cpo" "$cpo.c" 2>"$tmp/cpec"; then
    out=$("$cpo"); rc=$?
    if [ "$rc" = 6 ] && [ -z "$out" ]; then
        if gcc -std=c11 -fwrapv -g -fsanitize=address -o "$cpo.asan" "$cpo.c" 2>/dev/null; then
            aout=$("$cpo.asan" 2>&1); arc=$?
            if [ "$arc" = 6 ] && [ -z "$aout" ]; then
                echo "  ok   coro_pool          ->  体顶层建池跨挂起点：跑出 6，ASan 干净"
                pass=$((pass+1))
            else
                echo "  FAIL coro_pool          ->  ASan：退出码 $arc [$aout]"; fail=$((fail+1))
            fi
        else
            echo "  FAIL coro_pool          ->  ASan 编译失败"; fail=$((fail+1))
        fi
    else
        echo "  FAIL coro_pool          ->  输出 [$out] 退出码 $rc"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_pool          ->  $(head -2 "$tmp/cpec" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 任务表（src/coroutine.c）：extC 侧看得见，登记与释放都对。退出码 10 = before0 · mid1 · after0
ct=$tmp/coro_tasks
if "$EXTC" -w --no-line-map -o "$ct.c" tests/coro/coro_tasks.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$ct" "$ct.c" 2>"$tmp/cte"; then
    "$ct"; rc=$?
    if [ "$rc" = 10 ]; then
        echo "  ok   coro_tasks         ->  extC 侧读到任务表：before=0 mid=1 after=0（退出码 10）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_tasks         ->  退出码 $rc（期望 10）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_tasks         ->  $(head -2 "$tmp/cte" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 切片 B2a：spawn + 驱动（`while c.next() { c.value() }`）**端到端跑起来** ✓
b2=$tmp/coro_drive
if "$EXTC" -w --no-line-map -o "$b2.c" tests/coro/coro_drive.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -Wall -Werror -o "$b2" "$b2.c" 2>"$tmp/b2c"; then
    out=$("$b2"); rc=$?
    if [ "$out" = "0 1 2 | 0 1 2 3 " ] && [ "$rc" = 9 ]; then
        echo "  ok   coro_drive         ->  spawn + next/value 与 for-in 都跑出 [$out] 退出码 $rc（3+6=9 与 yield 次数一致）"
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
# 注意：脚本前段已经 `rm -rf "$tmp"` 过一次（那是它原来的收尾），这里要把目录建回来
mkdir -p "$tmp"
# `coroutine<A, B>`：请求与应答类型不同（A = i64 由 send 送入，B = f64 由 yield 交出）
rr=$tmp/coro_req_resp
if "$EXTC" -w --no-line-map -o "$rr.c" tests/coro/coro_req_resp.extc 2>"$tmp/rre" \
   && gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$rr" "$rr.c" 2>"$tmp/rre"; then
    "$rr" >"$tmp/rro" 2>&1; rc=$?
    if [ $rc -eq 25 ]; then
        echo "  ok   coro_req_resp     ->  请求 i64 / 应答 f64：2.5 ⇒ 退出码 25，打印 $(cat "$tmp/rro")"
        pass=$((pass+1))
    else
        echo "  FAIL coro_req_resp     ->  退出码 $rc（期望 25）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_req_resp     ->  $(head -2 "$tmp/rre" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1))
fi

# `coroutine<T>` 与 `coroutine<T, T>` 等价（前者是"请求与应答同型"的简写）
sh=$tmp/coro_shorthand
if "$EXTC" -w --no-line-map -o "$sh.c" tests/coro/coro_shorthand.extc 2>"$tmp/she" \
   && gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$sh" "$sh.c" 2>"$tmp/she"; then
    "$sh" >/dev/null 2>&1; rc=$?
    if [ $rc -eq 17 ]; then
        echo "  ok   coro_shorthand     ->  coroutine<i64> 与 coroutine<i64, i64> 结果相同（退出码 17）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_shorthand     ->  退出码 $rc（期望 17）"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_shorthand     ->  $(head -2 "$tmp/she" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1))
fi

# L1：**定时等待**（sleep）。200 个任务各睡 1..5ms：全部醒、无残留定时登记、真的等了。
nsl=$tmp/coro_sleep
if "$EXTC" -w --no-line-map -o "$nsl.c" tests/coro/coro_sleep.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -O2 -o "$nsl" "$nsl.c" 2>"$tmp/nsl.e"; then
    out=$("$nsl" 2>&1); rc=$?
    if [ "$rc" = 200 ] && echo "$out" | grep -q 'coro_sleep ok'; then
        echo "  ok   coro_sleep          ->  $(echo "$out" | tr '\n' '|')"
        pass=$((pass+1))
    else
        echo "  FAIL coro_sleep          ->  退出码 $rc：$(echo "$out" | tr '\n' '|')"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_sleep          ->  $(head -2 "$tmp/nsl.e" | tr '\n' ' ')"; fail=$((fail+1))
fi

# 等可写（WAIT_FD_W）：TLS 的 WANT_WRITE 没有它就等错方向。
nww=$tmp/coro_waitw
if "$EXTC" -w --no-line-map -o "$nww.c" tests/coro/coro_waitw.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -O2 -o "$nww" "$nww.c" 2>"$tmp/nww.e"; then
    out=$("$nww" 2>&1); rc=$?
    if [ "$rc" = 200 ] && echo "$out" | grep -q 'coro_waitw ok'; then
        echo "  ok   coro_waitw          ->  $(echo "$out" | tr '\n' '|')"
        pass=$((pass+1))
    else
        echo "  FAIL coro_waitw          ->  退出码 $rc：$(echo "$out" | tr '\n' '|')"; fail=$((fail+1))
    fi
else
    echo "  FAIL coro_waitw          ->  $(head -2 "$tmp/nww.e" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1))
fi

# 字面量切片的深度：**正例必须过、反例必须仍被拒**（放宽规则时反例比正例重要）
nlt=$tmp/coro_literal
if "$EXTC" -w --run tests/coro/coro_literal.extc >"$tmp/lit.out" 2>&1 && grep -q 'literal ok' "$tmp/lit.out"; then
    echo "  ok   coro_literal        ->  字面量切片跨 yield 合法（$(grep -c 'hello from a literal' "$tmp/lit.out") 行输出）"
    pass=$((pass+1))
else
    echo "  FAIL coro_literal        ->  $(tail -2 "$tmp/lit.out" | tr '\n' '|')"; fail=$((fail+1))
fi
cat > "$tmp/neg.extc" <<'NEGEOF'
use std::coro::scheduler as sched
fn w(fd: i64, s: mut ref sched::loop) -> coroutine<i64> {
    var arr: [8]u8
    let v: slice<u8> = arr[0..8]
    yield fd
    yield i64(-1) - i64(v.len)
}
fn main() -> i32 { return 0 }
NEGEOF
if out=$("$EXTC" -w --no-line-map -o "$tmp/neg.c" "$tmp/neg.extc" 2>&1); then
    echo "  FAIL coro_literal_neg    ->  **局部数组的视图跨 yield 编过了**（应当被拒）"; fail=$((fail+1))
else
    if echo "$out" | grep -q 'lives across a `yield`'; then
        echo "  ok   coro_literal_neg    ->  局部数组的视图跨 yield 仍被拒（反例守住）"
        pass=$((pass+1))
    else
        echo "  FAIL coro_literal_neg    ->  报错信息不对：$(echo "$out" | head -1)"; fail=$((fail+1))
    fi
fi

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ] || exit 1
