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
# `-fwrapv`：生成的 C 的**既定编译契约**（有符号溢出在 extC 里是有定义的绕回，见 MANUAL §8；
# 编译器驱动自己也是这么编的，见 src/main.c）。少这一条时，`-O2` 会按"乘法不会溢出"优化，
# 哈希混合这类依赖绕回的代码会静默变形 —— 2026-09-26 就因此把 grow 用例挂死过一次。
build_one() { "$EXTC" "tests/hashmap/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
    && "$CC" -O1 -std=c11 -fwrapv "build/$1.c" -o "build/$1" >/dev/null 2>&1; }
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

echo "== rebuild 的旧列**不许**留着（四列在自己的板块上）：增量 vs 一开始预留 =="
# 判据分两层。**第一层是精确字节账**（主判据）：两次跑完，这张表在自己池里占的字节必须在
# 1 MB 以内相等 —— 同样的条目数、同样的最终 cap（2,097,152）、同样的列布局，差值只可能来自
# "旧列没还"。一整代旧列的量级（1,048,576×17 = 17.8 MB）不可能藏在这 1 MB 里 ⇒ 有牙。
#
# 第二层是峰值 RSS（参考值，不再做判据）：它随"清不清零"漂移，同一份代码在
# `poolSlice`（清零）下是 63,296 vs 69,056，在 `poolSliceRaw`（不清零）下是 63,296 vs 51,968 ——
# 一个"增量 ≤ 预留×1.1"的判据在这两种情况下会给出相反的结论，所以它不能当判据用。
if build_one grow-inc grow_rss && build_one grow-res grow_rss_reserved; then
    bi=$(./build/grow-inc 0 | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
    br=$(./build/grow-res 0 | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
    gi=$(peak grow-inc 0); gr=$(peak grow-res 0)
    if [ -n "$bi" ] && [ -n "$br" ] && [ "$(( bi > br ? bi - br : br - bi ))" -le 1048576 ]; then
        echo "  ok   grow      ->  增量 ${bi} B vs 预留 ${br} B ⇒ **逐字节相当**（旧列全还了）✓ · 峰值 RSS ${gi} / ${gr} KB（参考）"
    else
        echo "  FAIL grow      ->  增量 ${bi} B vs 预留 ${br} B ⇒ 差了 $((${bi:-0} - ${br:-0})) B（一整代旧列还留在池里？）✗"; fail=1
    fi
    # 绝对值那一层（"期末字节 = cap×17"）**不能用**：池里的字节是逐块累加的，不是 cap×17 ——
    # 四列按需长、`bucketOfDense` 用 poolResize 换块、值池按已用涨 ⇒ cap×17 只是上界，
    # 当等式用会误报。真正有牙的是上面那条"增量 ≡ 预留"：同一状态、两条路径，逐字节必须相等。
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
          && gcc -O1 -g -fwrapv -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    "$EXTC" tests/hashmap/churn.extc -o "$TMP/c.c" >/dev/null 2>&1 \
      && gcc -O1 -g -fwrapv -fsanitize=address -o "$TMP/c" "$TMP/c.c" >/dev/null 2>&1 \
      && "$TMP/c" 20000 >/dev/null 2>&1 || { echo "  FAIL churn -> ASan 报错"; fail=1; ok=0; }
    [ "$ok" = 1 ] && echo "  ok   basic · collide · index · churn  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
