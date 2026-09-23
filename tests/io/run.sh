#!/usr/bin/env bash
# tests/io/run.sh —— **IO-0（定案 73 + 定案 74）的常设验收**
#
# 判据：
#   ① `use std::io` 能从 **stdin** 读（这一条是里程碑的门槛 ✓）
#   ② 两种风格**混着用**都对：`nextInt`（OI 式）+ `nextLine`（协议式）✓
#   ③ **三条路分得开**：EOF / 行太长 / 读错误（老 `readLine` 把后两条都当成 EOF ✗）
#   ④ `std::sys::io` 只管原语、`std::io` 是普通库（模块分层真的成立 ✓）
#      ⚠️ `sys` 不是"一个模块"，是**一条边界**的路径写法 ⇒ **按族分文件**
#         （后面还有 std::sys::{thread,time,net,proc}，全塞一个文件会变垃圾场 ✗）
#   ⑤ **不是逐字节读**：一次 64KB（老实现 50 万行要 2.3s，是"能跑但慢 190 倍"那种坏 ✓）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（两种风格混用：nextInt + nextLine）=="
if out=$(printf '5 7\nhello world\n99' | "$EXTC" --run tests/io/main.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "求和 = 12"      || ok=0
    echo "$out" | grep -qF "回显：hello world" || ok=0
    echo "$out" | grep -qF "下一个 = 99"     || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   read-stdin  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL read-stdin  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL read-stdin  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 三条路（EOF / 行太长 / 读错误 —— 必须分得开）=="
# ① EOF ⇒ success(0)（**不是错误** ✓）
if out=$(printf '' | "$EXTC" --run tests/io/eof.extc 2>&1) && echo "$out" | grep -qF "EOF ✓"; then
    echo "  ok   eof          ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL eof          ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
# ② 行比缓冲长 ⇒ failure(lineTooLong)，**绝不许静默切一半** ✗
if out=$(printf 'ab\n0123456789012345678\n' | "$EXTC" --run tests/io/line-too-long.extc 2>&1) \
   && echo "$out" | grep -qF "lineTooLong ✓"; then
    echo "  ok   line-too-long ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL line-too-long ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
# ③ read(2) 出错 ⇒ failure(readFailed)，**绝不许当成 EOF** ✗
if out=$("$EXTC" --run tests/io/read-failed.extc 2>&1) && echo "$out" | grep -qF "readFailed ✓"; then
    echo "  ok   read-failed  ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL read-failed  ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 顺序（println 与 writeBytes 混用 —— 定案 75 修的真缺陷）=="
if out=$(printf '' | "$EXTC" --run tests/io/order.extc 2>&1) \
   && [ "$(printf '%s' "$out" | tr -d '\r')" = "A
B
C" ]; then
    echo "  ok   order        ->  A / B / C 顺序正确 ✓"
else
    echo "  FAIL order        ->  顺序乱了：$(printf '%s' "$out" | tr '\n' '|')"; fail=1
fi
# 一个**静态**判据：`writeBytes` 里必须真的先 flush（删掉它上面那条也会红 ✓）
if awk '/^fn writeBytes/,/^}/' stdlib/std/io.extc | grep -q 'flush()'; then
    echo "  ok   writeBytes  ->  里面有 flush()（顺序不可能错 ✓）"
else
    echo "  FAIL writeBytes  ->  少了 flush() ⇒ 与 println 混用会乱序 ✗"; fail=1
fi

echo "== writer（写文件 → 用 reader 读回，两边对称 ✓）=="
if out=$(printf '' | "$EXTC" --run tests/io/writer-file.extc 2>&1) && echo "$out" | grep -qF "回读 = 100/200 ✓"; then
    echo "  ok   writer-file  ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL writer-file  ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
rm -f tests/io/writer-tmp.txt

echo "== 分层（std::sys::io = 特权层 · io 族，std::io = 普通库）=="
# ⚠️ 这是**结构检查**，不是编译器强制的（"只有特权模块能声明原语"那条还没做 ✗ 见定案 72）
if grep -q '^extern!' stdlib/std/sys/io.extc && ! grep -q '^fn main' stdlib/std/sys/io.extc; then
    echo "  ok   std::sys::io  ->  只有它声明 C 原语（+ 签字），没有 main ✓"
else
    echo "  FAIL std::sys::io  ->  形状不对 ✗"; fail=1
fi
if grep -q '^use std::sys::io' stdlib/std/io.extc && ! grep -q '^extern!' stdlib/std/io.extc; then
    echo "  ok   std::io   ->  普通库（自己不碰 extern，只用 std::sys::io ✓）"
else
    echo "  FAIL std::io   ->  形状不对 ✗"; fail=1
fi

echo "== 不是逐字节读（老实现 50 万行 2.3s —— 这条抓"能跑但慢 190 倍" ✓）=="
# 10 万行 × 3 个数 ≈ 1.7MB。逐字节读要 ~0.5s 以上；分块读是毫秒级 ✓
BIG=$(mktemp)
i=0
while [ $i -lt 100000 ]; do printf '123 456 789\n'; i=$((i+1)); done > "$BIG"
start=$(date +%s%N)
out=$(printf '' | "$EXTC" --run tests/io/sum-big.extc < "$BIG" 2>&1)
end=$(date +%s%N)
ms=$(( (end - start) / 1000000 ))
rm -f "$BIG"
# 期望和 = 100000 * (123+456+789) = 136800000
if echo "$out" | grep -qF "sum = 136800000"; then
    if [ "$ms" -lt 400 ]; then
        echo "  ok   分块读  ->  10 万行 ${ms}ms（< 400ms ✓ 逐字节要 ~500ms 以上）"
    else
        echo "  FAIL 分块读  ->  10 万行花了 ${ms}ms ⇒ 像是退回逐字节了 ✗"; fail=1
    fi
else
    echo "  FAIL 大输入求和  ->  $(echo "$out" | tail -2 | tr '\n' '|')"; fail=1
fi

exit $fail
