#!/usr/bin/env bash
# extlib/blas/tests/run.sh —— BLAS 绑定的判据
#
# 三件事，全都要过：
#   ① LP64 分支：用 lp64shim.c 造的 32 位维度 CBLAS，验 cblas_sgemm/sgemv/dgemm 这条分支
#      的签名宽度写对了（本机常常只有 numpy 自带的 ILP64 OpenBLAS，没有系统 libcblas）。
#   ② ILP64 / scipy 分支：机器上有真 CBLAS 时绑上去跑，结果必须与裸循环一致，
#      且 512³ 要比裸循环快 EXTC_BLAS_MIN_RATIO 倍（默认 2）—— 「封装没把性能吃掉」。
#   ③ 两条分支都必须报 mismatch=0：绑定层收 slice、查形状，算错的唯一可能来源就是
#      ABI 宽度（维度按 32 位声明却喂了 64 位库，或反过来）。
#
# 干净机器上没有 CBLAS 库时，②跳过、①照跑（shim 是自带的，不依赖环境）。
# 环境变量：EXTC 编译器路径 / EXTC_BLAS_LIB 指定库 / EXTC_BLAS_MIN_RATIO 速度下限。
set -u

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT" || exit 1

EXTC=${EXTC:-$ROOT/build/extc}
MIN_RATIO=${EXTC_BLAS_MIN_RATIO:-2.0}
CC=${CC:-cc}

if [ ! -x "$EXTC" ]; then
    echo "  --   跳过：还没有编译器（$EXTC）"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/extc-blas-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL $1"; exit 1; }

# ---------------------------------------------------------------- 构建判据程序
if ! "$EXTC" --build -I extlib/blas extlib/blas/tests/sgemm.extc -o "$TMP/sgemm" \
        --ccflag -l --ccflag dl >"$TMP/build.log" 2>&1; then
    cat "$TMP/build.log"
    fail "extlib/blas/tests/sgemm.extc 编不过"
fi

# ---------------------------------------------------------------- ① LP64 分支
if ! "$CC" -O2 -shared -fPIC -o "$TMP/libextc_blas_lp64shim.so" \
        extlib/blas/tests/lp64shim.c >"$TMP/shim.log" 2>&1; then
    cat "$TMP/shim.log"
    fail "lp64shim.c 编不过"
fi
if ! "$TMP/sgemm" "$TMP/libextc_blas_lp64shim.so" >"$TMP/shim.out" 2>&1; then
    cat "$TMP/shim.out"
    fail "LP64 shim 这次运行没有成功（rc != 0）"
fi
grep -q "^OK blas backend kind=1 " "$TMP/shim.out" ||
    { cat "$TMP/shim.out"; fail "LP64 shim 没有被认成 kind=1（ABI 认错了）"; }
for want in "sgemm 37x41x29 mismatch=0" "sgemv 37x41 mismatch=0" "dgemm 23x19x17 mismatch=0"; do
    grep -qF "$want" "$TMP/shim.out" || { cat "$TMP/shim.out"; fail "LP64 shim：$want 不成立"; }
done
echo "  ok   blas：LP64 ABI（自带 shim，32 位维度）sgemm/sgemv/dgemm 逐元素与裸循环一致"

# ---------------------------------------------------------------- ② 真库（ILP64 或 LP64）
find_blas() {
    if [ -n "${EXTC_BLAS_LIB:-}" ]; then printf '%s\n' "$EXTC_BLAS_LIB"; return 0; fi
    local n p
    for n in libopenblas.so.0 libopenblas64_.so.0 libopenblas64.so.0 libopenblas.so \
             libcblas.so.3 libblas.so.3 libmkl_rt.so.2 libmkl_rt.so; do
        p=$(ldconfig -p 2>/dev/null | awk -v n="$n" '$1 == n { print $NF; exit }')
        if [ -n "$p" ] && [ -e "$p" ]; then printf '%s\n' "$p"; return 0; fi
    done
    # numpy 轮子自带的那份（ILP64，符号前缀 scipy_）
    p=$(python3 - <<'PY' 2>/dev/null
import glob, os
try:
    import numpy
except Exception:
    raise SystemExit(0)
d = os.path.join(os.path.dirname(numpy.__file__), os.pardir, "numpy.libs")
hits = sorted(glob.glob(os.path.join(d, "libscipy_openblas64_*.so")))
if hits:
    print(os.path.abspath(hits[0]))
PY
)
    if [ -n "$p" ] && [ -e "$p" ]; then printf '%s\n' "$p"; return 0; fi
    return 1
}

# 候选逐个试：有的机器上只有 Fortran 的 libblas.so.3（没有 cblas_* 符号），
# 绑定层会返回 none、判据程序以 rc=3 表示「这个库不是 CBLAS」——换下一个，不是失败。
LIB=
CANDS=$(find_blas) || CANDS=
while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    "$TMP/sgemm" "$cand" >"$TMP/real.out" 2>&1
    rc=$?
    if [ "$rc" -eq 3 ]; then continue; fi
    if [ "$rc" -ne 0 ]; then
        cat "$TMP/real.out"
        fail "真库 $cand 这次运行没有成功（rc=$rc）"
    fi
    LIB=$cand
    break
done <<< "$CANDS"
if [ -z "$LIB" ]; then
    echo "  --   跳过：本机没有 CBLAS 库（系统 ldconfig 与 numpy 轮子都没找到可用的；用 EXTC_BLAS_LIB 指定）"
    exit 0
fi
KIND=$(sed -n 's/^OK blas backend kind=\([0-9]*\).*/\1/p' "$TMP/real.out" | head -1)
case "$KIND" in
    1|2) ;;
    *) cat "$TMP/real.out"; fail "真库 $LIB 的 ABI 没认出来（kind='$KIND'）" ;;
esac
for want in "sgemm 37x41x29 mismatch=0" "sgemv 37x41 mismatch=0" "dgemm 23x19x17 mismatch=0"; do
    grep -qF "$want" "$TMP/real.out" || { cat "$TMP/real.out"; fail "真库：$want 不成立"; }
done
RATIO=$(sed -n 's/.*ratio=\([0-9.][0-9.]*\)$/\1/p' "$TMP/real.out" | head -1)
awk -v r="$RATIO" -v m="$MIN_RATIO" 'BEGIN { exit !(r + 0 >= m + 0) }' ||
    { cat "$TMP/real.out"; fail "真库 512³ 只有裸循环的 ${RATIO}×，低于下限 ${MIN_RATIO}×（封装吃掉了性能？）"; }
echo "  ok   blas：$LIB（kind=$KIND）算对了，512³ 是裸循环的 ${RATIO}×"
exit 0
