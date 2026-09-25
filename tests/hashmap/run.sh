#!/usr/bin/env bash
# tests/hashmap/run.sh —— **开放寻址哈希表的常设验收**（docs/topics/REGIONS.md §10.2 的选型表）
#
# 判据：
#   ① 正例：新键/覆盖/查/删/表位置遍历 ✓
#   ② **碰撞**：cap=8 时 0/8/16/24/32 撞同一初始槽 ⇒ 线性探测全部找到；挖掉链条中间一个后
#      剩下的仍找到（墓碑 + 探测链 ✓）；再插回去复用墓碑槽 ✓
#   ③ **墓碑 churn**：反复 put+remove 同一个键 ⇒ 1e6 轮的峰值 RSS 与 1e5 轮相当 ✓
#      （没有 dead 计数与 rebuild 的实现会在这里退化 ✗ —— 哈希表最容易烂的地方 ✓）
#   ④ **判据有牙**：只 put 不 remove ⇒ 必须涨（canary）✓
#   ⑤ ASan 干净 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/hashmap/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/hashmap/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

echo "== 正例：新键 · 覆盖 · 查 · 删 =="
run_case basic
run_case structkey tests/hashmap/structkey.extc
run_case dense     tests/hashmap/dense.extc
run_case index     tests/hashmap/index.extc
echo "== 碰撞：线性探测 + 墓碑 + 复用墓碑槽 =="
run_case collide

echo "== 墓碑 churn：1e6 轮与 1e5 轮的峰值 RSS 必须相当 =="
build_one() { "$EXTC" "tests/hashmap/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
    && "$CC" -O1 -std=c11 "build/$1.c" -o "build/$1" >/dev/null 2>&1; }
peak() { /usr/bin/time -f %M "./build/$1" "$2" 2>&1 >/dev/null | tail -1; }
if build_one map-churn churn && build_one map-leak churn-leak; then
    s=$(peak map-churn 100000); b=$(peak map-churn 1000000)
    if [ "$b" -le $(( s * 3 / 2 )) ]; then
        echo "  ok   churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ **平** ✓"
    else
        echo "  FAIL churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ 涨了 ✗（墓碑/重建有问题？）"; fail=1
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

echo "== rebuild 的旧列**不许**留在 arena（四列在自己的板块上）：增量 vs 一开始预留 =="
# 判据：同一 1e6 条目、同一最终 cap（2,097,152）。四列用 `new`（arena）时，增量那一版
# 会把每一代旧列留给 arena ⇒ 90,412 KB 对 69,036 KB（多 30%）。现在两者应当持平。
if build_one grow-inc grow_rss && build_one grow-res grow_rss_reserved; then
    gi=$(peak grow-inc 0); gr=$(peak grow-res 0)
    if [ "$gi" -le $(( gr * 11 / 10 )) ]; then
        echo "  ok   grow rss ->  增量 ${gi} KB vs 预留 ${gr} KB ⇒ 旧列没留下 ✓"
    else
        echo "  FAIL grow rss ->  增量 ${gi} KB 明显高于预留 ${gr} KB ⇒ 旧列又留在 arena 了 ✗"; fail=1
    fi
else
    echo "  FAIL grow rss ->  编不过"; fail=1
fi

echo "== ASan =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in basic collide index; do
        "$EXTC" "tests/hashmap/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
          && gcc -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    "$EXTC" tests/hashmap/churn.extc -o "$TMP/c.c" >/dev/null 2>&1 \
      && gcc -O1 -g -fsanitize=address -o "$TMP/c" "$TMP/c.c" >/dev/null 2>&1 \
      && "$TMP/c" 20000 >/dev/null 2>&1 || { echo "  FAIL churn -> ASan 报错"; fail=1; ok=0; }
    [ "$ok" = 1 ] && echo "  ok   basic · collide · index · churn  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
