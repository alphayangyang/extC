#!/usr/bin/env bash
# 生成 C 的「金标准」快照 —— 重构时用它证明**行为一点没变**。
#
#   tools/golden.sh save    把当前编译器的输出记成基线（tools/golden.manifest，**可提交**）
#   tools/golden.sh check   重新生成并跟基线比 md5：有差异 ⇒ 打印哪个文件变了并非零退出
#
# ⚠️ 三条踩过的经验：
#   · 用 `--no-line-map`（否则生成物里带绝对路径，跟重构无关的噪声一堆）✓
#   · 基线**不能**放 build/ —— `make clean` 会把它一起删掉 ✗（我踩过）
#     所以基线是 `<md5>  <名字>` 的清单（可提交），完整文本存 build/golden/ 只作诊断 ✓
#   · 要拿"拆分前的编译器"当基线：`EXTC=/path/to/extc tools/golden.sh save` ✓
#
# 定死的规矩：**纯移动代码的拆分不许改变一个字节**；
# 有意改行为时，先 save 再 check，看差异**是不是你想要的那一处** ✓
set -u
cd "$(dirname "$0")/.."

EXTC=${EXTC:-./build/extc}
MANIFEST=tools/golden.manifest
DIR=build/golden

files() {                       # 例子里所有能编过的源
    local f
    for f in examples/*.extc bench/*/*.extc; do
        [ -f "$f" ] && echo "$f"
    done
}

gen() {                         # $1 = 输出目录；清单打到 stdout
    local out="$1" f n
    rm -rf "$out"; mkdir -p "$out"
    files | while read -r f; do
        n=$(echo "$f" | tr '/' '_' | sed 's/\.extc$//')
        if $EXTC --no-line-map "$f" > "$out/$n.c" 2>/dev/null; then
            printf '%s  %s\n' "$(md5sum < "$out/$n.c" | cut -d' ' -f1)" "$n"
        else
            printf '%s  %s\n' "(compile-failed)" "$n"
        fi
    done
}

case "${1:-check}" in
save)
    gen "$DIR/base" > "$MANIFEST"
    echo "ok   基线已存：$MANIFEST（$(wc -l < "$MANIFEST") 个文件）"
    ;;
check)
    mkdir -p "$DIR"
    [ -f "$MANIFEST" ] || { echo "没有基线，先跑 tools/golden.sh save"; exit 2; }
    gen "$DIR/now" > "$DIR/now.manifest"
    if diff -u "$MANIFEST" "$DIR/now.manifest" > /tmp/extc-golden.diff 2>&1; then
        echo "ok   生成 C 与基线**逐字节相同**（$(wc -l < "$MANIFEST") 个文件）"
    else
        echo "FAIL 生成 C 有差异（左=基线，右=现在）："
        head -20 /tmp/extc-golden.diff
        exit 1
    fi
    ;;
*)
    echo "用法：tools/golden.sh [save|check]"; exit 2 ;;
esac
