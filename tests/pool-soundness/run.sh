#!/usr/bin/env bash
# tests/pool-soundness/run.sh —— 池档健全性反例（见 docs/topics/POOL-SOUNDNESS.md）
# oracle：**接受 + ASan UAF** = 洞还开着；修好后应变成 **编译错误或 trap**。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

run_case() {
    local name=$1 src=$2 want=$3 out
    if ! out=$("$EXTC" -w -o "$TMP/$name.c" "$src" 2>&1); then
        echo "  ok   $name  ->  编译期挡住（修好后的期望形状）"
        return
    fi
    if ! gcc -O1 -g -fsanitize=address -fwrapv "$TMP/$name.c" -o "$TMP/$name" 2>/dev/null; then
        echo "  ?    $name  ->  生成的 C 编不过"; return
    fi
    out=$(timeout 120 "$TMP/$name" 2>&1)
    case "$want" in
    hole)
        if echo "$out" | grep -q "AddressSanitizer"; then
            echo "  HOLE $name  ->  接受 + ASan UAF（洞还开着，见文档 §5 E1）"
        else
            echo "  ok   $name  ->  不再 UAF ⇒ 洞可能已修（请把判据翻面）"
        fi ;;
    control)
        if echo "$out" | grep -q "AddressSanitizer"; then
            echo "  FAIL $name  ->  对照组不该 UAF"; fail=1
        else
            echo "  ok   $name  ->  对照组正常（$(echo "$out" | head -1)）"
        fi ;;
    esac
}

echo "== 池档：反例（今天没有开着的了 —— E1 已修）=="
run_case C3_string_sub_now_copies tests/pool-soundness/C3_string_sub_now_copies.extc control
echo "== 池档：对照（同形状、拷贝语义 ⇒ 不该炸）=="
run_case C1_vector_toslice_copy tests/pool-soundness/C1_vector_toslice_copy.extc control
# C2：陈旧拷贝 release —— **守卫在这里是承重的**（拿掉它 = 同一份程序 ASan UAF）
run_case C2_stale_copy_release tests/pool-soundness/C2_stale_copy_release.extc control
# C4：`subView` 的**安全用法**（取视图后不再改串）—— 零拷贝仍在，只是要签字
run_case C4_subview_contract tests/pool-soundness/C4_subview_contract.extc control
exit $fail
