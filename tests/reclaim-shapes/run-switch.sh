#!/usr/bin/env bash
# tests/reclaim-shapes/run-switch.sh —— **开关（EXTC_RECLAIM=1）下的门禁断言**
#
# 与同目录 `run.sh` 的分工：
#   run.sh         基线：只记录现状，exit 0，红条目在表里标出（同 arena-soundness 口径，不是门禁）。
#   本脚本         门禁：只在开关打开时成立的判据。关掉开关必须**红** —— 判据有牙的证据。
#
# 判据：
#   ① `scale_growth` N vs 4N：峰值比 < 1.5（开关关 ≈3.0× 红；开关开 ≈1.0× 绿）。
#      机制见 docs/RECLAMATION.md：计数归零当场回收 + 区域清扫兜环 ⇒ 界与轮数解耦。
#   ①b `cyclic_pressure` N vs 4N：峰值比 < 1.5（开关关 ≈3.4× 红；开关开 ≈1.0× 绿），
#      且默认与开关下输出逐字一致。与 ① 的区别是**垃圾是环**：持有者计数永不归零，
#      计数归零一个也放不掉 ⇒ 这条达到有界，只可能是清扫上了场（对照实验见 run-evidence.sh）。
#   ② `cond_save` / `cyclic_shape` 在开关下**语义不变**（开关只换运行期，不改语义）。
#   ③ 四个曾经的 SEGV 用例在开关下不崩、输出不变（板/池/栈/C 指针不是 rc 槽：
#      `owns` 守卫必须一个字节都不碰）：
#      tests/heap/loan、tests/heap/plate、tests/cabi/heapmain、tests/coro/iterator_protocol。
#
# 用法：tests/reclaim-shapes/run-switch.sh                 默认 EXTC_RECLAIM=1（绿）
#       EXTC_RECLAIM=0 tests/reclaim-shapes/run-switch.sh  显式关掉（① 必须红 ⇒ exit 1）
#
# 峰值内存取 `/usr/bin/time -v` 的 Maximum resident set size；程序退出码直接量（不看 `--run`，
# 它拿不到信号）。
set -u
cd "$(dirname "$0")/../.."          # 仓库根：与同目录 run.sh 同一约定

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 开关：默认打开。`EXTC_RECLAIM=0` 显式关掉。
# ⚠️ extc 判的是 `getenv("EXTC_RECLAIM") != NULL` ⇒ 空串也算**打开**，关掉必须 unset。
case "${EXTC_RECLAIM-1}" in
    0) unset EXTC_RECLAIM; sw=off ;;
    *) export EXTC_RECLAIM=1; sw=on ;;
esac

# 本脚本的跑法固定：不继承 `EXTC_DBG`（assert 版是另一条**正交**判据，由 check.sh 的断言版那一节管）。
# 理由：`cyclic_shape.extc` 在 assert 版下会撞到一个与回收无关的**已知**逃逸分析兜底 ——
# `src/check/check_escape.c:730 promoteFieldsAt: recursion budget (32) exhausted`（rc=134），
# 开关开/关都一样（task-5 改动前也如此）。那是编译器自身的兜底路径，不是本门禁要钉的回收判据。
if [ -n "${EXTC_DBG-}" ]; then
    echo "  note 不继承 EXTC_DBG（assert 版是另一条正交判据；cyclic_shape 在断言版下会撞到已知逃逸分析兜底）"
    unset EXTC_DBG
fi

pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }
rss_mb() { awk -v k="$1" 'BEGIN { if (k == "") printf "—"; else printf "%.2f MB", k/1024 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a == "" || b == "") printf "—"; else printf "%.2f", b/a }'; }

echo "== 回收开关断言（EXTC_RECLAIM=$sw）=="

# 编译 + 运行一个 .extc；结果进 BR_*：BR_C 编译（ok/fail）、BR_RC 退出码、BR_RSS 峰值 KB、
# BR_OUT 标准输出（去尾换行，保留内部换行）。
build_run() { # <源文件> [程序参数...]
    local src=$1; shift
    BR_C=fail; BR_RC=-; BR_RSS=""; BR_OUT=""
    if "$EXTC" -w --no-line-map "$src" -o "$TMP/prog.c" >"$TMP/prog.log" 2>&1 \
       && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/prog" "$TMP/prog.c" >>"$TMP/prog.log" 2>&1; then
        BR_C=ok
        "$TIME" -v -o "$TMP/prog.time" "$TMP/prog" "$@" >"$TMP/prog.out" 2>"$TMP/prog.err"
        BR_RC=$?
        BR_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/prog.time")
        BR_OUT=$(cat "$TMP/prog.out")
    fi
}

# ── ① scale_growth：峰值与轮数无关（开关关掉，这条就是红的）─────────────────────
echo "-- ① scale-growth N→4N：峰值必须与轮数解耦 --"
build_run tests/reclaim-shapes/scale_growth.extc 200000
n_c=$BR_C; n_rc=$BR_RC; n_out=$BR_OUT; n_rss=$BR_RSS
build_run tests/reclaim-shapes/scale_growth.extc 800000
f_c=$BR_C; f_rc=$BR_RC; f_out=$BR_OUT; f_rss=$BR_RSS
if [ "$n_c" = ok ] && [ "$f_c" = ok ] && [ "$n_rc" = 0 ] && [ "$f_rc" = 0 ] \
   && [ "$n_out" = "rounds=200000 saved=199999" ] && [ "$f_out" = "rounds=800000 saved=799999" ]; then
    ratio=$(awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { printf "%.2f", b/a }')
    if awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "scale-growth N→4N：$(rss_mb "$n_rss") → $(rss_mb "$f_rss")（${ratio}× < 1.5×）—— 界与轮数无关"
    else
        bad "scale-growth N→4N：$(rss_mb "$n_rss") → $(rss_mb "$f_rss")（${ratio}× ≥ 1.5×）—— 界仍与轮数有关"
    fi
else
    bad "scale-growth N→4N：编译/运行/语义没同时成立（N: c=$n_c rc=$n_rc out=$n_out / 4N: c=$f_c rc=$f_rc out=$f_out）"
fi

# ── ①b cyclic_pressure：**有环**形状，峰值必须与轮数解耦（清扫上场的证据）────────────
# 形状见 tests/reclaim-shapes/cyclic_pressure.extc 的文件头：桶满时整条双向链退役 ⇒ 链内
# next/prev 互指、每个节点都还有入边 ⇒ 持有者计数**永不归零** ⇒ 计数归零一个也放不掉，
# 只有区域清扫能收。判据：
#   · 默认（开关关）与开关开：输出**逐字一致**（回收只换运行期，不改语义）；
#   · 开关开 N vs 4N：峰值比 < 1.5（界与轮数解耦）。
# 同一形状开关关掉时 ≈3.4×（run.sh 基线里那条红）；「清扫真的上场」的对照实验（把清扫触发
# 阈值抬到 1 TB ⇒ 一次清扫都不发 ⇒ 峰值重新随轮数涨 ≈3.5×）在 run-evidence.sh。
echo "-- ①b cyclic-pressure N→4N：有环形状的峰值必须与轮数解耦（清扫上台）--"
CYC_WANT_N="rounds=200000 retired=22208 live=128 fwd=25591744 back=25591744 idx=20000 cache=1024 hits=155520 depth=212 tree=true"
CYC_WANT_F="rounds=800000 retired=88832 live=512 fwd=409468672 back=409468672 idx=20000 cache=1024 hits=622272 depth=80 tree=true"

# 编译 cyclic_pressure：on 带开关，off 显式不带（`env -u`，因为本脚本已 export 了开关）。
cyc_build() { # <on|off> <tag> → CY_C=ok/fail
    local mode=$1 tag=$2
    CY_C=fail
    if [ "$mode" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map tests/reclaim-shapes/cyclic_pressure.extc \
            -o "$TMP/cyc-$tag.c" >"$TMP/cyc-$tag.build.log" 2>&1
    else
        env -u EXTC_RECLAIM "$EXTC" -w --no-line-map tests/reclaim-shapes/cyclic_pressure.extc \
            -o "$TMP/cyc-$tag.c" >"$TMP/cyc-$tag.build.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/cyc-$tag" "$TMP/cyc-$tag.c" \
            >>"$TMP/cyc-$tag.build.log" 2>&1; then CY_C=ok; fi
}
cyc_run() { # <tag> <N> → CY_RC / CY_RSS / CY_OUT
    local tag=$1 n=$2
    CY_RC=-; CY_RSS=""; CY_OUT=""
    "$TIME" -v -o "$TMP/cyc-$tag.time" "$TMP/cyc-$tag" "$n" >"$TMP/cyc-$tag.out" 2>"$TMP/cyc-$tag.err"
    CY_RC=$?
    CY_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/cyc-$tag.time")
    CY_OUT=$(cat "$TMP/cyc-$tag.out")
}

cyc_build on  r; cyc_on_build=$CY_C
cyc_build off o; cyc_off_build=$CY_C
cyc_run r 200000; rn_rc=$CY_RC; rn_rss=$CY_RSS; rn_out=$CY_OUT
cyc_run o 200000; on_rc=$CY_RC; on_rss=$CY_RSS; on_out=$CY_OUT
cyc_run r 800000; rf_rc=$CY_RC; rf_rss=$CY_RSS; rf_out=$CY_OUT
cyc_run o 800000; of_rc=$CY_RC; of_rss=$CY_RSS; of_out=$CY_OUT

# 语义：开关开/关的输出必须逐字一致（回收只换运行期）。
if [ "$cyc_on_build" = ok ] && [ "$cyc_off_build" = ok ] && [ "$rn_rc" = 0 ] && [ "$on_rc" = 0 ] \
   && [ "$rn_out" = "$CYC_WANT_N" ] && [ "$on_out" = "$CYC_WANT_N" ]; then
    ok "cyclic-pressure：默认与开关下输出逐字一致（N=200000：桶链两向一致 / 索引 / 缓存 / 父指针全对）"
else
    bad "cyclic-pressure：默认/开关语义不一致或没跑对（on[c=$cyc_on_build rc=$rn_rc out=$rn_out] off[c=$cyc_off_build rc=$on_rc out=$on_out]）"
fi

# 峰值判据跟**本次调用的开关状态**走：`EXTC_RECLAIM=0 ./run-switch.sh` 时这条必须红
# （和 ① 一样是“有牙”的一部分），不能自己硬把开关打开。
if [ "$sw" = on ]; then amb_tag=r; amb_name="开关开"; other_ratio=$(ratio_of "$on_rss" "$of_rss"); else amb_tag=o; amb_name="开关关"; other_ratio=$(ratio_of "$rn_rss" "$rf_rss"); fi
cyc_run "$amb_tag" 200000; mn_rc=$CY_RC; mn_rss=$CY_RSS; mn_out=$CY_OUT
cyc_run "$amb_tag" 800000; mf_rc=$CY_RC; mf_rss=$CY_RSS; mf_out=$CY_OUT
if [ "$mn_rc" = 0 ] && [ "$mf_rc" = 0 ] && [ "$mn_out" = "$CYC_WANT_N" ] && [ "$mf_out" = "$CYC_WANT_F" ] \
   && [ -n "$mn_rss" ] && [ -n "$mf_rss" ]; then
    ratio=$(ratio_of "$mn_rss" "$mf_rss")
    if awk -v a="$mn_rss" -v b="$mf_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "cyclic-pressure N→4N（$amb_name）：$(rss_mb "$mn_rss") → $(rss_mb "$mf_rss")（${ratio}× < 1.5×）—— 环垃圾被清扫收住"
    else
        bad "cyclic-pressure N→4N（$amb_name）：$(rss_mb "$mn_rss") → $(rss_mb "$mf_rss")（${ratio}× ≥ 1.5×）—— 有环形状的界仍随轮数涨（另一档 ${other_ratio}×）"
    fi
else
    bad "cyclic-pressure N→4N：两次运行没同时跑对（N: rc=$mn_rc out=$mn_out / 4N: rc=$mf_rc out=$mf_out）"
fi

# ── ①c cross-region：跨区堆→堆边的登记 + 来源侧撤销 ────────────────────────────
# 形状见 tests/reclaim-shapes/cross_region.extc：region 1 的对象被 region 2 的堆对象指着，
# 而 region 2 每轮随块丢弃。两条判据：
#   · **登记**：`s.next = keeper` 是堆→堆跨区入边，没有它 region 1 的清扫看不见这条边；
#   · **撤销**：来源侧记账（rscnt + 来源对象/区域死亡时成批撤销）让登记位不粘 —— 只置不清
#     的话每轮的 keeper 都永久当种子，峰值随轮数涨（task-3 撤掉登记的原因）。
# 观测：开关下 N vs 4N 峰值比 < 1.5；`EXTC_RC_STATS=1` 的 **rs_hwm**（跨区登记高水位）不随
# 轮数增长，而 `rs_inc` 随轮数增长（证明登记真的在发）。开关关：同形状峰值随轮数涨（有牙）。
echo "-- ①c cross-region：跨区堆→堆边登记 + 撤销（登记量必须与轮数解耦）--"
XREG_WANT_N="rounds=200000 sink=40000400000 last=200000"
XREG_WANT_F="rounds=800000 sink=640001600000 last=200000"

xr_build() { # <on|off> <tag> → XR_C=ok/fail
    XR_C=fail
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map tests/reclaim-shapes/cross_region.extc \
            -o "$TMP/xr-$2.c" >"$TMP/xr-$2.build.log" 2>&1
    else
        env -u EXTC_RECLAIM "$EXTC" -w --no-line-map tests/reclaim-shapes/cross_region.extc \
            -o "$TMP/xr-$2.c" >"$TMP/xr-$2.build.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/xr-$2" "$TMP/xr-$2.c" \
            >>"$TMP/xr-$2.build.log" 2>&1; then XR_C=ok; fi
}
xr_run() { # <tag> <N> [stats] → XR_RC / XR_RSS / XR_OUT / XR_HWM / XR_INC
    local tag=$1 n=$2
    XR_RC=-; XR_RSS=""; XR_OUT=""; XR_HWM=""; XR_INC=""
    if [ "${3-}" = stats ]; then
        EXTC_RC_STATS=1 "$TIME" -v -o "$TMP/xr-$tag.time" "$TMP/xr-$tag" "$n" \
            >"$TMP/xr-$tag.out" 2>"$TMP/xr-$tag.err"
    else
        "$TIME" -v -o "$TMP/xr-$tag.time" "$TMP/xr-$tag" "$n" >"$TMP/xr-$tag.out" 2>"$TMP/xr-$tag.err"
    fi
    XR_RC=$?
    XR_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/xr-$tag.time")
    XR_OUT=$(cat "$TMP/xr-$tag.out")
    XR_HWM=$(grep -o 'rs_hwm=[0-9]*' "$TMP/xr-$tag.err" | head -1 | cut -d= -f2)
    XR_INC=$(grep -o 'rs_inc=[0-9]*' "$TMP/xr-$tag.err" | head -1 | cut -d= -f2)
}

xr_build on  r; xr_on_c=$XR_C
xr_build off o; xr_off_c=$XR_C
xr_run r 200000 stats; rn_rc=$XR_RC; rn_rss=$XR_RSS; rn_out=$XR_OUT; rn_hwm=$XR_HWM; rn_inc=$XR_INC
xr_run o 200000;       on_rc=$XR_RC; on_out=$XR_OUT
xr_run r 800000 stats; rf_rc=$XR_RC; rf_rss=$XR_RSS; rf_out=$XR_OUT; rf_hwm=$XR_HWM; rf_inc=$XR_INC
xr_run o 800000;       of_rc=$XR_RC; of_out=$XR_OUT

if [ "$xr_on_c" = ok ] && [ "$xr_off_c" = ok ] && [ "$rn_rc" = 0 ] && [ "$on_rc" = 0 ] \
   && [ "$rn_out" = "$XREG_WANT_N" ] && [ "$on_out" = "$XREG_WANT_N" ]; then
    ok "cross-region：默认与开关下输出逐字一致（N=200000 与开关关相同）"
else
    bad "cross-region：默认/开关语义不一致（on[c=$xr_on_c rc=$rn_rc out=$rn_out] off[c=$xr_off_c rc=$on_rc out=$on_out]）"
fi

# 登记量与轮数解耦：rs_inc 随轮数涨（登记在发），rs_hwm 不许涨（撤销及时）。
if [ -n "$rn_hwm" ] && [ -n "$rf_hwm" ] && [ -n "$rn_inc" ] && [ -n "$rf_inc" ] \
   && [ "$rf_inc" -gt "$rn_inc" ] && [ "$rf_hwm" -le $((rn_hwm * 2)) ] && [ "$rf_hwm" -lt 200000 ]; then
    ok "cross-region：登记在发（rs_inc ${rn_inc}→${rf_inc}）而高水位不随轮数涨（rs_hwm ${rn_hwm}→${rf_hwm}）—— 来源侧撤销及时"
else
    bad "cross-region：登记量判据破了（N: rs_inc=$rn_inc rs_hwm=$rn_hwm / 4N: rs_inc=$rf_inc rs_hwm=$rf_hwm）"
fi

# 峰值判据跟本次调用的开关状态走（`EXTC_RECLAIM=0 ./run-switch.sh` 时这条必须红）。
if [ "$sw" = on ]; then xr_tag=r; xr_name="开关开"; xr_off_ratio=$(ratio_of "$on_rss" "$of_rss"); else xr_tag=o; xr_name="开关关"; xr_off_ratio=$(ratio_of "$rn_rss" "$rf_rss"); fi
xr_run "$xr_tag" 200000; xn_rc=$XR_RC; xn_rss=$XR_RSS
xr_run "$xr_tag" 800000; xf_rc=$XR_RC; xf_rss=$XR_RSS
if [ "$xn_rc" = 0 ] && [ "$xf_rc" = 0 ] && [ -n "$xn_rss" ] && [ -n "$xf_rss" ]; then
    ratio=$(ratio_of "$xn_rss" "$xf_rss")
    if awk -v a="$xn_rss" -v b="$xf_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "cross-region N→4N（$xr_name）：$(rss_mb "$xn_rss") → $(rss_mb "$xf_rss")（${ratio}× < 1.5×）—— 跨区边不把旧对象钉住"
    else
        bad "cross-region N→4N（$xr_name）：$(rss_mb "$xn_rss") → $(rss_mb "$xf_rss")（${ratio}× ≥ 1.5×）—— 峰值仍随轮数涨（另一档 ${xr_off_ratio}×）"
    fi
else
    bad "cross-region N→4N：两次运行没同时跑对（N: rc=$xn_rc / 4N: rc=$xf_rc）"
fi

# ── ② 开关只换运行期，不改语义 ────────────────────────────────────────────────
echo "-- ② 开关下语义不变 --"
build_run tests/reclaim-shapes/cond_save.extc
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] && [ "$BR_OUT" = "saved=7" ]; then
    ok "cond-save：saved=7（与开关关时一致）"
else
    bad "cond-save：c=$BR_C rc=$BR_RC out=$BR_OUT（期望 saved=7）"
fi
build_run tests/reclaim-shapes/cyclic_shape.extc
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] && [ "$BR_OUT" = "fwd=28 back=28 tree=true cache=true" ]; then
    ok "cyclic-shape：fwd=back / tree / cache 全对（与开关关时一致）"
else
    bad "cyclic-shape：c=$BR_C rc=$BR_RC out=$BR_OUT（期望 fwd=28 back=28 tree=true cache=true）"
fi

# ── ③ 曾经的 SEGV 用例：开关下不崩 ────────────────────────────────────────────
# 板/池/栈/C 返回的指针不是 rc 槽。开关把计数挂在指针赋值上，这些形状都会经过 inc/dec/register；
# 没有 `owns` 守卫就会按位与到无关内存（板的 4 GiB 保留区没 commit）⇒ SEGV。判据 = rc=0 + 输出不变。
echo "-- ③ 曾经的 SEGV 用例（开关下 rc=0 且输出不变）--"

build_run tests/heap/loan.extc
loan_want="alloc=ok write=ok ro=true read=after-ro rw=true write2=ok closed=0"
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] && [ "$BR_OUT" = "$loan_want" ]; then
    ok "tests/heap/loan：rc=0，$loan_want"
else
    bad "tests/heap/loan：c=$BR_C rc=$BR_RC out=$BR_OUT（期望 rc=0 / $loan_want）"
fi

build_run tests/heap/plate.extc
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] \
   && printf '%s' "$BR_OUT" | grep -qF "reserve=4096MB" \
   && printf '%s' "$BR_OUT" | grep -qF "stored=11" \
   && printf '%s' "$BR_OUT" | grep -qF "holds=true" \
   && printf '%s' "$BR_OUT" | grep -qF "outside=false" \
   && printf '%s' "$BR_OUT" | grep -qF "copyin_teeth=-1" \
   && printf '%s' "$BR_OUT" | grep -qF "reuse=ok" \
   && printf '%s' "$BR_OUT" | grep -qF "closed=0"; then
    rss_d=$(printf '%s' "$BR_OUT" | grep -o 'rss_delta=[0-9]*' | cut -d= -f2)
    com=$(printf '%s' "$BR_OUT" | grep -o 'commit=[0-9]*' | cut -d= -f2)
    if [ "${rss_d:-999999}" -lt 4096 ] && [ "${com:-999999}" -le 4096 ]; then
        ok "tests/heap/plate：rc=0，保留 4 GiB 只涨 ${rss_d} KB · commit ${com} KB"
    else
        bad "tests/heap/plate：保留判据破了（rss_delta=${rss_d} KB / commit=${com} KB）"
    fi
else
    bad "tests/heap/plate：c=$BR_C rc=$BR_RC out=$BR_OUT"
fi

# heapmain 经 `dl::open("build/cabi-heapmod.so")` 用宿主建的板表：缺 .so 就先按 cabi/run.sh 的
# 方式编一份（只写构建产物，不动源码）。
if [ ! -f build/cabi-heapmod.so ]; then
    if ! "$CC" -std=c11 -fPIC -shared -o build/cabi-heapmod.so tests/cabi/heapmod.c 2>"$TMP/heapmod.log"; then
        echo "  note 先编 build/cabi-heapmod.so 失败（下面 heapmain 会因此报红）"; head -3 "$TMP/heapmod.log" | sed 's/^/        /'
    fi
fi
build_run tests/cabi/heapmain.extc
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] && [ "$BR_OUT" = "plate=ok byte=42 logged=2 outside=rejected" ]; then
    ok "tests/cabi/heapmain：rc=0，plate=ok byte=42 logged=2 outside=rejected"
else
    bad "tests/cabi/heapmain：c=$BR_C rc=$BR_RC out=$BR_OUT（期望 rc=0 / plate=ok byte=42 logged=2 outside=rejected）"
fi

build_run tests/coro/iterator_protocol.extc
ip_want=$(printf 'sum=15 shown=5\nempty=0\nsteps=4\nfor:sum=15 shown=5\nfor:empty=0\nfor:steps=4\nfield:while=6 for=6\nderef:while=6 for=6')
if [ "$BR_C" = ok ] && [ "$BR_RC" = 0 ] && [ "$BR_OUT" = "$ip_want" ]; then
    ok "tests/coro/iterator_protocol：rc=0，8 行输出逐字一致"
else
    bad "tests/coro/iterator_protocol：c=$BR_C rc=$BR_RC out=$(printf '%s' "$BR_OUT" | tr '\n' '|')"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
