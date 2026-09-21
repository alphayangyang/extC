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

fail=0
for f in tests/arena/*.extc; do
    name=$(basename "$f" .extc)
    # 每个用例都跑：循环里大量分配，只要有一处该释放没释放就会 out of arena memory
    out=$( ( ulimit -v 150000; ./build/extc --run "$f" ) 2>&1 | tr '\n' '|' )
    if echo "$out" | grep -q "out of arena memory"; then
        echo "  FAIL $name  ->  内存涨到超过 150MB（有块没释放）"
        echo "$out" | sed 's/^/        /'
        fail=1
    else
        echo "  ok   $name  ->  $out"
    fi
done
exit $fail
