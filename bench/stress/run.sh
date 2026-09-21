#!/usr/bin/env bash
# 压测：extC 与 C **同一个种子、同一份随机序列**（负载完全等价）⇒ 结果必须逐位相同 ✓
# 量：时间 + 峰值 RSS。⚠️ RSS 差异往往是**分配策略**差异，不是"慢" ✓
set -u
cd "$(dirname "$0")/../.."
mkdir -p build/stress
gcc -O2 bench/stress/stress.c -o build/stress/c
./build/extc bench/stress/stress.extc -o build/stress/e.c >/dev/null
gcc -O2 build/stress/e.c -o build/stress/extc
for v in extc c; do
    out=$( { /usr/bin/time -f "TIME %e RSS %M" "build/stress/$v"; } 2>&1 )
    echo "--- $v ---"
    echo "$out" | grep -E "^[①②③④]"
    echo "$out" | grep -E "^TIME"
done
