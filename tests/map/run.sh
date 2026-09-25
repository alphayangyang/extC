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
    && "$CC" -fwrapv -O1 -std=c11 "build/$1.c" -o "build/$1" >/dev/null 2>&1; }
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

echo "== 增长时旧存储**不许**留在 arena（四块在自己板块上、增长是原块加长）=="
# **判据换过一次（2026-09-26「零初始化收窄」那一刀）**：原来只比 RSS 不等式（增量 ≤ 预留）。
# 那条在"列还清零"时成立，但它成立的原因之一**是预留那版被清零撑大**（90,564 KB）——
# 收窄清零之后预留掉到 49,284 KB，不等式翻转 ⇒ 判据红，而红的原因不是"代码变差"、是"基线变好"。
# ⇒ 换成**精确字节账**（主判据）：两条路径的「每节点字节」必须相同。旧存储若留了一代，
#   `bytes` 会多出 ~cap/1.5 × 每节点 ⇒ 这个商立刻变大。`bytes` 来自容器自己的池记账
#   （`map.bytesOf()` = `extc_pool_bytes(pid)`），与 RSS 无关 ⇒ 不受清不清零影响。
# 次要判据：增量那版末代容量（1.5 倍增长 ⇒ ~177k 节点）比预留（2 的幂 ⇒ 262,144）小 ⇒ bytes 更低。
# RSS 仍然打出来当**参考**（收窄清零之后它明显低于 bytes，那正是"预留的页没被提交"）。
if build_one map-inc grow_rss && build_one map-res grow_rss_reserved; then
    oi=$(./build/map-inc 0); orv=$(./build/map-res 0)
    bi=$(echo "$oi"  | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
    ci=$(echo "$oi"  | sed -n 's/.*cap=\([0-9]*\).*/\1/p')
    br=$(echo "$orv" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
    cr=$(echo "$orv" | sed -n 's/.*cap=\([0-9]*\).*/\1/p')
    gi=$(peak map-inc 0); gr=$(peak map-res 0)
    if [ -n "$bi" ] && [ -n "$br" ] && [ "$((bi / ci))" = "$((br / cr))" ] && [ "$bi" -le "$br" ]; then
        echo "  ok   grow bytes ->  增量 cap=$ci bytes=$bi（每节点 $((bi / ci)) B）vs 预留 cap=$cr bytes=$br（每节点 $((br / cr)) B）⇒ 旧存储没留下 ✓ · 峰值 RSS ${gi} / ${gr} KB（参考）"
    else
        echo "  FAIL grow bytes ->  增量 cap=$ci bytes=$bi（每节点 $((bi / (ci ? ci : 1))) B）vs 预留 cap=$cr bytes=$br（每节点 $((br / (cr ? cr : 1))) B）⇒ 旧存储留了一代（每节点字节会变大）✗"; fail=1
    fi
else
    echo "  FAIL grow bytes ->  编不过"; fail=1
fi

echo "== 复杂度：n 与 2n 的**指令数**比值必须仍是 O(n log n)（优化不许让某种输入退化）=="
# 作者 2026-09-26 的口径：任何优化都不许带来时间复杂度退化（"这会很惨烈"）。
# 判据用 callgrind 的**指令数**而不是墙钟 —— 指令数确定、可复现，2.09 = n log n、4 ≈ n²。
# 四种形状各跑 n=20000 与 n=40000（阈值 2.6：给常数因子留余量，二次形状必红）：
#   i 顺序插入 + 查一遍 · d 顺序插入 + 全删 · r 随机插入 + 全删 · c churn 四轮
if command -v valgrind >/dev/null 2>&1 && build_one map-cx cx_app; then
    TMPX=$(mktemp -d)
    cx_ok=1
    for mode in i d r c; do
        a=$(valgrind --tool=callgrind --callgrind-out-file="$TMPX.cg" ./build/map-cx 20000 $mode >/dev/null 2>&1; sed -n 's/^summary: *\([0-9]*\).*/\1/p' "$TMPX.cg")
        b=$(valgrind --tool=callgrind --callgrind-out-file="$TMPX.cg" ./build/map-cx 40000 $mode >/dev/null 2>&1; sed -n 's/^summary: *\([0-9]*\).*/\1/p' "$TMPX.cg")
        if [ -z "$a" ] || [ -z "$b" ] || [ "$a" = "0" ]; then
            echo "  FAIL 复杂度 $mode  ->  callgrind 没给出指令数"; cx_ok=0; continue
        fi
        # 比值放大 100 倍比较，避开 bash 没有浮点的麻烦
        r=$(( b * 100 / a ))
        if [ "$r" -le 260 ]; then
            echo "  ok   复杂度 $mode  ->  n=20000: $a 条指令 · n=40000: $b 条 ⇒ 比值 $((r / 100)).$(printf "%02d" $((r % 100)))（O(n log n) ≈ 2.1）✓"
        else
            echo "  FAIL 复杂度 $mode  ->  比值 $((r / 100)).$(printf "%02d" $((r % 100))) > 2.60 ⇒ 某种输入退化了 ✗"; cx_ok=0
        fi
    done
    rm -rf "$TMPX"
    [ "$cx_ok" = 1 ] || fail=1
else
    echo "  --  跳过：没有 valgrind"
fi

echo "== ASan =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fwrapv -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in sorted bounds stress churn shrink release index ends structkey; do
        "$EXTC" "tests/map/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
          && gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    [ "$ok" = 1 ] && echo "  ok   sorted · bounds · stress · churn · shrink · release · index · ends · structkey  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
