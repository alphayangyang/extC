#!/usr/bin/env bash
# tests/zone/run.sh —— 位置（zone）判定的常设验收：**同类问题只有一种答案**
#
# 判据：
#   ① 循环里扩容之后，同层再取视图必须能过（假阳性回归，`grow_view`）✓
#   ② 反方向不许放宽：循环里 `new` 出来的缓冲区视图、循环里声明的定长数组视图、
#     存进外层位置必须**在检查期**被拒 ✓
#   ③ 被放弃的句柄任务在块退出时结束：活任务数回到 0（`handle_abandon`）✓
#
# 背景：`promoteInto2` 里"能不能按目的地降位置层级"曾经是手写 kind 枚举，`a + b` 漏在里面，
# 于是 method 写法能过、运算符写法报 "borrowed from depth 4"（提交 14093e6 修好，
# `tools/check_call_sites.py` 防止再长出第二份枚举）。池板块退场之后，这一层只剩位置判定本身。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 循环里扩容之后，同层再取视图必须能过（假阳性回归）=="
if out=$("$EXTC" --run tests/zone/grow_view.extc 2>&1); then
    want=$(grep -o "// expect:.*" tests/zone/grow_view.extc | sed "s|// expect: *||" | head -1)
    ok=1
    for p in ${want//|/ }; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   grow_view       ->  $(echo "$out" | tr "\n" "|")"
    else echo "  FAIL grow_view       ->  输出里缺「$want」（$(echo "$out" | tr "\n" "|")）"; fail=1; fi
else
    echo "  FAIL grow_view       ->  编不过（深度误拒回归？）"; echo "$out" | head -4 | sed "s/^/        /"; fail=1
fi

echo "== 反例：循环内的存储视图 / 循环局部容器读出的值，都不得存进外层位置 =="
for f in tests/zone/neg_loop_new_view.extx tests/zone/neg_loop_array_view.extx; do
    want=$(grep -o "// expect:.*" "$f" | sed "s|// expect: *||" | head -1)
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; continue
    fi
    if echo "$out" | grep -qF -- "$want"; then
        echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 "error:" | cut -c1-84)"
    else
        echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; echo "$out" | head -4 | sed "s/^/        /"; fail=1
    fi
done

echo "== 包装句柄被放弃时，块退出结束它看得见的任务 =="
# 句柄被丢弃（只 next() 一次、或 for 循环 break）时，块退出会结束编译器叫得出名字的那些任务
# （`cgCoroOwnedEndAt`）。判据：50 个被放弃的句柄任务，运行期的活任务数回到 0。
ha=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$ha/h.c" tests/zone/handle_abandon.extc >/dev/null 2>&1 \
   && cc -O2 -std=c11 -fwrapv -o "$ha/h" "$ha/h.c" 2>"$ha/e"; then
    out=$("$ha/h" 2>&1)
    if [ "$out" = "before=0 after=0" ]; then
        echo "  ok   handle_abandon     ->  50 个被放弃的句柄任务：活任务数 0→0（块退出结束自己能看见的任务）"
    else
        echo "  FAIL handle_abandon     ->  输出=$out（期望 before=0 after=0：不逃逸的句柄必须结束任务）"
        fail=1
    fi
else
    echo "  FAIL handle_abandon     ->  $(head -2 "$ha/e" | tr "\n" " ")"; fail=1
fi
rm -rf "$ha"

if [ "$fail" = 0 ]; then echo "  zone: 通过"; else echo "  zone: 失败"; fi
exit "$fail"