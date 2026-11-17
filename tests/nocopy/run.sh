#!/usr/bin/env bash
# tests/nocopy/run.sh —— **@noCopy 的常设验收**（定案 88 第 2 步）
#
# 判据：
#   ① 正例：造新值 / 按 ref 传 / 读字段 / 调方法 —— 这四件事**必须**照旧成立 ✓
#      （规则若把"造一个新值"也挡了，这个注解就没法用了 ✗）
#   ② 四个复制点全覆盖：绑定 · 实参 · 字段初始化 · 赋值 ✓
#      （四处各一个反例文件 —— 一个文件里只报第一条，所以必须分开写 ✓）
#   ③ 诊断口径：说清"它是 @noCopy"，并教"改成 ref / mut ref" ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（新值 · ref 传参 · 读字段 · 调方法）=="
if out=$("$EXTC" --run tests/nocopy/basic.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/nocopy/basic.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   basic  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL basic  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL basic  ->  编不过（正例必须过 ✗）"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 正例（移动：绑定把值交出去；赋新值后名字重新可用）=="
if out=$("$EXTC" --run tests/nocopy/move_bind.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/nocopy/move_bind.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   move_bind  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL move_bind  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL move_bind  ->  编不过（移动必须通过 ✗）"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 正例（移动进字段）=="
if out=$("$EXTC" --run tests/nocopy/move_into_field.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/nocopy/move_into_field.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   move_into_field  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL move_into_field  ->  输出里缺「$want」"; fail=1; fi
else
    echo "  FAIL move_into_field  ->  编不过（移动必须通过 ✗）"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 正例（移动进实参：按值传 = 交出所有权）=="
if out=$("$EXTC" --run tests/nocopy/move_arg.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/nocopy/move_arg.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   move_arg  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL move_arg  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL move_arg  ->  编不过（移动必须通过 ✗）"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 反例（其余复制点 + 移动后使用，逐个隔离）=="
for f in arg_twice assign_copy use_after_move use_after_move_field bind_field_copy bind_elem_copy; do
    if out=$("$EXTC" "tests/nocopy/errors/$f.extc" -o /dev/null 2>&1); then
        echo "  FAIL $f  ->  **编过了**（拷贝必须挡住 ✗）"; fail=1; continue
    fi
    if [ "$f" = use_after_move ] || [ "$f" = use_after_move_field ] || [ "$f" = arg_twice ]; then
        if ! echo "$out" | grep -qF 'was moved into'; then
            echo "  FAIL $f  ->  消息不是"已移动"口径"; echo "$out" | head -2 | sed 's/^/        /'; fail=1; continue
        fi
        echo "  ok   $f  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-78)"; continue
    fi
    if ! echo "$out" | grep -qF 'is `@noCopy`: it may not be copied by value'; then
        echo "  FAIL $f  ->  消息不是 @noCopy 口径"; echo "$out" | head -2 | sed 's/^/        /'; fail=1; continue
    fi
    echo "  ok   $f  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-78)"
done

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
