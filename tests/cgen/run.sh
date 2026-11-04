#!/usr/bin/env bash
# tests/cgen/run.sh —— **生成的 C 在严格警告下必须是干净的**（`-Wall -Wextra -Werror`）
#
# 为什么单独一套：`tests/lambda` 等处虽然也用 `-Werror` 编译产物，但那些程序不含这些形状；
# 而这两个缺陷（区间循环的步进标签、`par` 的返回值）只在"产物里有这种形状"时才暴露，
# 一旦回归，用户看到的是"extc 生成的 C 编不过"，比语言级诊断难查得多。
#
# 判据：
#   ① `for_no_continue`：产物里 `__extc_forstep_` 出现 **0** 次（标签只在 `continue` 跳它时才发）
#   ② `for_continue`：产物里必须有步进标签，且嵌套/外层 `continue` 的语义对（`42 3135`）
#   ③ `par_forms`：`par` 语句与 `parallel::run(...)` 当值用两种写法都在 `-Werror` 下过（`992 140 0`）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-18s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-18s %s\n' "$1" "$2"; }

case1() { # name file want  (want = 期望 stdout 单行)
    local name=$1 s=$2 want=$3 got n
    if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$s" 2>"$tmp/e"; then
        bad "$name" "extc: $(head -1 "$tmp/e")"; return; fi
    if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -pthread -o "$tmp/$name" "$tmp/$name.c" 2>"$tmp/ce"; then
        bad "$name" "gcc -Werror: $(head -1 "$tmp/ce")"; return; fi
    got=$("$tmp/$name")
    [ "$got" = "$want" ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got]"
    n=$(grep -c '__extc_forstep_' "$tmp/$name.c" || true); echo "$n" > "$tmp/$name.labels"
}

case1 for_no_continue tests/cgen/for_no_continue.extc "45"
if [ ! -f "$tmp/for_no_continue.labels" ]; then
    :            # 编译已经红过（case1 报过），标签形状不再单独判
elif [ "$(cat "$tmp/for_no_continue.labels")" = 0 ]; then
    ok "no-label" "体内没有 continue ⇒ 一个步进标签都没发"
else
    bad "no-label" "没有 continue 却发了 $(cat "$tmp/for_no_continue.labels") 处步进标签"
fi

case1 for_continue tests/cgen/for_continue.extc "42 3135"
if [ -f "$tmp/for_continue.labels" ]; then
    if [ "$(cat "$tmp/for_continue.labels")" -ge 3 ]; then
        ok "label-shape" "有 continue ⇒ 步进标签照发（$(cat "$tmp/for_continue.labels") 处）"
    else
        bad "label-shape" "有 continue 却没发步进标签"
    fi
fi

case1 par_forms tests/cgen/par_forms.extc "992 140 0"

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

# ④ `trap_file`：库里的检查与主程序里的检查各自报自己的文件名
case1 trap_file tests/cgen/trap_file.extc "7 0"
if grep -q 'stdlib/std/data/vector.extc' "$tmp/trap_file.c"; then
    ok "trap-file-lib" "库里的检查带 stdlib 路径"
else
    bad "trap-file-lib" "库里的检查没带 stdlib 路径"
fi
if grep -q 'tests/cgen/trap_file.extc' "$tmp/trap_file.c"; then
    ok "trap-file-main" "主程序的检查带自己的文件名"
else
    bad "trap-file-main" "主程序的检查没带自己的文件名"
fi
