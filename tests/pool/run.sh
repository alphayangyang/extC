#!/usr/bin/env bash
# tests/pool/run.sh —— **期 0（slot map / ECS 底座）的常设验收**（docs/topics/POOLS.md；期 0 是库级 slot map，期 1 起是运行期池注册表）
#
# 判据：
#   ① 正例：`insert`/`get`/`set`/`remove`/`len` + dense↔handle 双向 + `toSlice` ✓
#   ② 失效：`remove` 之后旧 handle **失效**（世代 +1 ⇒ contains=false ✓）
#   ③ **churn 不涨**：1e6 轮的峰值 RSS 必须与 1e5 轮**相当**（可见的判据）✓
#   ④ **判据有牙**：同一个形状**不 remove** ⇒ 必须明显涨（canary：9.9MB → 67MB ✓）
#   ⑤ 复用容量：`clear()` 之后 len=0、再插不重新分配 ✓
#   ⑥ ASan 干净 ✓
#   ⑦ 染色这一关的两条实测结论，钉成**常设判据**（见文件末尾）：
#        · 地方退出的代价随池数走得平（每池约 10 ns，其中板块 malloc/free 7 ns 是必需的）
#        · 真收缩：800 KB × 2000 轮的峰值 RSS 必须平，且期末字节账归零
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

# 跑一个 `// expect: a|b|c` 形状的用例
run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/pool/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/pool/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

echo "== 正例：稳定 handle · dense 连续 · 世代失效 · API 面 =="
run_case basic
run_case api
run_case gather

echo "== churn：1e6 轮的峰值 RSS 必须与 1e5 轮相当（还槽位 ⇒ 平 ✓）=="
build_one() {   # 名字 文件
    "$EXTC" "tests/pool/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
      && "$CC" -fwrapv -O1 -std=c11 "build/$1.c" -o "build/$1" >/dev/null 2>&1
}
peak() {        # 程序 轮数 -> 峰值 RSS(KB)
    /usr/bin/time -f %M "./build/$1" "$2" 2>&1 >/dev/null | tail -1
}
echo "== 池级 epoch 的价值：clear() 的代价与容量无关（旧版逐槽重写 ⇒ 4096 槽要慢几百倍）=="
if build_one pool-perf-s perf_clear_8 && build_one pool-perf-b perf_clear_4096; then
    ts=$(/usr/bin/time -f %e ./build/pool-perf-s 2>&1 >/dev/null | tail -1)
    tb=$(/usr/bin/time -f %e ./build/pool-perf-b 2>&1 >/dev/null | tail -1)
    if awk -v a="$ts" -v b="$tb" 'BEGIN{exit !(b <= a*4 + 0.20)}'; then
        echo "  ok   clear O(1) ->  cap=8: ${ts}s · cap=4096: ${tb}s ⇒ **同量级**（400 万轮）"
    else
        echo "  FAIL clear O(1) ->  cap=8: ${ts}s · cap=4096: ${tb}s ⇒ 随容量涨了（clear 还在逐槽重写？）"; fail=1
    fi
else
    echo "  FAIL clear O(1) ->  编不过"; fail=1
fi

if build_one pool-churn churn && build_one pool-leak churn-leak; then
    s=$(peak pool-churn 100000);  b=$(peak pool-churn 1000000)
    if [ "$b" -le $(( s * 3 / 2 )) ]; then
        echo "  ok   churn      ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ **平** ✓（10 倍轮数，内存不变）"
    else
        echo "  FAIL churn      ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ 涨了 ✗（槽位没复用？）"; fail=1
    fi
    ls=$(peak pool-leak 100000); lb=$(peak pool-leak 1000000)
    if [ "$lb" -gt $(( ls * 2 )) ]; then
        echo "  ok   canary     ->  不 remove：${ls} KB → ${lb} KB ⇒ **判据会响** ✓"
    else
        echo "  FAIL canary     ->  不 remove 竟然没涨（${ls} → ${lb} KB）⇒ 上面那条判据没有牙 ✗"; fail=1
    fi
else
    echo "  FAIL churn  ->  编不过"; fail=1
fi

echo "== ASan：这些路径必须干净 =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fwrapv -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in basic api; do
        if "$EXTC" "tests/pool/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
           && gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
           && "$TMP/$t" >/dev/null 2>&1; then :; else echo "  FAIL $t  ->  ASan 报错"; fail=1; ok=0; fi
    done
    if "$EXTC" tests/pool/churn.extc -o "$TMP/churn.c" >/dev/null 2>&1 \
       && gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/churn" "$TMP/churn.c" >/dev/null 2>&1 \
       && "$TMP/churn" 20000 >/dev/null 2>&1; then :; else echo "  FAIL churn  ->  ASan 报错"; fail=1; ok=0; fi
    [ "$ok" = 1 ] && echo "  ok   basic · api · churn  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
fi
rm -rf "$TMP"

# ---------------------------------------------------------------- 期 1：运行期池注册表
# 机制本身（不是容器）的验收：建/世代/释放 · 块退出带走子树 · churn 容量停在高水位 ·
# 旧 handle 带位置地失败 · 内存来自同一只 arena · 生成物 -Werror + ASan
echo
echo "== 期 1 · 池注册表：建 / 世代 / 释放 =="
rt_run() {   # rt_run <名字> <文件> <期望的一整行>
    # `-w`：这一节判的是**运行期行为**（输出的那一整行必须逐字相等）。`@sharesStorage` 的
    # 编译期警告会混进 `2>&1` 里，把比较搅坏 —— 而那条警告本身有专门的判据
    #（tests/stl/run.sh 的 clone_warn/clone_ok），这里静音不丢覆盖面。
    local name=$1 f=$2 want=$3 out
    if ! out=$("$EXTC" -w --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if [ "$out" = "$want" ]; then echo "  ok   $name  ->  $out"
    else echo "  FAIL $name  ->  期望「$want」，得到「$out」"; fail=1; fi
}
rt_run rt_basic    tests/pool/rt_basic.extc    "live=0 up=1 gen=1 down=0 stale=0"

echo "== 期 1 · 块退出带走子树 · 父释放带走子 =="
rt_run rt_blockexit tests/pool/rt_blockexit.extc "before=0 in=1 rid=0 after=0 two=2 gone=0"

echo "== 期 1 · churn：容量停在高水位（内存平）=="
rt_run rt_churn    tests/pool/rt_churn.extc    "live=0 cap=64 gen=0 acc=20000100000"

echo "== 期 1 · 旧 handle 带位置地失败（bug ⇒ trap、条件 ⇒ 值）=="
if out=$("$EXTC" --run tests/pool/rt_stale.extc 2>&1); rc=$?; then :; fi
if [ "${rc:-1}" = 1 ] && echo "$out" | grep -q "staleHandle" && echo "$out" | grep -q "tests/pool/rt_stale.extc:"; then
    echo "  ok   rt_stale  ->  $(echo "$out" | head -1 | cut -c1-72)"
else
    echo "  FAIL rt_stale  ->  期望带 staleHandle 与 tests/pool/rt_stale.extc:行 的 trap + 退出码 1，得到 rc=${rc:-?}：$(echo "$out" | head -1)"
    fail=1
fi

echo "== 期 1 · 一个地方一个 zone：多个池 + 嵌套树，出块整区走 =="
rt_run rt_tree     tests/pool/rt_tree.extc     "live0=1 in=2 depth=2 after=1 depth=1"
rt_run rt_zone     tests/pool/rt_zone.extc     "depth=1 live=0 in=2/4 after-drop=3 after-reset=2 out=1/0"

# ---------------------------------------------------------------- zone 按需发射
# 钩子是**税**：每个嵌套块（含每个循环体）压/弹一次 zone，而绝大多数块根本不建池。
# 修法是给函数算一个传递摘要 `makesPool`（能不能直接/间接调到 `extc_pool_new`，最小不动点），
# 块只在这个摘要为真时压 zone。这两条判据把"该省的省了"和"该留的留着"同时钉住。
echo "== zone 按需发射 · 正例：不建池的循环体里不许有钩子（次数写死，理由在下面）=="
if "$EXTC" tests/pool/rt_zone_ondemand.extc -o build/rt_zone_ondemand.c >/dev/null 2>&1; then
    # 期望值 4 的来历（从这个用例的形状数出来，不是量出来的）：
    #   1) `main` 的帧压一次（`__extc_zm1`）—— 它自己调 `pool::extc_pool_new`，摘要为真；
    #   2) 建池的那个块压一次 —— 块自己建池；
    #   3) 那个块出块时弹一次 —— 配平（每个压过的层弹一次）；
    #   4) `main` 尾声弹回帧深度一次 —— 早退路径也走这里，所以这一行必须留着。
    # 修之前这里是 11 次（5 压 6 弹）：多出来的是三条只读元素的 while 循环体（各自压/弹）
    # 与不建池的库调用。`want_enter = 2` 正是"不建池的循环体不许有钩子"这条判据的抓手。
    want_enter=2
    want_leave=2
    got_enter=$(grep -c 'int64_t __extc_zm[0-9]* = extc_pool_zoneEnter()' build/rt_zone_ondemand.c)
    got_leave=$(grep -c 'extc_pool_zoneLeaveTo(__extc_zm[0-9]*)' build/rt_zone_ondemand.c)
    if [ "$got_enter" = "$want_enter" ] && [ "$got_leave" = "$want_leave" ]; then
        echo "  ok   rt_zone_ondemand  ->  zoneEnter ${got_enter} 次（期望 ${want_enter}）· zoneLeaveTo ${got_leave} 次（期望 ${want_leave}）⇒ 不建池的循环体没有钩子"
    else
        echo "  FAIL rt_zone_ondemand  ->  zoneEnter ${got_enter}（期望 ${want_enter}）· zoneLeaveTo ${got_leave}（期望 ${want_leave}）⇒ 钩子发多了（税没省掉）或发少了（池会漏登记）"
        fail=1
    fi
else
    echo "  FAIL rt_zone_ondemand  ->  编不过"; fail=1
fi

echo "== zone 按需发射 · 反例：在循环体里建池 ⇒ 那一层的钩子必须还在 =="
rt_run rt_zone_inloop tests/pool/rt_zone_inloop.extc "total=6 live=0 depth=1"
if "$EXTC" tests/pool/rt_zone_inloop.extc -o build/rt_zone_inloop.c >/dev/null 2>&1; then
    got_enter=$(grep -c 'int64_t __extc_zm[0-9]* = extc_pool_zoneEnter()' build/rt_zone_inloop.c)
    if [ "$got_enter" = 2 ]; then
        echo "  ok   rt_zone_inloop   ->  zoneEnter ${got_enter} 次 ⇒ 循环体那一层还在（帧 + 循环体）"
    else
        echo "  FAIL rt_zone_inloop   ->  zoneEnter ${got_enter} 次（期望 2）⇒ 循环体建池却没有自己的 zone，池会漏登记"; fail=1
    fi
else
    echo "  FAIL rt_zone_inloop   ->  编不过"; fail=1
fi

echo "== 期 1 · 槽位复用：同一个槽位号，世代 +1 =="
rt_run rt_reuse    tests/pool/rt_reuse.extc    "same=true gen=1->2"

echo "== 期 1 · 容器接入：池随容器创建，release 整批还，出块整区走 =="
rt_run rt_container tests/pool/rt_container.extc "live=0 made=1 closed=0 again=1 out=0"

echo "== 期 1 · 拷贝 + 槽位复用：陈旧拷贝不许放掉别人的池（pidGen）=="
rt_run rt_pidgen  tests/pool/rt_pidgen.extc  "live=1 c-len=0"

echo "== 容量工具：shrink 降高水位，数据与 handle 全部继续有效 =="
rt_run rt_shrink  tests/pool/rt_shrink.extc  "cap=1024->8 len=3 vals=10,20,30 again=40 len=4"

echo "== 板块：池自己的内存（拿一块 · 原块上长 · 还一块 · 字节账 · drop 归零）=="
rt_run rt_plate    tests/pool/plate.extc       "take=800 zero=0 wrote=7 bytes=800 grew=1200 kept=7 tail0=0 dual=1264 gave=1 aftergive=64 live=0"

echo "== 容器接线：一个容器 = 一棵池树（容器自己的池 + 值池以它为父）=="
rt_run rt_ctree    tests/pool/rt_container_tree.extc "live=2 len=2 v=22 after=0"

echo "== 提权的运行期落点：池生在外层那个地方 ⇒ 内层块退出带不走它 =="
rt_run rt_promote  tests/pool/rt_promote.extc  "in=2 out=1 use=42 live=1"

echo "== 池的提权（容器那一档）：循环里建内层、推入外层 ⇒ 内层池生在外层那个地方 =="
rt_run rt_nest     tests/pool/rt_nest_promote.extc "after=4 innerlen=2 v=20 bytes=240"

echo "== 池的提权（返回那一档）：函数里建好的容器返回给调用方 =="
rt_run rt_rett     tests/pool/rt_return_promote.extc "live=1 len=3 v0=5 v2=7"

echo "== 染色（§7.2/§7.3）：同一个下标每一轮的颜色都不同 ⇒ 翻一位作废整棵树 =="
rt_run rt_color    tests/pool/rt_color.extc    "c1=1 c2=2 c3=3 diff=1,1 birth_eq_row=1 live=0"

echo "== 守卫（别名那一档）：一份 release 之后另一份不许静默写已释放的板块 =="
if out=$("./build/extc" --run tests/pool/rt_stale_alias.extc 2>&1); rc=$?; then :; fi
if [ "${rc:-0}" = 1 ] && printf '%s' "$out" | grep -q "trap: index" && printf '%s' "$out" | grep -q "tests/pool/rt_stale_alias.extc:"; then
    echo "  ok   rt_alias   ->  $(printf '%s' "$out" | head -1 | cut -c1-76)"
else
    echo "  FAIL rt_alias   ->  期望带位置的下标 trap + 退出码 1，得到 rc=${rc:-?}：$(printf '%s' "$out" | head -1)"; fail=1
fi

echo "== 池级 epoch：clear 是 O(1) 染色（旧句柄整体失配 · 再插不涨容量 · 同 epoch 复用同槽仍失配）=="
rt_run rt_epoch     tests/pool/rt_epoch.extc     "cap=8 stale=0 len=0 refill=3 cap2=8 live=3 d=7,8,9 removed=1 reuse=0 fresh=1"

echo "== 期 1 · 生成物：-Wall -Wextra -Werror（gcc 与 clang）+ ASan 含泄漏检查 =="
TMP2=$(mktemp -d)
if "$EXTC" tests/pool/rt_churn.extc -o "$TMP2/rt.c" >/dev/null 2>&1; then
    if gcc -fwrapv -std=c11 -Wall -Wextra -Werror -o "$TMP2/rt" "$TMP2/rt.c" >/dev/null 2>&1; then
        echo "  ok   gcc -Wall -Wextra -Werror 编得过"
    else
        echo "  FAIL 生成的 C 在 -Werror 下编不过"; fail=1
    fi
    if command -v clang >/dev/null 2>&1; then
        if clang -fwrapv -std=c11 -Wall -Wextra -Werror -c -o /dev/null "$TMP2/rt.c" >/dev/null 2>&1; then
            echo "  ok   clang -Wall -Wextra -Werror 也干净"
        else
            echo "  FAIL clang 报了"; fail=1
        fi
    fi
    if gcc -fwrapv -std=c11 -g -fsanitize=address -o "$TMP2/rt_asan" "$TMP2/rt.c" >/dev/null 2>&1 \
       && ! "$TMP2/rt_asan" 2>&1 | grep -q Sanitizer; then
        echo "  ok   ASan（含泄漏检查）干净"
    else
        echo "  FAIL ASan 报了"; fail=1
    fi
else
    echo "  FAIL 生成失败"; fail=1
fi
rm -rf "$TMP2"

# ---------------------------------------------------------------- 染色这一关的两条实测结论
# 染色（POOLS.md §7）三步的结论都在 bench/ 里量过，但那不够 —— **结论也要有常设判据**，
# 否则将来有人改了 zone 生命周期或池板块的归属，这两条会静默失效。这里把它们钉住。
echo
echo "== 染色②的结论：地方退出的代价随池数走得**平**（每池约 10 ns，板块 malloc/free 占 7 ns）=="
# 形状：每轮 = 一个地方；地方里建 N 个池各拿一块小板块后出块。**总操作数固定 3e7**，
# 所以可以跨 N 直接比"每次建+退"的纳秒数。判据：最小与最大之比 ≤ 1.5。
#   若有人按 §7.4-1 去改注册表（期望收益 2~3 ns/池），这条会告诉他收益上限；若改出回归
#   （例如 zone 退出退化成 O(池数) 之外的额外结构开销），小 N 会先翘起来 ⇒ 判据响。
build_one zt-exit zt_exit_cost && build_one zt-shrink zt_shrink_rss
if [ -x build/zt-exit ]; then
    ns_min=""; ns_max=""; line=""
    for n in 500 5000 50000; do
        b=999
        for _ in 1 2 3; do
            t=$( { /usr/bin/time -f "%e" ./build/zt-exit "$n" 30000000 >/dev/null; } 2>&1 | tail -1 )
            b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
        done
        v=$(awk -v t="$b" 'BEGIN{printf "%.2f", t*1e9/3e7}')
        line="$line N=$n:${v}ns"
        if [ -z "$ns_min" ]; then ns_min=$v; ns_max=$v; else
            ns_min=$(awk -v a="$v" -v b="$ns_min" 'BEGIN{print (a<b)?a:b}')
            ns_max=$(awk -v a="$v" -v b="$ns_max" 'BEGIN{print (a>b)?a:b}')
        fi
    done
    if awk -v a="$ns_min" -v b="$ns_max" 'BEGIN{exit !(b <= a*1.5 + 0.5)}'; then
        echo "  ok   地方退出 -> ${line# } ⇒ 每次建+退约 ${ns_min}~${ns_max} ns，**平** ✓"
    else
        echo "  FAIL 地方退出 -> ${line# } ⇒ 随池数翘了（${ns_min} → ${ns_max} ns/次）✗"; fail=1
    fi
else
    echo "  FAIL 地方退出 -> 编不过"; fail=1
fi

echo "== 染色③的结论：真收缩（800 KB × 2000 轮，累计 1.6 GB）峰值 RSS 必须平、字节账归零 =="
if [ -x build/zt-shrink ]; then
    out=$(./build/zt-shrink)
    pk=$( { /usr/bin/time -f "%M" ./build/zt-shrink >/dev/null; } 2>&1 | tail -1 )
    if [ "$pk" -le 8192 ] && echo "$out" | grep -q "live=0 bytes=0"; then
        echo "  ok   真收缩   ->  峰值 ${pk} KB（累计分配 1.6 GB）· $out ⇒ 板块全还、字节账归零 ✓"
    else
        echo "  FAIL 真收缩   ->  峰值 ${pk} KB（期望 ≤ 8192）· $out（期望含 live=0 bytes=0）✗"; fail=1
    fi
else
    echo "  FAIL 真收缩   -> 编不过"; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
