#!/usr/bin/env bash
# extlib/ggml/tests/run.sh —— ggml 量化内核绑定的判据
#
# 两件事：
#   ① 算得对：q4_0 / q8_0 的块布局（f16 尺度 + nibble/int8 位序）必须与 ggml 的
#      dequantize_row_* 逐位一致；vec_dot 必须与「反量化后 f32 点积」一致 ——
#      这层抓的是 ggml 版本漂移（符号还在但布局/调用约定变了）。
#   ② 有收益：m×k 的 q4_0 GEMV 至少要快过同规模 f32 GEMV EXTC_GGML_MIN_RATIO 倍
#      （默认 2）—— 绑定的价值就是 decode 阶段少读 4 倍权重字节，不快就是白绑。
#
# 干净机器上没有 ggml 时整段跳过（exit 0，电池记 skip）：ggml 是可选第三方库，
# 不进 std、也不入库；用 EXTC_GGML_DIR 指到 cmake 的 build/src 目录。
# 环境变量：EXTC 编译器路径 / EXTC_GGML_DIR ggml 构建目录 / EXTC_GGML_MIN_RATIO 速度下限。
set -u

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT" || exit 1

EXTC=${EXTC:-$ROOT/build/extc}
MIN_RATIO=${EXTC_GGML_MIN_RATIO:-2.0}

if [ ! -x "$EXTC" ]; then
    echo "  --   跳过：还没有编译器（$EXTC）"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/extc-ggml-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL $1"; exit 1; }

# ---------------------------------------------------------------- 构建判据程序
if ! "$EXTC" --build -I extlib/ggml extlib/ggml/tests/gemv.extc -o "$TMP/gemv" \
        --ccflag -l --ccflag dl >"$TMP/build.log" 2>&1; then
    cat "$TMP/build.log"
    fail "extlib/ggml/tests/gemv.extc 编不过"
fi

# ---------------------------------------------------------------- 找 ggml
# 只认显式指定与几个常见布局：ggml 不是系统库，猜不到就该跳过。
find_ggml() {
    if [ -n "${EXTC_GGML_DIR:-}" ] && [ -e "$EXTC_GGML_DIR/libggml-base.so" ]; then
        printf '%s\n' "$EXTC_GGML_DIR"
        return 0
    fi
    local d
    for d in "$ROOT/build/ggml/src" "$ROOT/third_party/ggml/build/src" \
             "$ROOT/../ggml/build/src" "/usr/local/lib"; do
        if [ -e "$d/libggml-base.so" ] && [ -e "$d/libggml-cpu.so" ]; then
            printf '%s\n' "$d"
            return 0
        fi
    done
    return 1
}

GDIR=$(find_ggml) || GDIR=
if [ -z "$GDIR" ]; then
    echo "  --   跳过：本机没有 ggml 共享库（用 EXTC_GGML_DIR 指到 cmake 的 build/src）"
    exit 0
fi

"$TMP/gemv" "$GDIR" >"$TMP/out" 2>&1
rc=$?
if [ "$rc" -eq 3 ]; then
    cat "$TMP/out"
    echo "  --   跳过：$GDIR 里的 ggml 认不出需要的符号（版本太旧/太新？）"
    exit 0
fi
if [ "$rc" -ne 0 ]; then
    cat "$TMP/out"
    fail "ggml 判据程序没有通过（rc=$rc）"
fi

# ---------------------------------------------------------------- 判据
grep -qF "layout q4_0 mismatch=0 q8_0 mismatch=0" "$TMP/out" ||
    { cat "$TMP/out"; fail "块布局读错了：手写解 q4_0/q8_0 与 dequantize_row_* 不一致"; }
grep -qF "gemvQ4 33x1024 mismatch=0" "$TMP/out" ||
    { cat "$TMP/out"; fail "gemvQ4 与反量化后 f32 GEMV 不一致"; }
RATIO=$(sed -n 's/^OK ggml .*ratio=\([0-9.][0-9.]*\)$/\1/p' "$TMP/out" | head -1)
[ -n "$RATIO" ] || { cat "$TMP/out"; fail "判据程序没给出 OK 行"; }
awk -v r="$RATIO" -v m="$MIN_RATIO" 'BEGIN { exit !(r + 0 >= m + 0) }' ||
    { cat "$TMP/out"; fail "q4_0 GEMV 只有 f32 GEMV 的 ${RATIO}×，低于下限 ${MIN_RATIO}×（绑定把带宽优势吃掉了？）"; }
echo "  ok   ggml：q4_0/q8_0 布局与 dequantize 逐位一致，q4_0 GEMV 是 f32 GEMV 的 ${RATIO}×"
exit 0
