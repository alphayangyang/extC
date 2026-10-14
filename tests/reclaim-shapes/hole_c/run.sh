#!/usr/bin/env bash
# tests/reclaim-shapes/hole_c/run.sh —— **C 洞的绿判据**（先赤后修；这一支现在是门禁）。
#
# 洞：清扫的静态根表是**词法作用域**的（本帧局部 + 形参 + 全局），**看不见调用栈上其他帧的
# 活引用**。内层帧给**外层区域**（家 arena，调用者一路传下来的同一只）分配 ⇒ 用内层的根表扫
# 外层的区域 ⇒ 种子不完整 ⇒ 把外层帧还持有的对象当垃圾放掉 ⇒ 真悬垂 UAF。
#
# 本目录三条形状（都在这里跑）：
#   · `hole_c.extc`      最小递归：外层调用帧的 `t` 连着已建好的子树，内层帧的触发根表是空的
#                        （`maybeCollect(区域, (void *const *)0, 0)`）⇒ 整棵放掉。
#   · `caller_frame.extc` 同一个洞的**非递归**形态：`main` 手里的链头是活引用，被调 `fresh()`
#                        只负责 `new`。修之前症状是挂住（rc=124）。
#   · 真实语料那一档（`bench/oi/persist/p3834_nodes.extc` 的最小化版）见
#                        `tests/reclaim-shapes/persist_nodes.extc` + `run-switch.sh` ①i。
#
# 修法（(a) 立刻健全，见 `src/back/codegen.c` 的「第三个安全方向」）：触发点只在**本帧拥有**的
# 区域上下手 —— 家 arena / 线程本地 arena 上一个触发都不发；本帧自己的块 arena 仍照常扫
# （外层帧拿不到指向它的引用），所以清扫能力没有被整体废掉（护栏见 `run-switch.sh` 与 README）。
#
# 开关口径（`reclaim-default` 分支起）：回收**默认打开**；`EXTC_RECLAIM=1` 是"**允许**回收
# （不强制发射）"—— ②（"这个区域不需要回收"的静态证明）判 idle 的单元**整段不发射 rc 运行期**
# 是合格且更强的结果，不是失败；`EXTC_RECLAIM=0` 关掉。各档都跑、都要**输出正确**
# （关档那一档是"不空转"的对照：它同样要生成、编译、运行、断言逐字输出）。
#
# 判据：
#   ① 开档（`EXTC_RECLAIM=1`）：`hole_c` rc=0 且输出逐字 `n = 131071`（修之前 rc=139）。
#      这一格接受两种结果：**(i)** 发了运行期（生成物里有 `extc_rc_alloc`）⇒ 就是上面那条；
#      **(ii)** ②生效（生成物里 0 个 rc 调用点）⇒ 等价断言：输出逐字正确（本格）、与关档
#      **逐字节相同**（⑧a）、ASan 干净（②）、峰值 N vs 4N 有界（⑧b）。
#   ② 开档 + ASan：rc=0，且报告里**没有** SEGV / heap-use-after-free；
#   ③ 关档（`EXTC_RECLAIM=0`）：rc=0 且输出逐字 `n = 131071`（对照组，不空转）；
#   ④ `caller_frame` 开档：rc=0（不挂住）且输出逐字 `n = 70000`；
#   ⑤ `caller_frame` 关档：rc=0 且输出逐字 `n = 70000`；
#   ⑥ 生效位置（生成物层面）：开档的 `hole_c` **分配点**里不发射 `extc_rc_maybeCollect`
#      —— 那两处分配都在家 arena 上，内层帧一个触发都不该发。子集 B 之后它们改成在区域上
#      记一笔「欠一次收集」（`extc_rc_owe`），由**拥有那只区域**的一帧在自己的调用点上用
#      **自己完整的静态根表**销账（`extc_rc_checkOwed`）⇒ 本条判据仍成立（分配点上没有触发）。
#      ②生效时这一格平凡成立（一个 rc 调用点都没有），照跑不误。
#   ⑦ 前提健全性：深度 15（65535 节点 = 1.0 MiB − 16 B）没过 `EXTC_RC_FLOOR`(1 MiB) ⇒ 开档
#      也不该崩、输出 `n = 65535`。这条钉住"① 的复现需要越过压力线"这个前提。
#   ⑧ ②生效时的**等价断言**（只在②生效那一支判；今天就是）：
#      a 开档产物与关档产物**逐字节相同**（⇒ 与无回收基线同一份代码）；
#      b 同一形状**循环里反复建/丢**一棵深度 16 的树，`N=2` vs `N=8`（4×）峰值比 < 1.5×
#        —— hole_c 自己没有规模旋钮（深度是 16），所以用这个变体量"界与轮数无关"。
#
# 用法：bash tests/reclaim-shapes/hole_c/run.sh          exit 0 = 绿（洞不在了）
#       EXTC=/path/to/extc bash tests/reclaim-shapes/hole_c/run.sh
set -u
cd "$(dirname "$0")/../../.."          # 仓库根

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
SRC=tests/reclaim-shapes/hole_c/hole_c.extc
CF=tests/reclaim-shapes/hole_c/caller_frame.extc
WANT="n = 131071"                      # 2^17 - 1 个节点（深度 16 的满二叉树）
CF_WANT="n = 70000"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

[ -x "$EXTC" ] || { echo "缺少可执行编译器 $EXTC（先 make）"; exit 2; }

gen() { # <开|关> <tag> <源文件> —— 生成 C（开档强制 EXTC_RECLAIM=1；关档 =0）
    if [ "$1" = 开 ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$3" -o "$TMP/$2.c" >"$TMP/$2.gen.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "$3" -o "$TMP/$2.c" >"$TMP/$2.gen.log" 2>&1
    fi
}
cc_build() { # <tag> [asan] —— 生成物 $TMP/<tag>.c → 可执行 $TMP/<tag>
    if [ "${2-}" = asan ]; then
        "$CC" -std=c11 -fwrapv -O1 -g -fsanitize=address -o "$TMP/$1" "$TMP/$1.c" \
            >"$TMP/$1.cc.log" 2>&1
    else
        "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$1" "$TMP/$1.c" >"$TMP/$1.cc.log" 2>&1
    fi
}
gen_ok() { grep -q 'extc_rc_alloc' "$TMP/$1.c" 2>/dev/null; }
run() { # <tag> [asan] —— 跑；套一层 `sh -c` 把 bash 的 "Segmentation fault" 噪声留在子 shell 里
    if [ "${2-}" = asan ]; then
        ASAN_OPTIONS=detect_leaks=0 sh -c '"$0" >"$1" 2>"$2"' \
            "$TMP/$1" "$TMP/$1.out" "$TMP/$1.err" 2>/dev/null
    else
        sh -c '"$0" >"$1" 2>"$2"' "$TMP/$1" "$TMP/$1.out" "$TMP/$1.err" 2>/dev/null
    fi
    R_RC=$?
    R_OUT=$(cat "$TMP/$1.out")
}

echo "== hole_c：清扫不得在种子不完整时下手（本帧拥有的区域才触发）=="
echo "   形状 $SRC / $CF；期望 开档=绿（修之前 rc=139 / 挂住）/ 关档=绿"

# ── ① 开档：非 ASan ──────────────────────────────────────────────────────────
# 接受两种结果：(i) 发了运行期 ⇒ 今天的断言；(ii) ②生效（0 个 rc 调用点）⇒ 等价断言
# （输出逐字 + ⑧a 逐字节相同 + ②的 ASan + ⑧b 的 N vs 4N 界）。②生效**不是失败**。
# 调用点口径：**数调用点，不数行** —— 只认用户区才有的实参形状（运行期每个原语的定义行
# 参数是具名变量，不匹配），免得把 `static` 定义算成调用。
rc_calls() { grep -oE 'extc_rc_(initRef|initRefNoHold|storeRefOwnedReg|storeRefSameRegion|storeRefOwned|storeRefUnknown)\(\(void \*\*\)&|extc_rc_(alloc|maybeCollect)\(__extc_rcr' "$1" 2>/dev/null | wc -l | tr -d ' '; }
if gen 开 on "$SRC" && cc_build on; then
    run on; on_rc=$R_RC; on_out=$R_OUT
    on_calls=$(rc_calls "$TMP/on.c")
    if [ "$on_rc" = 0 ] && [ "$on_out" = "$WANT" ]; then
        if gen_ok on; then
            ok "开档（EXTC_RECLAIM=1）：rc=0 out='$on_out' —— 外层帧的活引用没被放掉"
        elif [ "$on_calls" = 0 ]; then
            ok "开档（EXTC_RECLAIM=1）：②生效——整段不发射 rc 运行期（用户区 0 个 rc 调用点）；等价断言：输出逐字 '$on_out' == 关档（③）+ 产物与关档逐字节相同（⑧a）+ ASan 干净（②）+ N vs 4N 峰值有界（⑧b）"
        else
            bad "开档：生成物里有 $on_calls 个 rc 调用点，但没有运行期分配点（生成物形态自相矛盾）"
        fi
    else
        bad "开档：rc=$on_rc out='$on_out'（期望 rc=0 / '$WANT'）—— 提前释放仍在（或②生效时输出错）"
    fi
else
    bad "开档：生成或编译失败"
    sed 's/^/       /' "$TMP/on.gen.log" "$TMP/on.cc.log" 2>/dev/null | head -6
fi

# ── ② 开档 + ASan ───────────────────────────────────────────────────────────
if gen 开 ona "$SRC" && cc_build ona asan; then
    run ona asan; asan_rc=$R_RC
    if [ "$asan_rc" = 0 ] && ! grep -qE 'SEGV|heap-use-after-free|DEADLYSIGNAL|AddressSanitizer: (stack|global)' "$TMP/ona.err"; then
        ok "开档（ASan）：rc=0，无 SEGV / UAF 报告"
    else
        bad "开档（ASan）：rc=$asan_rc，报告里有 SEGV/UAF —— 悬垂仍在"
        grep -m2 -E 'SEGV on unknown address|heap-use-after-free|SUMMARY: AddressSanitizer' "$TMP/ona.err" \
            | sed 's/^/       /'
    fi
else
    bad "开档（ASan）：生成或编译失败"
fi

# ── ③ 关档：对照组必须绿（不空转：同样生成/编译/运行/逐字比输出） ─────────────
if gen 关 off "$SRC" && cc_build off; then
    run off; off_rc=$R_RC; off_out=$R_OUT
    if [ "$off_rc" = 0 ] && [ "$off_out" = "$WANT" ]; then
        ok "关档（EXTC_RECLAIM=0）：rc=0 out='$off_out' —— 对照组绿（判据不空转）"
    else
        bad "关档：rc=$off_rc out='$off_out'（期望 rc=0 / '$WANT'）"
    fi
else
    bad "关档：生成或编译失败"
fi

# ── ④⑤ caller_frame：非递归形态（修之前挂住 rc=124） ────────────────────────
if gen 开 cf "$CF" && cc_build cf; then
    timeout 20 sh -c '"$0" >"$1" 2>"$2"' "$TMP/cf" "$TMP/cf.out" "$TMP/cf.err" 2>/dev/null
    cf_rc=$?; cf_out=$(cat "$TMP/cf.out")
    if [ "$cf_rc" = 0 ] && [ "$cf_out" = "$CF_WANT" ]; then
        ok "caller_frame 开档：rc=0 out='$cf_out' —— 主调帧的活引用没被放掉（修之前 rc=124 挂住）"
    else
        bad "caller_frame 开档：rc=$cf_rc out='$cf_out'（期望 rc=0 / '$CF_WANT'）"
    fi
else
    bad "caller_frame 开档：生成或编译失败"
fi

if gen 关 cfo "$CF" && cc_build cfo; then
    timeout 20 sh -c '"$0" >"$1" 2>"$2"' "$TMP/cfo" "$TMP/cfo.out" "$TMP/cfo.err" 2>/dev/null
    cfo_rc=$?; cfo_out=$(cat "$TMP/cfo.out")
    if [ "$cfo_rc" = 0 ] && [ "$cfo_out" = "$CF_WANT" ]; then
        ok "caller_frame 关档：rc=0 out='$cfo_out' —— 对照组绿（判据不空转）"
    else
        bad "caller_frame 关档：rc=$cfo_rc out='$cfo_out'（期望 rc=0 / '$CF_WANT'）"
    fi
else
    bad "caller_frame 关档：生成或编译失败"
fi

# ── ⑥ 生效位置：开档生成物里一个触发都不发（两处分配都在家 arena 上） ────────
if [ -f "$TMP/on.c" ]; then
    ntrig=$(grep -c 'extc_rc_maybeCollect(__extc_' "$TMP/on.c" 2>/dev/null || true)
    if [ "${ntrig:-1}" = 0 ]; then
        ok "生成物：开档的 hole_c 分配点不发射清扫触发（家 arena 上的分配只记欠收，销账在拥有者帧）"
    else
        bad "生成物：开档的 hole_c 仍发射了 $ntrig 处清扫触发 —— 家 arena 上的触发没被挡住"
        grep -o 'extc_rc_regionOf([^,]*, [-0-9]*)' "$TMP/on.c" | sort -u | sed 's/^/       /' | head -4
    fi
else
    bad "生成物：没有开档生成物可查"
fi

# ── ⑦ 前提健全性：深度 15 没过 1 MiB 压力下限 ⇒ 开档也不该崩 ─────────────────
sed 's/build(16)/build(15)/' "$SRC" >"$TMP/d15.extc"
if gen 开 d15 "$TMP/d15.extc" && cc_build d15; then
    run d15; d15_rc=$R_RC; d15_out=$R_OUT
    if [ "$d15_rc" = 0 ] && [ "$d15_out" = "n = 65535" ]; then
        ok "开档 + 深度 15（65535 节点 = 1.0 MiB - 16 B）：rc=0 —— 触发点由 1 MiB 下限把关"
    else
        bad "开档 + 深度 15：rc=$d15_rc out='$d15_out'（期望 rc=0 / 'n = 65535'）"
    fi
else
    bad "开档 + 深度 15：生成或编译失败"
fi

# ── ⑧ ②生效时的**等价断言** ───────────────────────────────────────────────────
# ⑧a 开档产物与关档产物逐字节相同 ⇒ 与无回收基线同一份代码（②生效的强证据）。
if [ -f "$TMP/on.c" ] && [ -f "$TMP/off.c" ]; then
    if [ "$(rc_calls "$TMP/on.c")" = 0 ]; then
        if cmp -s "$TMP/on.c" "$TMP/off.c"; then
            ok "⑧a ②生效：开档产物与关档（EXTC_RECLAIM=0）逐字节相同 ⇒ 与无回收基线同一份代码"
        else
            bad "⑧a ②生效（用户区 0 个 rc 调用点）但开档产物与关档不同 —— 生成物形态自相矛盾"
        fi
    else
        echo '  --   ⑧a 开档发了运行期，不作「逐字节相同」断言（开/关两档本就不同）'
    fi
else
    bad "⑧a 缺少开档/关档生成物"
fi

# ⑧b 界与轮数无关：同一形状**循环里反复建/丢**一棵深度 16 的树，N=2 vs N=8（4×）峰值比 < 1.5×。
# hole_c 自己没有规模旋钮（深度固定 16），所以用这个变体量这件事；两档（②生效 / 发运行期）都判。
TIME=${TIME:-/usr/bin/time}
peak_kb() { "$TIME" -f %M "$1" 2>"$TMP/peak.err" >/dev/null; tail -1 "$TMP/peak.err"; }
cat > "$TMP/hc_loop.extc" <<'EXTCEOF'
struct tree {
    left: ?ref tree
    right: ?ref tree
}

fn build(d: i32) -> mut ref tree {
    var t: mut ref tree = new tree
    if d > 0 { t.left = build(d - 1)  t.right = build(d - 1) }
    return t
}

fn count(t: ?ref tree) -> i64 {
    if t == null { return 0 }
    return 1 + count(t.left) + count(t.right)
}

fn main() {
    var k: i64 = 0
    var acc: i64 = 0
    while k < @N@ {
        var t: mut ref tree = build(16)
        acc = acc + count(t)
        k = k + 1
    }
    println("n = ", acc)
}
EXTCEOF
hl_bad=""
for hn in 2 8; do
    want_n=$((131071 * hn))
    sed "s/@N@/$hn/" "$TMP/hc_loop.extc" >"$TMP/hc_loop$hn.extc"
    if gen 开 "hl$hn" "$TMP/hc_loop$hn.extc" && cc_build "hl$hn"; then
        run "hl$hn"
        if [ "$R_RC" != 0 ] || [ "$R_OUT" != "n = $want_n" ]; then
            hl_bad="hl$hn 运行 rc=$R_RC out='$R_OUT'（期望 rc=0 / 'n = $want_n'）"
        fi
        eval "hl_rss_$hn=\$(peak_kb \"\$TMP/hl$hn\")"
    else
        hl_bad="hl$hn 生成或编译失败"
    fi
done
if [ -n "$hl_bad" ]; then
    bad "⑧b 循环建丢变体：$hl_bad"
else
    hl_r2=${hl_rss_2:-0}; hl_r8=${hl_rss_8:-0}
    hl_ratio=$(awk -v a="$hl_r8" -v b="$hl_r2" 'BEGIN{ if (b+0==0) printf "—"; else printf "%.2f", a/b }')
    if awk -v r="$hl_ratio" 'BEGIN{ exit !(r+0 < 1.5) }'; then
        ok "⑧b 界与轮数无关：循环里反复建/丢 depth-16 树，N=2 → N=8 峰值 $((${hl_r2}/1024)) MB → $((${hl_r8}/1024)) MB（${hl_ratio}× < 1.5×）"
    else
        bad "⑧b 循环建丢变体峰值随轮数涨：$((${hl_r2}/1024)) MB → $((${hl_r8}/1024)) MB（${hl_ratio}× ≥ 1.5×）"
    fi
fi

echo
echo "判据：通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
