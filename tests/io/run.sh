#!/usr/bin/env bash
# tests/io/run.sh —— **IO 的第一块（定案 73）的常设验收**
#
# 判据：① `use std::io` 能从 **stdin** 读（这一条是里程碑的门槛 ✓）
#       ② 解析 + 计算 + 回显都对（逐字节比输出 ✓）
#       ③ `std::sys` 只管原语、`std::io` 是普通库（模块分层真的成立 ✓）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（从 stdin 读两行：算和 + 回显）=="
if out=$(printf '5 7\nhello world\n' | "$EXTC" --run tests/io/main.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "求和 = 12" || ok=0
    echo "$out" | grep -qF "回显：hello world" || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   read-stdin  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL read-stdin  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL read-stdin  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 分层（std::sys = 特权层，std::io = 普通库）=="
# ⚠️ 这是**结构检查**，不是编译器强制的（"只有特权模块能声明原语"那条还没做 ✗ 见定案 72）
if grep -q '^extern!' stdlib/std/sys.extc && ! grep -q '^fn main' stdlib/std/sys.extc; then
    echo "  ok   std::sys  ->  只有它声明 C 原语（+ 签字），没有 main ✓"
else
    echo "  FAIL std::sys  ->  形状不对 ✗"; fail=1
fi
if grep -q '^use std::sys' stdlib/std/io.extc && ! grep -q '^extern!' stdlib/std/io.extc; then
    echo "  ok   std::io   ->  普通库（自己不碰 extern，只用 std::sys ✓）"
else
    echo "  FAIL std::io   ->  形状不对 ✗"; fail=1
fi

exit $fail
