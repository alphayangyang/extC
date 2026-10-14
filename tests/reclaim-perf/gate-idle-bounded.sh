#!/usr/bin/env bash
# tests/reclaim-perf/gate-idle-bounded.sh —— **② 判据的内存界**（「区域不需要回收 ⇒ 整段不发射
# rc 运行期」的安全网）。
#
# 为什么要有这条判据：②的失效方向是**内存涨**（rc 只释放标记不到的对象，关掉它只会释放更少），
# 而且**开/关档峰值比看不出这件事** —— ②生效时"开档"就是"关档"，两个产物逐字节相同、比值恒
# 1.00×。真正该问的是：**被 ② 判成 idle 的形状，峰值随规模涨不涨**。
#
# 判据（三格 + 两格反空跑）：
#   ① 每个列出形状：默认档产物与 `EXTC_RECLAIM=0` 逐字节相同（= ②生效，且 rc 记号 0 个）⇒
#      量 `N` 与 `4N` 的**峰值 RSS 比** 必须 < `BOUND`（默认 1.5×）**且** 4N 的绝对峰值
#      ≤ `PEAK_KB_MAX`（默认 8 MiB）。
#   ② 至少 2 个形状真被判成 idle —— 否则 ② 没生效/判据退化，这条判据此刻没有牙。
#   ③ **对照形状**（`cyclic_pressure`）**必须仍在回收档** —— 防"判据瞎了把什么都判 idle"。
#      另一条对照 `outer_alloc_inner_write` 钉的是 (a′) 的**循环深度条件**：宿主在外层循环建、
#      站点在内层循环写 ⇒ 同一个对象被每轮内层改写 ⇒ **必须仍在回收档**（去掉那条条件它就
#      会被判 idle，实测 N vs 4N 2.09×）。它同时也在 §① 的名单里：一旦被误判成 idle，比值
#      断言会再补一枪。
#
# 没被 ② 判成 idle 的形状只打印比值：它们仍在回收档，有界性由 rc 那一侧的判据管。
#
# 这条判据是**实测抓出来的**：初版判据漏了"带引用的值聚合只当临时量用"那一档，`byval_temp`
# 被误判 idle，峰值 N=50k/200k/800k 从 2.2 MB 线性涨到 14.2 MB（开档平在 4.9 MB）。补上那条
# 判据后它回到回收档、峰值重新有界。这条判据进电池，就是为了让那种回归**当场红**。
#
# 验牙：注入 `reclaim-idle-always`（把判定强制为真）⇒ 本不该 idle 的形状也被判 idle ⇒ 它们的
# N vs 4N 比当场超线 ⇒ 本判据红（同时 `gate-reclaim-switch` 也红）。
#
# `rebuild_per_round` 与 `closure_chain` 是细化 (a′)+(b′)（"循环里建、每轮丢"）的安全网：它们
# 必须在这里**有界**（N vs 4N < 1.5× + rc 记号 0 个）。判据的三条"绊线"——`cross_scope_cycle`、
# `scale_growth`、`live_churn`——在细化面前**必须仍留在回收档**（下面 §对照之外另有
# `gate-reclaim-switch` 的形状棘轮；`cross_scope_cycle` 会作为对照之一出现在那里）。
#
# 档位：进 `nobench`（不进 `smoke`）—— 名单里的形状 × 两种规模，约十几秒；`smoke` 是 ≈10s 的高信号
# 集，这条属于"② 落地后必须常驻"的判据，放 gate 组。
#
# ── 活集本身就是 O(N) 的形状：`N vs 4N < 1.5` **不适用**，用"同规模 idle ≤ 回收档"─────────────
# `persist_nodes`（以及它最小化来源的真实语料 `bench/oi/persist/p3834_nodes.extc`）是**可持久化
# 线段树**：每个版本的根都留在 `root[]` 里，`update` 造出来的节点全都被某些版本指着 ⇒ **活集就是
# O(N)**，峰值天生随 N 线性涨（实测 idle 档 5k→20k = 2.73×、回收档 2.91×）——`N vs 4N < 1.5`
# 对它没有意义。可用的判据是"**同规模下 idle 档的峰值 ≤ 回收档的峰值**"：
# 回收档峰值是同规模下"②没省掉任何东西"的参照，idle 档若把垃圾攒起来（②判错的方向），
# 就会超过它 ⇒ 红。
#
# 口径：`ABS_SHAPES` 每行的上限 KB = **该形状在自带规模上、回收档编译器的实测峰值**（本机
# `taskset -c 6`、`/usr/bin/time -f %M`），测量条件写在行尾注释里。
#   · persist_nodes：回收档 32172 KB（pin `5a0c0f07…`，N=50000 自带规模）；idle 实测 21996 KB。
#   · p3834_nodes（真实语料）：回收档 765864 KB；idle 实测 508332 KB（墙钟 4.33 s → 3.18 s）。
# 若形状/规模变了，这两个数要跟着重测（它们**不是**"永远成立的常量"，是带条件的实测值）。
ABS_SHAPES="
persist_nodes|tests/reclaim-shapes/persist_nodes.extc|32172
p3834_nodes|bench/oi/persist/p3834_nodes.extc|765864
"

# 用法：tests/reclaim-perf/gate-idle-bounded.sh
#   EXTC=… / CC=… / TIME=…   换编译器 / C 编译器 / 取 RSS 的工具
#   N_IDLE=…                 基准规模（4N = 4×N）
#   BOUND=…                  峰值比上限（默认 1.5）
#   PEAK_KB_MAX=…            4N 的绝对峰值上限（KiB，默认 8192）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
N=${N_IDLE:-50000}
N4=$((N * 4))
BOUND=${BOUND:-1.5}
PEAK_KB_MAX=${PEAK_KB_MAX:-8192}

SHAPES="
blind_localhold|tests/reclaim-acyclic/blind_localhold.extc
bounded_churn|tests/reclaim-shapes/bounded_churn.extc
byval_temp|tests/reclaim-gaps/shapes/byval_temp.extc
struct_copy_ret_home|tests/reclaim-gaps/shapes/struct_copy_ret_home.extc
rebuild_per_round|tests/reclaim-perf/shapes/rebuild_per_round.extc
closure_chain|tests/reclaim-gaps/shapes/closure_chain.extc
outer_alloc_inner_write|tests/reclaim-perf/shapes/outer_alloc_inner_write.extc
"
# 必须**仍在回收档**的对照（防"判据瞎了"）。`outer_alloc_inner_write` 是 (a′) 循环深度条件的
# 常驻牙：宿主建在外层循环、站点写在内层循环 ⇒ 同一个对象被反复改写 ⇒ 判 idle 就是不健全。
# `byval_temp` 是 **F 细化的边界**（task-36）：它的 F 站点在**循环里**（`take(mk(i))`），
# 判 idle 会让峰值 N vs 4N = 2.12×（实测）⇒ 必须仍在回收档。
# `field_overwrite_lost_root` / `fixed_slot_overwrite` 是 **task-38 三条窄化各自的对照**（实测抓出来的：
# 现成的六条对照对这三发牙**一条都不红** ⇒ 判据本来没有牙）：
#   · 前者：细化 A（"旧值另有根"）若被打回"整条 A 放开" ⇒ 它被判 idle，峰值 20k→80k = **1.52×**（实测）；
#   · 后者：细化 E（"下标在本循环体内被赋值"）若被打回"任意元素目的地" ⇒ 它被判 idle，比值 **1.56×**（实测）。
# 两条都必须留在回收档。
CONTROLS="
cyclic_pressure|tests/reclaim-shapes/cyclic_pressure.extc
outer_alloc_inner_write|tests/reclaim-perf/shapes/outer_alloc_inner_write.extc
byval_temp|tests/reclaim-gaps/shapes/byval_temp.extc
field_overwrite_lost_root|tests/reclaim-perf/shapes/field_overwrite_lost_root.extc
fixed_slot_overwrite|tests/reclaim-perf/shapes/fixed_slot_overwrite.extc
"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== ② 的内存界（判成 idle 的形状峰值必须 N vs 4N < ${BOUND}×、4N 绝对峰值 ≤ $((PEAK_KB_MAX/1024)) MiB；N=$N）=="
echo "   口径：默认档产物与 EXTC_RECLAIM=0 逐字节相同（且 rc 记号 0 个）⇒ ②生效 ⇒ 判峰值"

gen() { # <源> <档 on|off> <输出.c> → 0/1
    if [ "$2" = off ]; then
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$3" >/dev/null 2>&1
    else
        "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$3" >/dev/null 2>&1
    fi
}

is_elided() { # <源> <标签> → 打印 yes/no
    gen "$1" on "$TMP/$2.c" && gen "$1" off "$TMP/$2.off.c" || { printf 'gen-fail'; return; }
    if cmp -s "$TMP/$2.c" "$TMP/$2.off.c" && ! grep -q 'extc_rc_' "$TMP/$2.c"; then printf yes
    else printf no; fi
}

rss_of() { # <二进制> <N> → 峰值 KiB（最后一行的 %M）
    "$TIME" -f %M "$1" "$2" 2>"$TMP/t.err" >/dev/null || return 1
    tail -1 "$TMP/t.err"
}

fail=0
elided=0
echo
for row in $SHAPES; do
    name=${row%%|*}; src=$ROOT/${row#*|}
    [ -f "$src" ] || { bad "$name：形状不存在（$src）"; fail=1; continue; }
    if ! gen "$src" on "$TMP/$name.c"; then bad "$name：默认档生成失败"; fail=1; continue; fi
    if ! gen "$src" off "$TMP/$name.off.c"; then bad "$name：关档生成失败"; fail=1; continue; fi
    e=no
    if cmp -s "$TMP/$name.c" "$TMP/$name.off.c" && ! grep -q 'extc_rc_' "$TMP/$name.c"; then
        e=yes; elided=$((elided + 1))
    fi
    if ! "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/$name" "$TMP/$name.c" \
            >"$TMP/$name.cc.log" 2>&1; then
        bad "$name：默认档生成物编不过（$(sed -n '1p' "$TMP/$name.cc.log")）"; fail=1; continue
    fi
    r_n=$(rss_of "$TMP/$name" "$N") || { bad "$name：N=$N 运行失败"; fail=1; continue; }
    r_4=$(rss_of "$TMP/$name" "$N4") || { bad "$name：N=$N4 运行失败"; fail=1; continue; }
    ratio=$(awk -v a="$r_4" -v b="$r_n" 'BEGIN{ if (b+0==0) printf "—"; else printf "%.2f", a/b }')
    if [ "$e" = yes ]; then
        if awk -v r="$ratio" -v lim="$BOUND" 'BEGIN{ exit !(r+0 < lim+0) }' \
           && [ "$r_4" -le "$PEAK_KB_MAX" ]; then
            ok "$name：②生效（产物 == 关档）+ 峰值 $((${r_n}/1024)) MB → $((${r_4}/1024)) MB（${ratio}× < ${BOUND}×，绝对 ≤ $((PEAK_KB_MAX/1024)) MiB）"
        else
            bad "$name：②生效但峰值随规模涨/超绝对界（$((${r_n}/1024)) MB → $((${r_4}/1024)) MB，${ratio}×；上限 ${BOUND}× / $((PEAK_KB_MAX/1024)) MiB）—— 判据把不该省的形状判成了 idle"
            fail=1
        fi
    else
        printf '  --   %-22s 仍在回收档（产物含 rc 运行期），只打印：峰值 %s MB → %s MB（%s×）\n' \
               "$name" "$((${r_n}/1024))" "$((${r_4}/1024))" "$ratio"
    fi
done

# ── ③ 对照形状：必须仍在回收档（防"判据瞎了"）────────────────────────────────────
for crow in $CONTROLS; do
    cname=${crow%%|*}; csrc=$ROOT/${crow#*|}
    ce=$(is_elided "$csrc" "$cname")
    if [ "$ce" = no ]; then
        ok "对照 $cname：仍在回收档 —— 判据没有一律放行"
    elif [ "$ce" = yes ]; then
        bad "对照 $cname：它**也被判成 idle** —— 判据瞎了（这条形状不会丢引用是假的）"
        fail=1
    else
        bad "对照 $cname：默认档/关档生成失败（$ce）—— 这条对照没量到，不算通过"
        fail=1
    fi
done

# ── ④ 活集 O(N) 的形状：同规模 idle ≤ 回收档（上限 = 回收档实测峰值）──────────────────────
echo
echo "== ④ 活集 O(N) 的形状：idle 档峰值 ≤ 回收档实测峰值（N vs 4N 对它们不适用，理由见文件头）=="
for row in $ABS_SHAPES; do
    aname=${row%%|*}; rest=${row#*|}; asrc=$ROOT/${rest%%|*}; alim=${rest##*|}
    [ -f "$asrc" ] || { bad "$aname：形状不存在（$asrc）"; fail=1; continue; }
    ae=$(is_elided "$asrc" "$aname")
    if [ "$ae" != yes ]; then
        bad "$aname：②没生效（产物含 rc 或与关档不同：$ae）—— 这条形状应当被判 idle"; fail=1; continue
    fi
    elided=$((elided + 1))
    if ! "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/$aname" "$TMP/$aname.c" \
            >"$TMP/$aname.cc.log" 2>&1; then
        bad "$aname：生成物编不过（$(sed -n '1p' "$TMP/$aname.cc.log")）"; fail=1; continue
    fi
    apk=$(rss_of "$TMP/$aname" "$N") || { bad "$aname：运行失败"; fail=1; continue; }
    if [ "$apk" -le "$alim" ]; then
        ok "$aname：②生效 + 峰值 $((${apk}/1024)) MB ≤ 回收档实测 $((${alim}/1024)) MB（同规模对照）"
    else
        bad "$aname：②生效但峰值 $((${apk}/1024)) MB **超过**回收档实测 $((${alim}/1024)) MB —— ②把垃圾攒起来了（判错方向）"
        fail=1
    fi
done

echo
if [ "$elided" -ge 2 ]; then
    ok "反空跑：$elided 个形状真被判成 idle（② 在编译器里生效）"
else
    bad "反空跑：只有 $elided 个形状被判成 idle（要 ≥2）—— ② 没生效或判据退化，这条判据此刻没有牙"
    fail=1
fi
echo "  （没被 ② 判成 idle 的形状不在这里判有界性：它们仍在回收档，由 rc 那一侧的判据管）"
exit "$fail"
