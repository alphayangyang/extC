#!/usr/bin/env bash
# 生成 C 的「金标准」快照 —— 重构时用它证明**行为一点没变**。
#
#   tools/golden.sh save    把当前编译器对 examples/ 的输出存成基线（build/golden/base）
#   tools/golden.sh check   重新生成并跟基线对比：有差异 ⇒ 打印差异并非零退出
#
# ⚠️ 用 `--no-line-map`（否则生成物里带绝对路径和行号映射，跟重构无关的噪声一堆）✓
# 用它定死的规矩：**纯移动代码的拆分不许改变一个字节**；
# 有意改行为时，先跑 `save` 再跑 `check` 看差异**是不是你想要的那一处** ✓
set -u
cd "$(dirname "$0")/.."

EXTC=./build/extc
DIR=build/golden
BASE="$DIR/base"
NOW="$DIR/now"

gen() {
    local out="$1"
    rm -rf "$out"
    mkdir -p "$out"
    local f n
    for f in examples/*.extc bench/*/*.extc; do
        [ -f "$f" ] || continue
        n=$(echo "$f" | tr '/' '_' | sed 's/\.extc$//')
        if ! $EXTC --no-line-map "$f" > "$out/$n.c" 2>"$out/$n.err"; then
            echo "// (compile failed; see .err)" > "$out/$n.c"
        fi
    done
    rm -f "$out"/*.err          # 编译失败的由 tests/ 套件负责，这里只比"能编过"的输出
}

case "${1:-check}" in
save)
    gen "$BASE"
    echo "ok   基线已存：$BASE（$(ls "$BASE" | wc -l) 个文件，$(cat "$BASE"/*.c | wc -l) 行）"
    ;;
check)
    [ -d "$BASE" ] || { echo "没有基线，先跑 tools/golden.sh save"; exit 2; }
    gen "$NOW"
    if diff -rq "$BASE" "$NOW" > /tmp/extc-golden.diff 2>&1; then
        echo "ok   生成 C 与基线**逐字节相同**（$(ls "$BASE" | wc -l) 个文件）"
    else
        echo "FAIL 生成 C 有差异："
        cat /tmp/extc-golden.diff
        exit 1
    fi
    ;;
*)
    echo "用法：tools/golden.sh [save|check]"; exit 2 ;;
esac
