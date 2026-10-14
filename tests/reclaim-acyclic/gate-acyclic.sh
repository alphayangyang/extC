#!/usr/bin/env bash
# manual: 事件档（计数开 + 清扫关）是**诊断档**且已知不完备 —— 实测它在语料里没有一个能赢的
# 形状（有环形状上不成立 / 无环形状上会漏 / 就算不漏，计数也比清扫贵 +100%~+455%）。
# 三条都在 `tests/reclaim-perf/TAXMAP.md` 第 7 节。所以本脚本**摘出电池**，留作手工诊断；
# 机制代码与语料保留（`EXTC_RC_SWEEPFREE` 仍是默认关的诊断开关）。
# tests/reclaim-acyclic/gate-acyclic.sh —— 「无环区域用事件回收（计数）、关掉清扫」这条改动的
# **动态判据**（原本挂 `check.sh`/`nobench` 的 `gate-acyclic` 项，现已摘出）。
#
# 背景（已经实测过的两段）：原型 `prototype-reclaim/README.md` 的 `ev` 档 = **0 次清扫** /
# 0 停顿 / 内存 12 KiB；而今天默认档同时清扫（事件路径 + 全局清扫两份钱）。改动方向：
# 程序被判定**无环**时，用计数（事件路径）并让 `shouldSweep` 恒为 0 ⇒ **清扫次数必须为 0**。
# 危险方向反过来：**判错了** ⇒ 有环区域的环垃圾永远收不掉 ⇒ **峰值 RSS 涨**（不是悬垂）。
#
# 所以本门禁钉的是三件事（外加一条盲点棘轮）：
#   ① **成功判据**：无环形状 + 事件档 ⇒ 清扫 0 次、stdout 与 baseline 逐字相同、
#      峰值 RSS ≤ 同形状**默认档** RSS × 1.5；
#   ② **上界判据**：有环形状（`cyclic_pressure`）**不得**被判成事件档 ⇒ 它的清扫次数必须 > 0
#      （否则判据瞎了）；旁证是那个开关**非空转**（事件档下同一形状清扫变 0）；
#   ③ **红证明**：把"事件档"**强制**施加到有环形状上 ⇒ 成功判据那一格必须 **FAIL**
#      （实测 RSS 涨 ~8.9×）。若它反而通过 ⇒ 这条判据抓不住误判 ⇒ 本门禁自己红。
#   ④ **盲点棘轮**：某条形状"事件档 vs 默认档"的峰值比不再放大（进步）时必须从
#      `known-red.txt` 删掉那条，否则这一格红。`blind_localhold` 已因②（"这个区域不需要
#      回收"的静态证明 ⇒ 默认档整段不发射 rc 运行期）转绿；它的界由 `gate-reclaim-idle` 看着。
#
# ⑤⑥ 端到端（真实路径）接受**两种等价证据**（`EXTC_RECLAIM=1` 是"允许回收 / 不强制关闭"）：
#   (i)  事件档接线生效：`SWEEP_FREE=1` 且 collect == 0；
#   (ii) ②生效：默认档用户区**0 个 rc 调用点** ⇒ 整段不发射运行期（走这一支时"清扫次数
#        不可信"一位显式跳过：没有运行期就没有清扫可谈）。
#
# 「档」的选择 == 现有开关（`src/back/codegen.c`，Lead 接线）：
#   · 默认档            ：不设任何变量（不计数 + 区域清扫）
#   · **事件档**        ：`EXTC_RC_SWEEPFREE=1`（打开计数 + `shouldSweep` 恒 0）
#   · 无回收基线        ：`EXTC_RECLAIM=0`（stdout 的语义参照）
# 本门禁**只认**这三个开关与 RSS/清扫次数，不依赖 `fnAcyclic`（分析侧并行做）——
# 分析侧落地后，把「①的输入」从 `EXTC_RC_SWEEPFREE=1` 换成"编译器自己判无环"即可，
# 判据本身一个字不用动。
#
# 用法：
#   bash tests/reclaim-acyclic/gate-acyclic.sh
#   GATE_ACYCLIC_HEAVY=1 bash tests/reclaim-acyclic/gate-acyclic.sh   # 另跑真实语料 churn/rebuild
# 可调（都有默认，判据口径不许放宽，只许把规模调小/调大）：
#   N_ACY=200000 N_CYC=800000 N_BLIND=2000000 MIN_BLOWUP=5 BOUND=1.5
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2

SWEEPS=tests/reclaim-acyclic/sweeps.sh
ACY=tests/reclaim-acyclic/acyclic_heapref.extc
CYC=tests/reclaim-shapes/cyclic_pressure.extc
BLIND=tests/reclaim-acyclic/blind_localhold.extc
HOLE=tests/reclaim-shapes/hole_c/hole_c.extc
SCALE=tests/reclaim-shapes/scale_growth.extc
CHURN=bench/gc/src/churn.extc
REBUILD=bench/gc/src/rebuild.extc
KNOWN_RED=tests/reclaim-acyclic/known-red.txt

N_ACY=${N_ACY:-200000}
N_CYC=${N_CYC:-800000}
N_BLIND=${N_BLIND:-2000000}
N_HEAVY=${N_HEAVY:-0}          # churn/rebuild 是自带轮数的固定程序（无 argv），这项保留给可读性
MIN_BLOWUP=${MIN_BLOWUP:-5}    # 红证明/盲点格要求事件档 RSS / 默认档 RSS ≥ 这个倍数
BOUND=${BOUND:-1.5}            # 成功判据的 RSS 上界：事件档 ≤ 默认档 × BOUND
HEAVY=${GATE_ACYCLIC_HEAVY:-0}

pass=0; fail=0
ok()   { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }
note() { printf '  note %s\n' "$1"; }
# 「内层判据」的 FAIL：红证明/盲点格里**期望**看到的那种红，不计入门禁失败数。
inner_fail() { printf '  FAIL %s\n' "$1"; }

ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b+0==0) printf "—"; else printf "%.2f", a/b }'; }
gt()    { awk -v a="$1" -v b="$2" 'BEGIN{ exit !(a+0 >  b+0) }'; }
le()    { awk -v a="$1" -v b="$2" 'BEGIN{ exit !(a+0 <= b+0) }'; }
mb()    { awk -v k="${1:-}" 'BEGIN{ if (k=="") printf "—"; else printf "%.2f MB", k/1024 }'; }

# 生成物里事件档宏的值（`1`/`0`；编译失败或没有该宏 ⇒ 空）。结构证据：接线在编译器里生效。
macro_of() {
    local tmp; tmp=$(mktemp /tmp/gate-acyclic-macro-XXXXXX.c)
    "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$tmp" 2>/dev/null
    sed -n 's/^#define EXTC_RC_SWEEP_FREE \([01]\)$/\1/p' "$tmp" | head -1
    rm -f "$tmp"
}

# 默认档生成物里**用户区**的 rc 调用点数。第二种等价证据：`EXTC_RECLAIM=1` 是"**允许**回收 /
# 不强制关闭"，而②（"这个区域不需要回收"的静态证明）判 idle 的单元**整段不发射 rc 运行期**
# ⇒ 那时一个调用点都没有（比"切进事件档"更强：连计数都不发）。
#
# 口径（本会话踩过的坑）：**数调用点，不数行** —— 运行期每个原语都有一行 `static` 定义，
# 直接 `grep -c` 会把定义行算进去。所以先排除定义行，再按**只有用户区才有的实参形状**数：
#   · 初始化/赋值点：`extc_rc_initRef*((void **)&`、`extc_rc_storeRef*((void **)&`
#   · 分配/触发点：`extc_rc_alloc(__extc_rcr`、`extc_rc_maybeCollect(__extc_rcr`
# 运行期内部的互调（实参是槽指针变量名，没有 `(void **)&` / `__extc_rcr…`）一个都不匹配。
rc_calls_of() {
    local tmp; tmp=$(mktemp /tmp/gate-acyclic-calls-XXXXXX.c)
    "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$tmp" 2>/dev/null
    grep -vE '^[[:space:]]*(__attribute__|static)' "$tmp" 2>/dev/null \
        | grep -oE 'extc_rc_(initRef|initRefNoHold|storeRefOwnedReg|storeRefSameRegion|storeRefOwned|storeRefUnknown)\(\(void \*\*\)&|extc_rc_(alloc|maybeCollect)\(__extc_rcr' \
        | wc -l | tr -d ' '
    rm -f "$tmp"
}

[ -x "$SWEEPS" ] || { echo "gate-acyclic：找不到 $SWEEPS" >&2; exit 2; }
[ -f "$SWEEPS" ] || { echo "gate-acyclic：找不到 $SWEEPS" >&2; exit 2; }

# 编译器路径：既给 `macro_of` 用，也 export 给 `sweeps.sh`（它读 EXTC）。
EXTC=${EXTC:-$ROOT/build/extc}
export EXTC

# ── 量一个 (档, 形状, [N]) ───────────────────────────────────────────────────
# measure <default|event|norecl|heavy|heavy0> <src> [N]
# 出参：M_OK M_MEAS M_SWEEPS M_RSS M_HASH M_OUT M_RC M_MD5
measure() {
    local tier=$1 src=$2 n=${3:-} kv
    local args=(); [ -n "$n" ] && args=("$n")     # 无 argv 的形状不许给空参数
    case "$tier" in
        default) kv=$(bash "$SWEEPS" "$src" "${args[@]}" 2>/dev/null) ;;
        event)   kv=$(EXTC_RC_SWEEPFREE=1 bash "$SWEEPS" "$src" "${args[@]}" 2>/dev/null) ;;
        norecl)  kv=$(EXTC_RECLAIM=0 bash "$SWEEPS" "$src" "${args[@]}" 2>/dev/null) ;;
        heavy)   kv=$(EXTC_RC_SWEEPFREE=1 bash "$SWEEPS" "$src" 2>/dev/null) ;;
        heavy0)  kv=$(bash "$SWEEPS" "$src" 2>/dev/null) ;;
        *) echo "gate-acyclic：未知档 $tier" >&2; return 2 ;;
    esac
    M_OK=$(awk -F= '$1=="compile_ok"{print $2}' <<<"$kv")
    M_MEAS=$(awk -F= '$1=="sweeps_measurable"{print $2}' <<<"$kv")
    M_SWEEPS=$(awk -F= '$1=="sweeps"{print $2}' <<<"$kv")
    M_RSS=$(awk -F= '$1=="rss_kb"{print $2}' <<<"$kv")
    M_HASH=$(awk -F= '$1=="out_hash"{print $2}' <<<"$kv")
    M_OUT=$(awk -F= '$1=="out_file"{print $2}' <<<"$kv")
    M_RC=$(awk -F= '$1=="rc"{print $2}' <<<"$kv")
    M_MD5=$(awk -F= '$1=="extc_md5"{print $2}' <<<"$kv")
    [ "$M_OK" = "1" ] || return 1
    return 0
}

start_md5=$(md5sum build/extc 2>/dev/null | cut -d' ' -f1 || true)
echo "== gate-acyclic：无环 ⇒ 事件档（0 次清扫、RSS 有界、stdout 不变）+ 红证明 + 已知盲点 =="
echo "   形状：$ACY（无环/堆槽）· $CYC（有环）· $BLIND（局部量持有，盲点）· $HOLE（真无环，端到端）· $SCALE（回边，安全格）"
echo "   规模：N_ACY=$N_ACY N_CYC=$N_CYC N_BLIND=$N_BLIND  红证明/盲点阈值 ${MIN_BLOWUP}×  上界 ${BOUND}×"
echo "   编译器 md5=${start_md5:-?}"

# ── ① 成功判据：无环形状 + 事件档 ─────────────────────────────────────────────
echo
echo "== ① 成功判据：无环形状（$ACY）+ 事件档（EXTC_RC_SWEEPFREE=1）=="
if ! measure default "$ACY" "$N_ACY"; then bad "① 默认档编译/运行失败（$ACY）"; else
    a_d_rss=$M_RSS; a_d_sweeps=$M_SWEEPS
    if ! measure event "$ACY" "$N_ACY"; then bad "① 事件档编译/运行失败（$ACY）"; else
        a_e_meas=$M_MEAS; a_e_sweeps=$M_SWEEPS; a_e_rss=$M_RSS; a_e_hash=$M_HASH; a_e_rc=$M_RC
        if ! measure norecl "$ACY" "$N_ACY"; then bad "① baseline（EXTC_RECLAIM=0）编译/运行失败"; else
            a_b_hash=$M_HASH; a_b_rc=$M_RC
            # ①a 清扫次数 == 0（并且必须真的量到了 —— "没量到"不等于 0）
            if [ "$a_e_meas" != "1" ]; then
                bad "①a 事件档没有 [rccount] 行 ⇒ 清扫次数不可信（EXTC_RC_TIME 没生效？）"
            elif [ "$a_e_sweeps" = "0" ]; then
                ok "①a 事件档清扫次数 = 0（collect 没被调用过）"
            else
                bad "①a 事件档清扫次数 = $a_e_sweeps（要求 == 0）"
            fi
            # ①b stdout 与 baseline 逐字相同
            if [ -n "$a_e_hash" ] && [ "$a_e_hash" = "$a_b_hash" ]; then
                ok "①b stdout 与 baseline（EXTC_RECLAIM=0）逐字相同（sha256 ${a_e_hash:0:12}）"
            else
                bad "①b stdout 与 baseline 不一致（event=${a_e_hash:-?} baseline=${a_b_hash:-?}）"
            fi
            # ①c 峰值 RSS ≤ 默认档 × BOUND
            r=$(ratio "$a_e_rss" "$a_d_rss")
            if [ -n "$a_e_rss" ] && [ -n "$a_d_rss" ] && le "$a_e_rss" "$(awk -v d="$a_d_rss" -v b="$BOUND" 'BEGIN{printf "%.0f", d*b}')"; then
                ok "①c 峰值 RSS $(mb "$a_e_rss") ≤ 默认档 $(mb "$a_d_rss") × $BOUND（实测 ${r}×）"
            else
                bad "①c 峰值 RSS $(mb "$a_e_rss") > 默认档 $(mb "$a_d_rss") × $BOUND（实测 ${r}×）"
            fi
            note "① 规模行为旁证：默认档 collect=$a_d_sweeps；语义参照 rc（event/baseline）= $a_e_rc/$a_b_rc"
            A_D_RSS=$a_d_rss; A_D_SWEEPS=$a_d_sweeps; A_E_SWEEPS=$a_e_sweeps
        fi
    fi
fi

# ── ② 上界判据：有环形状不得进事件档 ───────────────────────────────────────────
echo
echo "== ② 上界判据：有环形状（$CYC）不得被判成事件档 =="
C_D_RSS=""; C_D_SWEEPS=""; C_E_RSS=""; C_E_SWEEPS=""
if ! measure default "$CYC" "$N_CYC"; then bad "② 默认档编译/运行失败（$CYC）"; else
    C_D_RSS=$M_RSS; C_D_SWEEPS=$M_SWEEPS; C_D_MEAS=$M_MEAS
    if [ "$C_D_MEAS" != "1" ]; then
        bad "②a 默认档没有 [rccount] 行 ⇒ 清扫次数不可信"
    elif gt "$C_D_SWEEPS" 0; then
        ok "②a 有环形状默认档清扫次数 = $C_D_SWEEPS > 0（没被判成"不扫"档）"
    else
        bad "②a 有环形状默认档清扫次数 = ${C_D_SWEEPS:-?}，要求 > 0 —— **它被判成事件档了**（判据瞎了）"
    fi
    if ! measure event "$CYC" "$N_CYC"; then bad "② 事件档编译/运行失败（$CYC）"; else
        C_E_RSS=$M_RSS; C_E_SWEEPS=$M_SWEEPS; C_E_MEAS=$M_MEAS; C_E_HASH=$M_HASH
        # ②b 旋钮非空转：事件档下同一形状清扫确实变 0（否则"0 次清扫"可能只是开关没生效）。
        # 默认档已经不是清扫档时（②a 已红，例如注入/全局强制事件化）跳过，不重复计账。
        if [ "$C_D_MEAS" = "1" ] && ! gt "$C_D_SWEEPS" 0; then
            note "②b 默认档已被强制事件化（collect=0）⇒ 无法构成「开关非空转」对照，跳过"
        elif [ "$C_E_MEAS" = "1" ] && [ "$C_E_SWEEPS" = "0" ] && [ "$C_D_MEAS" = "1" ] && gt "$C_D_SWEEPS" 0; then
            ok "②b 开关非空转：同一形状默认档 collect=$C_D_SWEEPS → 事件档 collect=0"
        else
            bad "②b 开关空转：默认档 collect=${C_D_SWEEPS:-?} / 事件档 collect=${C_E_SWEEPS:-?}（事件档应量到 0）"
        fi
    fi
fi

# ── ③ 红证明：把事件档强制施加到有环形状上 ⇒ 成功判据那一格必须 FAIL ─────────────
echo
echo "== ③ 红证明：把事件档强制施加到有环形状（$CYC）上 ⇒ ①的三条必须 FAIL =="
if [ -z "$C_D_SWEEPS" ] || ! gt "$C_D_SWEEPS" 0; then
    note "③ 无法构成对照：默认档的清扫次数不是 > 0（②已经红了）⇒ 这一格不重复计账"
elif [ -z "$C_E_RSS" ]; then
    bad "③ 事件档（强制）没量到 RSS ⇒ 红证明无法判定"
else
    if ! measure norecl "$CYC" "$N_CYC"; then
        note "③ 无回收基线编译/运行失败 ⇒ 只判 RSS 那一半"
        c_b_hash=""
    else
        c_b_hash=$M_HASH
    fi
    r=$(ratio "$C_E_RSS" "$C_D_RSS")
    # 内层就是成功判据 ① 的三条：清 0（这一半会"通过"，正是误判的形态）、stdout（应一致）、
    # RSS ≤ 默认 × BOUND（这一半**必须** FAIL）。
    if [ "$C_E_SWEEPS" = "0" ]; then
        note "③ 强制事件档下清扫次数确实是 0 —— 这一半"通过"正是"判据瞎了"的形态"
    fi
    if [ -n "$c_b_hash" ] && [ -n "$C_E_HASH" ] && [ "$C_E_HASH" = "$c_b_hash" ]; then
        note "③ 强制事件档下 stdout 仍与 baseline 逐字相同（回收换运行期、不改语义）"
    elif [ -n "$c_b_hash" ] && [ -n "$C_E_HASH" ]; then
        note "③ 强制事件档下 stdout 与 baseline 不一致（${C_E_HASH:0:12} vs ${c_b_hash:0:12}）—— 这是另一类问题，单列"
    fi
    if gt "$r" "$BOUND"; then
        inner_fail "①c 峰值 RSS $(mb "$C_E_RSS") > 默认档 $(mb "$C_D_RSS") × $BOUND（实测 ${r}×）—— 成功判据在这一格 FAIL"
    fi
    if gt "$r" "$MIN_BLOWUP"; then
        ok "③ 红证明成立：强制事件档后成功判据的 RSS 格确实 FAIL（${r}× ≥ ${MIN_BLOWUP}×）⇒ 判据有牙"
    else
        bad "③ 红证明失败：强制事件档后 RSS 只涨 ${r}×（< ${MIN_BLOWUP}×）—— 这条判据抓不住误判，请查判据或形状"
    fi
fi

# ── ④ 盲点棘轮（预期红 ↔ 已知红基线的双向对齐）────────────────────────────────
# 现有事件路径只数"rc 槽 → 对象"这条边：引用只住在局部量里的对象永不释放。这一格量的是
# "事件档 vs 默认档的峰值比"；某条形状**不再红**（进步）时必须从 known-red.txt 删掉那条，
# 否则这条棘轮当场红。`blind_localhold` 就是最近转绿的一条：② 判它"回收无事可做" ⇒
# 默认档整段不发射 rc 运行期（不再有"事件档"可比），它的界由 `gate-reclaim-idle` 看着。
is_known_red() { [ -f "$KNOWN_RED" ] && grep -q "^$1	" "$KNOWN_RED"; }

# expect_red <名字> <形状> <N> <有无 argv：1/0>
expect_red() {
    local name=$1 src=$2 n=$3 hasargv=$4
    echo
    echo "== ④ 已知盲点（预期红）：$name（$src${hasargv:+ N=$n}）=="
    local dkv ekv
    if [ "$hasargv" = "1" ]; then
        dkv=$(bash "$SWEEPS" "$src" "$n" 2>/dev/null)
        ekv=$(EXTC_RC_SWEEPFREE=1 bash "$SWEEPS" "$src" "$n" 2>/dev/null)
    else
        dkv=$(bash "$SWEEPS" "$src" 2>/dev/null)
        ekv=$(EXTC_RC_SWEEPFREE=1 bash "$SWEEPS" "$src" 2>/dev/null)
    fi
    local dok eok d_rss e_rss d_sw e_sw r
    dok=$(awk -F= '$1=="compile_ok"{print $2}' <<<"$dkv"); eok=$(awk -F= '$1=="compile_ok"{print $2}' <<<"$ekv")
    d_rss=$(awk -F= '$1=="rss_kb"{print $2}' <<<"$dkv"); e_rss=$(awk -F= '$1=="rss_kb"{print $2}' <<<"$ekv")
    d_sw=$(awk -F= '$1=="sweeps"{print $2}' <<<"$dkv"); e_sw=$(awk -F= '$1=="sweeps"{print $2}' <<<"$ekv")
    if [ "$dok" != "1" ] || [ "$eok" != "1" ]; then
        bad "④ $name：编译/运行失败（default=$dok event=$eok）"
        return
    fi
    # 默认档已经被强制事件化（例如注入/全局 `EXTC_RC_SWEEPFREE=1`）时，两侧是同一档
    # ⇒ 比值恒 ~1，既不是"保持红"也不是"转绿"，无法构成对照：跳过，不重复计账。
    if [ "$d_sw" = "0" ]; then
        note "④ $name：默认档 collect=0 ⇒ 默认档已被强制事件化，无法构成对照，跳过棘轮判定"
        return
    fi
    r=$(ratio "$e_rss" "$d_rss")
    if gt "$r" "$MIN_BLOWUP"; then
        inner_fail "①c（盲点格）峰值 RSS $(mb "$e_rss") > 默认档 $(mb "$d_rss") × ${MIN_BLOWUP}（实测 ${r}×）—— 事件路径还没覆盖「引用只在局部量里」"
        if is_known_red "$name"; then
            ok "④ $name 预期红保持（collect ${d_sw}→${e_sw}，RSS ${r}×）；等根引用计数落地后应转绿并删 known-red.txt 条目"
        else
            bad "④ $name 是**新增红**：事件路径退化了？确认后加进 $KNOWN_RED 并注明原因/跟踪"
        fi
    else
        if is_known_red "$name"; then
            bad "④ $name 已不再红（RSS ${r}× < ${MIN_BLOWUP}×）⇒ 这是进步：请从 $KNOWN_RED 删掉这条（棘轮只许缩）"
        else
            ok "④ $name 已不红（RSS ${r}×）"
        fi
    fi
}

if [ -f "$BLIND" ]; then
    expect_red blind-localhold "$BLIND" "$N_BLIND" 1
else
    bad "④ 找不到盲点形状 $BLIND"
fi

# ── ⑤⑥ 端到端（真实路径）：默认编译**真无环**的形状就该切事件档 ─────────────────
# ①③④ 都用 `EXTC_RC_SWEEPFREE=1` **人工选档**，测的是"事件档运行期对不对"；
# 这两格不设任何变量，测的是"分析（`planFnAcyclic`）+ codegen 接线（`unitAllAcyclic`）
# 有没有真的把被判无环的程序切过去"。没有它们，接线整条断掉也看不出来（①照样绿）。
#
# 两个形状：
#   · `hole_c/hole_c.extc`    —— 递归建树，`all=1`，不用 extern（`println` 是内建）
#   · `acyclic_heapref.extc`  —— 引用在堆槽，用 `io` ⇒ 会拉出 `extc_cout_put/flush` 的
#     **extern 声明**；`collectEmitted` 会把无体的 extern 声明也推进 `g->funcs`，而
#     `planFnAcyclic` 对无函数体者恒 false。Lead 已修 `unitAllAcyclic`：**跳过 `!f->body`
#     的函数**（extern 没有体 ⇒ 不写我们的引用槽；它拿到的引用受调用点逃逸/借用约束 ⇒
#     无法让区域内对象变成不可达）。承重假设与判错方向（内存涨，不是悬垂）写在代码注释里，
#     由 ⑦ 的安全格看着"别放宽过头"。
real_path_green() {              # <格号> <src> [N]
    local num=$1 src=$2 n=${3:-}
    local meas sw rss hash macro calls
    echo
    echo "== $num 端到端（真实路径，不设任何变量）：默认编译 $src 就该切事件档（或②生效时不发射）=="
    if ! measure default "$src" ${n:+"$n"}; then
        bad "$num 默认档编译/运行失败（$src）"
        return
    fi
    meas=$M_MEAS; sw=$M_SWEEPS; rss=$M_RSS; hash=$M_HASH
    macro=$(macro_of "$src")
    calls=$(rc_calls_of "$src")
    if ! measure norecl "$src" ${n:+"$n"}; then
        bad "$num 无回收基线（EXTC_RECLAIM=0）编译/运行失败"
        return
    fi
    local bhash=$M_HASH
    # a 两种**等价证据**（决定 (A)：`EXTC_RECLAIM=1` 是"允许回收 / 不强制关闭"）：
    #    (i)  事件档接线生效：默认档清扫 0 次（而且必须量到了 —— "没量到"≠0）；
    #    (ii) ②生效：默认档用户区**一个 rc 调用点都没有** ⇒ 整段不发射回收运行期。
    #         这条分支下"清扫次数不可信"**显式跳过**：不是静默放过，而是"没有运行期就没有
    #         清扫可谈"—— 那一位此刻没有意义（运行期不存在，谈不上次数）。
    if [ "$calls" = 0 ]; then
        ok "$num a ②生效：默认档用户区 0 个 rc 调用点 ⇒ 整段不发射回收运行期（清扫次数一位显式跳过：没有运行期就没有清扫可谈）"
    elif [ "$meas" != "1" ]; then
        bad "$num a 默认档没有 [rccount] 行 ⇒ 清扫次数不可信"
    elif [ "$sw" = "0" ]; then
        ok "$num a 真实路径生效：默认编译 collect = 0（$src）"
    else
        bad "$num a 默认编译 collect=$sw ≠ 0 —— 被判无环的程序没被切进事件档"
    fi
    # b 结构证据：生成物里真的发了事件档，**或**②生效（一个 rc 调用点都没有，比事件档更强）
    if [ "$macro" = "1" ]; then
        ok "$num b 生成物 #define EXTC_RC_SWEEP_FREE = 1（接线在编译器里真的生效）"
    elif [ "$calls" = 0 ]; then
        ok "$num b ②生效：生成物里 0 个 rc 调用点（整段不发射 ⇒ 比事件档更强；调用点计数已排除 static 定义行）"
    else
        bad "$num b 生成物 #define EXTC_RC_SWEEP_FREE = ${macro:-?}（要求 1）—— 接线没生效"
    fi
    # c stdout 与关回收基线逐字相同
    if [ -n "$hash" ] && [ "$hash" = "$bhash" ]; then
        ok "$num c stdout 与 baseline（EXTC_RECLAIM=0）逐字相同（sha256 ${hash:0:12}）"
    else
        bad "$num c stdout 与 baseline 不一致（real=${hash:-?} baseline=${bhash:-?}）"
    fi
    # d 真实路径与人工强制档等价（同一档的两个入口）
    if measure event "$src" ${n:+"$n"}; then
        local r; r=$(ratio "$rss" "$M_RSS")
        if le "$rss" "$(awk -v e="$M_RSS" -v b="$BOUND" 'BEGIN{printf "%.0f", e*b}')"; then
            ok "$num d 真实路径峰值 RSS $(mb "$rss") 与 EXTC_RC_SWEEPFREE=1 强制档 $(mb "$M_RSS") 一致（${r}× ≤ $BOUND×）"
        else
            bad "$num d 真实路径 RSS $(mb "$rss") > 强制档 $(mb "$M_RSS") × $BOUND（${r}×）"
        fi
    fi
}

real_path_green ⑤ "$HOLE"
real_path_green ⑥ "$ACY" "$N_ACY"

# ── ⑦ 安全格（真实路径）：**该拒的必须拒** ──────────────────────────────────────
# Lead 的修法放宽了 extern（跳过无体函数）。必须由这一格看住"别放宽过头"：这些形状里
# 有真环（`cyclic_pressure`）、有"值不比宿主年轻"的回边（`scale_growth`）⇒ 真实路径下
# **必须**仍是清扫档：`SWEEP_FREE=0` 且 collect > 0。
# 摘掉 `blind_localhold`：它被判成"回收无事可做"（没有丢引用）⇒ 默认档整段不发射 rc 运行期，
# 本来就到不了"清扫档"这一格。它的界**不是**靠这里看着，而是 `gate-reclaim-idle` 的
# 被判 idle 形状 N vs 4N 峰值界（实测 rc 关掉 1452 KB 平，比开档还低）。
# （`bench/app/session.extc` 没用上：它默认**根本不发回收运行期**（生成物里 0 个 `extc_rc_`，
#   `[rccount]` 一行都没有），没有"collect > 0"可断言 —— 见 README「安全格」一节。）
N_SAFE=${N_SAFE:-800000}
real_path_reject() {             # <src> [N]
    local src=$1 n=${2:-} macro sw meas
    macro=$(macro_of "$src")
    if ! measure default "$src" ${n:+"$n"}; then
        bad "⑦ $src 编译/运行失败"
        return
    fi
    sw=$M_SWEEPS; meas=$M_MEAS
    if [ "$macro" = "0" ] && [ "$meas" = "1" ] && [ -n "$sw" ] && gt "$sw" 0; then
        ok "⑦ $(basename "${src%.extc}")${n:+ N=$n} 真实路径正确拒绝（SWEEP_FREE=0、collect=$sw > 0）"
    else
        bad "⑦ $(basename "${src%.extc}")${n:+ N=$n} 真实路径**没拒绝**（SWEEP_FREE=${macro:-空}、collect=${sw:-?}）—— extern 放宽过头了？"
    fi
}
echo
echo "== ⑦ 安全格（真实路径）：该拒的必须拒（SWEEP_FREE=0 且 collect > 0）=="
real_path_reject "$CYC" "$N_CYC"
real_path_reject "$SCALE" "$N_SAFE"

if [ "$HEAVY" = "1" ]; then
    if [ -f "$CHURN" ]; then expect_red churn "$CHURN" 0 0; else bad "④ 找不到 $CHURN"; fi
    if [ -f "$REBUILD" ]; then expect_red rebuild "$REBUILD" 0 0; else bad "④ 找不到 $REBUILD"; fi
else
    note '④ 真实语料 churn/rebuild 这一档默认**不跑**（事件档下 churn 峰值 ~812 MB，nobench 要轻）；'
    note '   要看就设 GATE_ACYCLIC_HEAVY=1 再跑本脚本（实测 churn 5 100 KB → 831 852 KB、rebuild → ~521 MB）'
fi

# ── 收尾：编译器在跑的过程中被换掉要说出来 ─────────────────────────────────────
end_md5=$(md5sum build/extc 2>/dev/null | cut -d' ' -f1 || true)
if [ "$start_md5" != "$end_md5" ]; then
    note "编译器 md5 在本次运行中变了：$start_md5 → $end_md5（别的工作在重编？数字按各自那一段读）"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
