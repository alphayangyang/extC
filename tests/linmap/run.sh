#!/usr/bin/env bash
# tests/linmap/run.sh —— **线性关联容器的常设验收**（`std::linmap` + `std::linset`）
#
# 为什么有这一节：它们回答的是「**不依赖 #57 能不能写泛型容器**」——
# 答案是能，只要键只需要 `==`（而 `==` 在类型参数上是支持的：实例化时检查）✓
# 于是「哈希版 map<K,V>」从"门槛"降级成"优化" ✓
#
# 这一节同时是**今天修好的三条缺陷的活判据**：
#   #60 返回「另一个泛型实例的 `?V`」· #61 泛型体里 match 裸变体 ·
#   #62 泛型体里 `slice<V>` 的变量下标（值本身是字符串那一格）
# ⚠️ 判据必须**编译生成的 C** ⇒ 一律 `--run`（`-o` 成功 ≠ 生成的 C 编得过 ✗）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

for t in map structkey set; do
    if ! out=$(timeout 60 "$EXTC" --run "tests/linmap/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -5; fail=1; continue
    fi
    want=$(grep -o '// expect:.*' "tests/linmap/$t.extc" | sed 's|// expect: *||' | head -1)
    if printf '%s' "$out" | grep -qF -- "$want"; then
        echo "  ok   $t  ->  $(printf '%s' "$out" | tr '\n' '|')"
    else
        echo "  FAIL $t  ->  输出对不上：要「$want」，得到「$(printf '%s' "$out" | tr '\n' '|')」"; fail=1
    fi
done

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
