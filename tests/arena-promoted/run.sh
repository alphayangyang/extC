#!/usr/bin/env bash
# 「**转正**」库（2026-09-23）：这些形状以前被**误拒**或**放行成 UB**，现在编译器
# 应该**接受**它们，而且 ASan 必须干净 ✓
#
# 来源：`docs/topics/ARENA-SOUNDNESS.md` §9 档 1（B2：`alloc<T>` 与 `new` 对称）
#   · `alloc_return_local` / `allocSlice_escape_return` —— 以前被误拒
#     （`alloc` 的层号无条件按块层算，而不看"有家 ⇒ 进家"）
#   · `A_field_root_lowered` / `C1/C2/C3_if_join_*` / `G_stale_origin` —— 以前放行成 UB，
#     第 1 步的记账修复把它们挡住（**误拒**），B2 之后 `alloc` 进了家 arena ⇒ 真的安全 ✓
#     这一条最值得记：**洞与误拒共用同一个出口**（§A1），B2 一做才分得清 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0
for f in tests/arena-promoted/*.extc; do
    n=$(basename "$f" .extc)
    if ! "$EXTC" "$f" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1; then
        echo "  FAIL $n  ->  被拒了（但它是安全程序）"; head -2 "$TMP/$n.cerr" | sed 's/^/        /'; fail=1; continue
    fi
    if ! gcc -O1 -g -fsanitize=address -o "$TMP/$n" "$TMP/$n.c" >"$TMP/$n.gerr" 2>&1; then
        echo "  FAIL $n  ->  生成的 C 编不过"; head -2 "$TMP/$n.gerr" | sed 's/^/        /'; fail=1; continue
    fi
    out=$("$TMP/$n" 2>&1)
    if echo "$out" | grep -q "AddressSanitizer"; then
        echo "  FAIL $n  ->  $(echo "$out" | grep -m1 -o 'ERROR: AddressSanitizer:.*')"; fail=1
    else
        echo "  ok   $n  ->  接受 + ASan 干净（$(echo "$out" | tr '\n' '|' | cut -c1-50)）"
    fi
done
exit $fail
