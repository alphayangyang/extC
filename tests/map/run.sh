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

echo "== 压力：4001 键乱序插入（多层分裂）⇒ 按序遍历 · get · lowerBound · 删除后复核 =="
run_case stress

echo "== churn：5 轮「插满 2000 → 全删」⇒ 节点回收（nodeCount 不得随轮数线性增长）=="
run_case churn

echo "== shrink：2 万键插满后删到 100 ⇒ 活节点压实（水印下降），且压实后仍可插入 =="
run_case shrink

echo "== release：容器与运行期池一起活、一起还 =="
run_case release

echo "== 下标语法 m[k]（③ 第一步：只做读）=="
run_case index

echo "== 有序容器的最小/最大键（map 的 firstKey/lastKey、set 的 first/last）=="
run_case ends

echo "== 键只要求 <：结构体键的有序表（相等由 !(a<b) && !(b<a) 派生）=="
run_case structkey

echo "== churn（RSS）：1e6 轮与 1e5 轮的峰值 RSS 必须相当 =="
build_one() { "$EXTC" "tests/map/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
    && "$CC" -O1 -std=c11 "build/$1.c" -o "build/$1" >/dev/null 2>&1; }
peak() { /usr/bin/time -f %M "./build/$1" "$2" 2>&1 >/dev/null | tail -1; }
if build_one map-churn rss && build_one map-leak leak; then
    s=$(peak map-churn 100000); b=$(peak map-churn 1000000)
    if [ "$b" -le $(( s * 3 / 2 )) ]; then
        echo "  ok   churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ **平** ✓（节点回收链在起作用）"
    else
        echo "  FAIL churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ 涨了 ✗（节点没回收？）"; fail=1
    fi
    ls=$(peak map-leak 100000); lb=$(peak map-leak 1000000)
    if [ "$lb" -gt $(( ls * 2 )) ]; then
        echo "  ok   canary   ->  只 put 不 remove：${ls} KB → ${lb} KB ⇒ **判据会响** ✓"
    else
        echo "  FAIL canary   ->  没涨（${ls} → ${lb} KB）⇒ 上面那条没有牙 ✗"; fail=1
    fi
else
    echo "  FAIL churn  ->  编不过"; fail=1
fi

echo "== 反例：只有读侧 [] 而没有写侧 []= ⇒ 写侧必须给可读的诊断 =="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err tests/map/errors/index_assign.extc 'defines `[]` but not `[]=`'

echo "== ASan =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in sorted bounds stress churn shrink release index ends structkey; do
        "$EXTC" "tests/map/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
          && gcc -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    [ "$ok" = 1 ] && echo "  ok   sorted · bounds · stress · churn · shrink · release · index · ends · structkey  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
