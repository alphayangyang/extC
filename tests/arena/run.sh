#!/usr/bin/env bash
# A2（按块细化）的验收：**循环里分配，内存不该涨**。
#
# 做法：把虚拟内存卡在 150MB，再跑一个"循环里每次分配 1MB × 300 次"的程序。
#   · 每帧 arena  ⇒ 累积到 300MB ⇒ `out of arena memory`（现在的行为）
#   · 每作用域    ⇒ 每轮回收 ⇒ 正常跑完 ✓
#
# ⚠️ A2 做完之前这里**预期失败**，所以暂时没有并进 tests/run.sh ✓
set -u
cd "$(dirname "$0")/../.."

out=$( ( ulimit -v 150000; ./build/extc --run tests/arena/loop-growth.extc ) 2>&1 )
if echo "$out" | grep -q "跑完了"; then
    echo "  ok   loop-growth  ->  $out"
    exit 0
fi
echo "  FAIL loop-growth  ->  $(echo "$out" | head -1)"
echo "       （A2 未完成：循环里分配会累积到函数返回）"
exit 1
