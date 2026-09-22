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

# ② 正例语料上**零警告**（误报判据）
noisy=""
for f in examples/*.extc bench/*/*.extc; do
    out=$("$EXTC" "$f" -o /dev/null 2>&1)
    if echo "$out" | grep -q "warning:"; then
        noisy="$noisy $f"
    fi
done
if [ -n "$noisy" ]; then
    echo "  FAIL 正例语料上有误报：$noisy"
    fail=1
else
    echo "  ok   正例语料（examples/ + bench/）**零警告** ⇒ 不是噪音 ✓"
fi

exit $fail
