#!/usr/bin/env bash
# **警告通道**的验收（2026-09-22 加）：
#   ① 该响的必须响（用例里写 `// expect-warning: <片段>`）
#   ② **不该响的一条都不许响** —— 拿整个正例语料（examples/ + bench/）扫一遍
#      （警告最容易变成噪音：宁可少一条，也不要每编译一次就糊一屏 ✗）
#   ③ 警告**不改变退出码**（编译照常成功 ✓）；`-w` 能全关 ✓
set -u
cd "$(dirname "$0")/../.."

EXTC=./build/extc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0

# ① 该响的
for f in tests/warnings/*.extc; do
    name=$(basename "$f" .extc)
    out=$("$EXTC" "$f" -o "$TMP/$name.c" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "  FAIL $name  ->  带警告的**正例**不该编译失败"
        echo "$out" | head -3 | sed 's/^/        /'
        fail=1; continue
    fi
    want=$(grep -o '// expect-warning:.*' "$f" | sed 's|// expect-warning: *||' | head -1)
    if [ -z "$want" ]; then
        echo "  FAIL $name  ->  用例里没写 // expect-warning"
        fail=1; continue
    fi
    if echo "$out" | grep -qF -- "$want"; then
        # ③ `-w` 必须能关掉
        if "$EXTC" -w "$f" -o /dev/null 2>&1 | grep -q "warning:"; then
            echo "  FAIL $name  ->  \`-w\` 没关掉警告"
            fail=1
        else
            echo "  ok   $name  ->  警告响了，\`-w\` 也关得掉 ✓"
        fi
    else
        echo "  FAIL $name  ->  没吐期望的警告「$want」"
        echo "$out" | head -3 | sed 's/^/        /'
        fail=1
    fi
done

# ④ **静默用例**：写死的形状一条警告都不许吐
#
# 为什么要有这一节：②拿语料当"误报判据"，可语料是**例子**，形状是碰巧覆盖到的 ——
# 2026-09-26 那一族误报（切片上下界 / 转换操作数 / 转换里的方法接收者）就是靠
# `examples/gomoku-board.extc` 之类**碰巧**才发现的 ✗。这里放的是**故意**写深的用法：
# 谁再漏走一格，这个目录立刻响 ✓ （比"等下一个例子碰巧踩到"早一整轮）
for f in tests/warnings/silent/*.extc; do
    [ -e "$f" ] || continue
    name=$(basename "$f" .extc)
    out=$("$EXTC" "$f" -o /dev/null 2>&1)
    if [ -z "$(echo "$out" | grep 'warning:')" ]; then
        echo "  ok   silent/$name  ->  一条警告都没吐 ✓"
    else
        echo "  FAIL silent/$name  ->  不该有警告：$(echo "$out" | grep 'warning:' | head -2 | tr '\n' '|')"
        fail=1
    fi
done

# ② 正例语料上**零警告**（误报判据）
# 这一段要把 examples/ + bench/ 全量编译一遍 —— 串行时是整个套件最贵的一段（实测 ~30s）✗
# 它没有断言、用例之间无依赖 ⇒ 交给 parrun.py 并行（保序、并发有界）✓
if out=$(python3 tools/parrun.py --mode warn-scan 2>&1); then
    echo "  ok   $out"
else
    echo "  FAIL $out"
    fail=1
fi

exit $fail
