#!/usr/bin/env bash
# 「**转正**」库（2026-09-23）：这些形状以前被**误拒**或**放行成 UB**，现在编译器
# 应该**接受**它们，而且 ASan 必须干净 ✓
#
# 这里只跑已经转正的 `.extc`；每个形状都必须被接受并在 ASan 下运行干净。
#
# 来源：`../extc-docs/archive/topics/ARENA-SOUNDNESS.md` §9 档 1（B2：`alloc<T>` 与 `new` 对称）
#   · `alloc_return_local` / `allocSlice_escape_return` —— 以前被误拒
#     （`alloc` 的层号无条件按块层算，而不看"有家 ⇒ 进家"）
#   · `A_field_root_lowered` / `C1/C2/C3_if_join_*` / `G_stale_origin` —— 以前放行成 UB，
#     第 1 步的记账修复把它们挡住（**误拒**），B2 之后 `alloc` 进了家 arena ⇒ 真的安全 ✓
#     这一条最值得记：**洞与误拒共用同一个出口**（§A1），B2 一做才分得清 ✓
set -u
# LeakSanitizer 对这套内存模型没有意义（arena 到进程结束才释放），它在最小发行版里还会偶尔
# 在退出扫描时挂住 ⇒ 这里只抓越界与 use-after-free。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
# gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核会**偶发**在启动阶段自己 SEGV
# （stderr 只有 DEADLYSIGNAL，然后挂住）；实测 32 路并发 9/32 次，关掉地址随机化后 0/32。
# 原因与三条出路见 tests/asan-probe.sh。这里只对本套件关随机化。
if [ -z "${EXTC_ASAN_NO_SETARCH:-}" ] && command -v setarch >/dev/null 2>&1; then
    EXTC_ASAN_NO_SETARCH=1 exec setarch "$(uname -m)" -R bash "$0" "$@"
fi
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0
for f in tests/arena-promoted/*.extc; do
    n=$(basename "$f" .extc)
    if ! "$EXTC" "$f" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1; then
        echo "  FAIL $n  ->  被拒了（但它是安全程序）"; head -2 "$TMP/$n.cerr" | sed 's/^/        /'; fail=1; continue
    fi
    if ! gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$n" "$TMP/$n.c" >"$TMP/$n.gerr" 2>&1; then
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
