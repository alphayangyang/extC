#!/usr/bin/env bash
# MEMORY-SAFETY.md 的**机器可复核**判据。
#
# 每个用例头部写着 `// expect: OK|REJECT|TRAP` 与一句说明 ⇒ 跑一遍就知道
# "文档表格说的"和"编译器实际做的"是否一致。
#   OK     = 编译通过、运行无 trap（或 TRAP 以外的正常输出）
#   REJECT = 编译期报错（必须带源码位置）
#   TRAP   = 编译通过，但运行时带位置地 trap
#
# 用法： bash tools/memsafe/run.sh          （全部）
#        bash tools/memsafe/run.sh a1       （只跑名字匹配的）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
for f in tools/memsafe/qa/*.extc; do
    name=$(basename "$f" .extc)
    [ -n "${1:-}" ] && case "$name" in *"$1"*) ;; *) continue ;; esac
    want=$(grep -m1 '^// expect:' "$f" | sed 's|^// expect: *||')
    desc=$(sed -n '2s|^// *||p' "$f")
    out=$(timeout 20 "$EXTC" --run "$f" 2>&1); rc=$?
    if echo "$out" | grep -qE "error:"; then got=REJECT
    elif [ $rc -eq 124 ]; then got=HANG
    elif echo "$out" | grep -q "trap:"; then got=TRAP
    else got=OK; fi
    if [ "$got" = "$want" ]; then
        printf '  ok   %-26s %-7s %s\n' "$name" "$got" "$desc"; pass=$((pass+1))
    else
        printf '  FAIL %-26s 期望 %s，实际 %s\n' "$name" "$want" "$got"
        echo "$out" | head -3 | sed 's/^/        /'
        fail=$((fail+1))
    fi
done
echo "通过 $pass，失败 $fail"
[ $fail -eq 0 ]
