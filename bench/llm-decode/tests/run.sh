#!/usr/bin/env bash
# bench/llm-decode/tests/run.sh —— 端到端 decode 判据：extC 跑真的 tiny 模型，和 llama.cpp 比
#
# 两件事：
#   ① 算得对：stories15M 的 q8_0 / q4_0 两个 GGUF 上，贪心解出 20 个 token 必须与 llama.cpp
#      逐 id 一致（oracle 硬编码在这里；换模型/换 oracle 就要改这张表）。
#   ② 没退化：单线程吞吐不得低于 EXTC_LLM_MIN_TOKS（默认 200 tok/s）。本机实测 q8_0 约
#      1000、q4_0 约 1200，这条只是「别退化成解释器/别把 GEMV 写回标量」的护栏。
#
# 依赖（缺一样就跳过，电池记 skip，干净机器上不会红）：ggml 共享库（EXTC_GGML_DIR 指到 cmake
# 的 build/src）、stories15M GGUF（EXTC_LLM_MODEL_DIR 或 /tmp/models）。
set -u

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT" || exit 1

EXTC=${EXTC:-$ROOT/build/extc}
MIN_TOKS=${EXTC_LLM_MIN_TOKS:-200}
NL=$'\n'

if [ ! -x "$EXTC" ]; then
    echo "  --   跳过：还没有编译器（$EXTC）"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/extc-llm-decode.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL $1"; exit 1; }

# ---------------------------------------------------------------- 构建
if ! "$EXTC" -I extlib/ggml --build bench/llm-decode/decode.extc -o "$TMP/decode" \
        --ccflag -lm >"$TMP/build.log" 2>&1; then
    cat "$TMP/build.log"
    fail "bench/llm-decode/decode.extc 编不过"
fi

# ---------------------------------------------------------------- 找 ggml 与模型
find_ggml() {
    if [ -n "${EXTC_GGML_DIR:-}" ] && [ -e "$EXTC_GGML_DIR/libggml-base.so" ]; then
        printf '%s\n' "$EXTC_GGML_DIR"; return 0
    fi
    local d
    for d in "$ROOT/build/ggml/src" "$ROOT/third_party/ggml/build/src" \
             "$ROOT/../ggml/build/src" "/usr/local/lib"; do
        if [ -e "$d/libggml-base.so" ] && [ -e "$d/libggml-cpu.so" ]; then
            printf '%s\n' "$d"; return 0
        fi
    done
    return 1
}

find_model() {  # $1 = q8_0 / q4_0
    local name="stories15M-$1.gguf" d
    for d in "${EXTC_LLM_MODEL_DIR:-}" "$ROOT/bench/llm-decode/models" /tmp/models; do
        [ -n "$d" ] || continue
        [ -f "$d/$name" ] && { printf '%s\n' "$d/$name"; return 0; }
    done
    if [ -n "${EXTC_LLM_MODEL:-}" ] && [ -f "${EXTC_LLM_MODEL}" ]; then
        case "$EXTC_LLM_MODEL" in *"$1"*) printf '%s\n' "$EXTC_LLM_MODEL"; return 0;; esac
    fi
    return 1
}

GDIR=$(find_ggml) || GDIR=
if [ -z "$GDIR" ]; then
    echo "  --   跳过：本机没有 ggml 共享库（用 EXTC_GGML_DIR 指到 cmake 的 build/src）"
    exit 0
fi
MODELS=
for q in q8_0 q4_0; do
    m=$(find_model "$q") || continue
    MODELS="$MODELS$q=$m$NL"
done
if [ -z "$MODELS" ]; then
    echo "  --   跳过：本机没有 stories15M 的 GGUF（用 EXTC_LLM_MODEL_DIR 指到放模型的目录）"
    exit 0
fi

# ---------------------------------------------------------------- 判据
# oracle = llama.cpp 在同一模型上的贪心结果（llama-tokenize + llama-cli/probe 取得）。
oracle_for() {
    case "$1" in
        q8_0) echo "29892 727 471 263 2217 7826 4257 365 2354 29889 2296 18012 304 1708 5377 297 278 6575 845 457" ;;
        q4_0) echo "29892 727 471 263 2217 7826 4257 365 2354 29889 2296 18012 304 1708 5377 322 26987 278 3186 2820" ;;
        *) return 1 ;;
    esac
}

tested=
while IFS='=' read -r q model; do
    [ -n "$q" ] || continue
    want=$(oracle_for "$q") || fail "没有 $q 的 oracle"
    LD_LIBRARY_PATH="$GDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$TMP/decode" "$model" 20 "$GDIR" >"$TMP/out-$q" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        cat "$TMP/out-$q"
        fail "decode（$q）rc=$rc"
    fi
    got=$(sed -n 's/^GEN //p' "$TMP/out-$q" | head -1)
    if [ "$got" != "$want" ]; then
        cat "$TMP/out-$q"
        fail "$q 的贪心 token 与 llama.cpp 不一致"$'\n'"  期望：$want"$'\n'"  实得：$got"
    fi
    toks=$(sed -n 's/^TIME .*tok_s=\([0-9.][0-9.]*\)$/\1/p' "$TMP/out-$q" | head -1)
    [ -n "$toks" ] || { cat "$TMP/out-$q"; fail "$q 没打印 tok_s"; }
    awk -v t="$toks" -v m="$MIN_TOKS" 'BEGIN { exit !(t + 0 >= m + 0) }' ||
        { cat "$TMP/out-$q"; fail "$q 只有 ${toks} tok/s，低于下限 ${MIN_TOKS}（GEMV 退回标量了？）"; }
    tested="${tested:+$tested }$q=${toks}tok/s"
done <<< "$MODELS"

echo "  ok   llm-decode：贪心 20 token 与 llama.cpp 逐 id 一致（$tested）"
exit 0
