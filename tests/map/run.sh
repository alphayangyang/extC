#!/usr/bin/env bash
# tests/map/run.sh —— **有序 map<K, V> 的常设验收**（POOLS.md §11：B 树；第一步是单节点有序表）
#
# 判据：
#   ① 正例：乱序插入后**按序**遍历正确 · 覆盖返回真 · 删中间键后仍有序 ✓
#   ② lowerBound 四个边界：空表 · 比最小还小 · 比最大还大 · 命中 ✓
#   ③ ASan 干净 ✓
#   （RSS churn 与 canary 两条判据要等节点搬进池之后再加：今天的实现是单个有序数组，
#     插入是 O(n) 搬移，1e6 轮的规模跑不动 —— 不是省略，是这一步还没有那个性质 ✓）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/map/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/map/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

echo "== 正例：乱序插入 ⇒ 按序遍历 · 覆盖 · 删中间键 =="
run_case sorted

echo "== lowerBound 四边界 + 缺失路径 =="
run_case bounds

echo "== ASan =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in sorted bounds; do
        "$EXTC" "tests/map/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
          && gcc -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    [ "$ok" = 1 ] && echo "  ok   sorted · bounds  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
