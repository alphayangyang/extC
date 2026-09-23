#!/usr/bin/env bash
# tests/qname/run.sh —— **全限定名（PLAN #53）的常设验收**
#
# 判据（主人 2026-09-23 的原话定的口径）：
#   「为什么用户不能写 `std::sys::io::read` **这应该是被允许的**，
#     但是别名是必要的，否则就太长了，用 `as` 即可」
# ⇒ 两件事都要成立：**全名是权利**（任意层都能写）+ **别名是方便**（`as`）✓
#
# ⚠️ 为什么这七个用例一个都不能少（#53 失败过五轮，每一轮都是"改完发现另一处又坏了"）：
#   · `t11` 值位置的三段全名（常量）——最容易漏的一格
#   · `t12` 值位置的三段全名（**函数调用**，而且要真把值流出来）
#   · `t13` `as` 别名 + 调用
#   · `t14` **同一文件里 `std::io` 与 `std::sys::io` 共存**（这是这条缺口真正的痛点：
#           "从 stdin 读 + `open` 一个文件"恰好要这两层，而两边的短名都是 `io` ✗）
#   · `t15` 限定名到**深层模块**的**变体位置**（`lib::sub::color.color.green`）
#   · `t16` 限定名到**深层模块**的**函数调用**（前缀是多段，不是"第一个 `::` 之前"）
#   · `t17` 反例：没 `use` 的深层全名 ⇒ 必须**编译期**报错，而且指路要准
set -u
cd "$(dirname "$0")/../.."

EXTC=./build/extc
fail=0

echo "== 正例（全名是权利 · 别名是方便）=="
for f in tests/qname/t1[1-6].extc; do
    name=$(basename "$f" .extc)
    if ! out=$("$EXTC" --run "$f" -I tests/qname 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | sed 's/^/        /' | head -6; fail=1; continue
    fi
    # `// expect: a|b|c` —— 每一段都必须出现在输出里 ✓（跟 examples / modules 一个规矩）
    want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1)
    ok=1
    if [ -n "$want" ]; then
        IFS='|' read -ra parts <<< "$want"
        for p in "${parts[@]}"; do
            echo "$out" | grep -qF -- "$p" || ok=0
        done
    fi
    if [ "$ok" = 1 ]; then
        echo "  ok   $name  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL $name  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
done

echo '== 反例（深层全名没 use ⇒ 编译期报错，而且要说清 use 什么）=='
if out=$("$EXTC" tests/qname/errors/not-imported.extc -I tests/qname -o /dev/null 2>&1); then
    echo "  FAIL not-imported  ->  **编过了**（应该报错 ✗）"; fail=1
elif echo "$out" | grep -q "not imported here" && echo "$out" | grep -q 'use std::sys::io'; then
    echo "  ok   not-imported  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-88)"
else
    echo "  FAIL not-imported  ->  消息没指路：$(echo "$out" | head -2 | tr '\n' '|')"; fail=1
fi
# ⚠️ 消息必须报**全名**：报 `std` 的话用户照着写会 `use std`（一个不存在的模块 ✗）
if echo "$out" | grep -q 'add `use std`' && ! echo "$out" | grep -q 'use std::sys::io'; then
    echo "  FAIL not-imported  ->  指路指到了 `std`（路径前缀不是模块名 ✗）"; fail=1
fi

# ⚠️ 结构判据：`as` 别名必须**只换 shortName**、不许动 `path`
# （`path` 是"全名对全名"匹配的依据 —— 动它深层限定名就再也解不开了 ✗）
if grep -q 'u->shortName = alias ? alias : shortName;' src/parser.c; then
    echo "  ok   别名只换 shortName（path 保持全名 ✓）"
else
    echo "  FAIL 别名把 path 也改了 ⇒ 深层全名会解不开 ✗"; fail=1
fi

exit $fail
