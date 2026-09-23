#!/usr/bin/env bash
# tests/fs-shape/run.sh —— **`std::fs` 命名规范（定案 77）的常设验收**
#
# ⚠️ 为什么要有一支验收：这条规范不是"口味"，是**编译期判据** ——
#    主人 2026-09-23：「**open 不够清晰，因为我不知道打开的是读还是写**」✗
#    定的规范：`openRead` / `openWrite`（截断）/ `openAppend`（追加），
#    而且**读型和写型是两个 struct** ⇒ 误用**编不过** ✓
#
# 判据三条：
#   ① 正例跑通：三个名字都能用，而且**行为对**（截断 vs 追加要真的不一样 ✓）
#   ② 反例挡住：写型当读型用 / 读型当写型用 ⇒ **编译期**报错（不是运行时 ✗）
#   ③ 平台常量不外露：`O_*` 只能出现在 `std::sys::io` 那层
set -u
cd "$(dirname "$0")/../.."

EXTC=./build/extc
fail=0

echo "== 正例（三个名字 · 行为要对：截断 vs 追加）=="
if out=$("$EXTC" --run tests/fs-shape/fsproto.extc -I tests/fs-shape 2>&1); then
    ok=1
    echo "$out" | grep -qF "读到 13 字节"   || ok=0      # "hello\n"(6) + "世界\n"(7)
    echo "$out" | grep -qF "追加之后 = 19 字节" || ok=0   # 13 + "again\n"(6)
    echo "$out" | grep -qF "fs 形状原型 ✓ 跑通了" || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   fsproto  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL fsproto  ->  行为不对：$(echo "$out" | tr '\n' '|')"; fail=1
    fi
else
    echo "  FAIL fsproto  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 反例（**误用必须编不过** —— 这条规范的全部价值就在这里）=="
check_err() {   # check_err 文件 消息里必须出现的关键字
    local f=$1 want=$2 out
    if out=$("$EXTC" "tests/fs-shape/errors/$f.extc" -I tests/fs-shape -o /dev/null 2>&1); then
        echo "  FAIL $f  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    if ! echo "$out" | grep -qF -- "$want"; then
        echo "  FAIL $f  ->  消息里没有「$want」"; echo "$out" | sed 's/^/        /' | head -3; fail=1; return
    fi
    echo "  ok   $f  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err write-as-input  "argument expects \`inputFile\`, found \`outputFile\`"      # 写型当读型用
check_err read-as-output  "no method \`put\`"           # 读型当写型用

echo "== 平台常量不外露（\`O_*\` 只在特权层）=="
# std::fs（将来的那层）与用户层都不该出现 O_，只有 std::sys::io 能定义它们 ✓
# ⚠️ 只看**代码行**：注释里当然会提到 `O_WRONLY` —— 那正是在解释"为什么不让用户看见" ✓
codeHasO() { grep -v '^[[:space:]]*[*#/]' "$1" | grep -q 'io::O_\|^let O_' ; }
if grep -q '^let O_' stdlib/std/sys/io.extc && ! codeHasO tests/fs-shape/fsproto.extc; then
    echo "  ok   O_*   ->  只在 std::sys::io 定义；用户层代码里一个 O_ 都没有 ✓"
else
    echo "  FAIL O_*  ->  平台常量漏到用户层代码里了 ✗"; fail=1
fi

echo "== 三个名字的规范（\`SYNTAX.md\` §3′ 的判据）=="
if grep -q 'openRead' SYNTAX.md && grep -q 'openAppend' SYNTAX.md && grep -q 'inputFile' SYNTAX.md && grep -q '定案 77' SYNTAX.md; then
    echo "  ok   规范  ->  SYNTAX.md §3′（定案 77）写着三个名字 + 两个类型 ✓"
else
    echo "  FAIL 规范  ->  SYNTAX.md §3′ 没写（规范不落文档就没人遵守 ✗）"; fail=1
fi

rm -f build/fsproto-out.txt
exit $fail
