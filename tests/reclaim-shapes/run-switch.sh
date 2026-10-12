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
#   ①c `cross_region` N vs 4N：跨区堆→堆边的登记 + 来源侧撤销（登记量必须与轮数解耦）。
#   ①d `cross_region_elem` N vs 4N：同一条洞落在**下标槽** `a[i]` 里 —— 修之前 `a[i]` 不登记
#      ⇒ 清扫种子不完整 ⇒ UAF（ASan SEGV / 非 ASan rc=139）；开关关时峰值随轮数涨（有牙）。
#   ①d′ `unknown_host`：宿主**证不出来**的槽（`(*id(a)).next = a`，基表达式带调用）必须走保守
#      路径（目标区域退出清扫），绝不许 `storeRefOwned(..., owner=0)` —— 生成物判据。
#   ①e **分片旋钮**（`EXTC_RC_SHARDS=8`）：语义与 `=1` **逐字一致**（分片只换存储/登记/清扫的
#      切分，不改语义），且峰值内存**仍在界内**（N vs 4N 比 < 1.5 —— 每片守的是均分后的压力
#      下限，总量上界不因片数改变）。开关关掉时这一条同样必须红（那一档峰值随轮数涨）。
#      分片的停顿/内存/吞吐对照实验在 `run-shard.sh`。
#   ①f `struct_copy`：**整值存储**携带引用叶子（`s.w = wrap { n: keeper }`）—— 目标槽不是
#      `ref T`，于是赋值走的是裸复合字面量拷贝，既没 `inc`、也没登记跨区边（与 `a7e2642`
#      同一类"槽写了但没记账"，走另一个机制）。判据：开关开 N/4N rc=0、`ok=true`、峰值比
#      < 1.5，且生成物里那条边是**逐叶记账**（`storeRefOwned`，owner = 容器对象 `s`）或保守
#      路径，不许是 `s->w = (wrap){.n = keeper};`；开关关时峰值随轮数涨（有牙）。
#   ①i **C 洞（真实语料）** `persist_nodes`（`bench/oi/persist/p3834_nodes.extc` 最小化版，
#      N=50000 ⇒ 根数组槽 512 KiB、全程无超大块）：`update` 递归，内层帧 `new` 的对象由**父帧
#      的局部量**持有，而触发点的静态根表按词法作用域拼（`{ z, prev }`）看不见它 ⇒ 内层帧用
#      不完整的种子扫**外层帧的家 arena** ⇒ 父帧还建着的子树被放掉 ⇒ 修之前开档 rc=139。
#      判据：开档（=1）与关档（=0）都 rc=0，5 行输出逐字一致。
#   ①j **C 洞（最小递归 + 非递归）** `hole_c` / `caller_frame`：`hole_c` 的触发根表**直接为空**
#      （`maybeCollect(区域, (void *const *)0, 0)`），`caller_frame` 证明与递归无关（修之前
#      挂住 rc=124）。判据：开档/关档都 rc=0，输出逐字 `n = 131071` / `n = 70000`。
#      这三条只钉**健全性**；它们的清扫损失（只往家 arena 分配的函数从此不触发）在
#      README 的「C 洞」一节如实写明，清扫能力的护栏仍是 ①/①b/①c/①e/①f 那几条。
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

# 开关：**默认打开**（`reclaim-default` 接线之后；带回收管辖分配点的程序自动带运行期）。
# 本脚本的两档一律**显式**给值，不看默认：
#   开档 = `EXTC_RECLAIM=1`（强制发射，与默认档在这些形状上等价）；
#   关档 = `EXTC_RECLAIM=0`（明确关掉）。⚠️ 不能再用 `env -u EXTC_RECLAIM` 表示"关"：
#   接线之后 unset 就是**默认打开**，那样两档是同一档，"关掉必须红"的证据就没了。
case "${EXTC_RECLAIM-1}" in
    0) sw=off ;;
    *) sw=on ;;
esac
export EXTC_RECLAIM=1     # `build_run` 那条路径走开档；关档的调用点各自显式覆盖成 0

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
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/cyclic_pressure.extc \
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
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/cross_region.extc \
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

# ── ①d cross-region 下标槽：`a[i]` 里的跨区入边也必须是种子 ─────────────────────
# 形状见 tests/reclaim-shapes/cross_region_elem.extc：与 ①c 同一个洞的另一半 —— 跨区堆→堆边
# 落在**下标槽** `s.kids[0]` 里（槽的存储仍在 `s` 的载荷里，宿主就是 `s`，但基表达式是 `a[i]`）。
# 修之前 codegen 只认纯地方基（`p->f` / `*p`），`a[i]` 落进"owner=0、不登记"那一档 ⇒ region 1
# 的清扫种子不完整 ⇒ 还被 `s->kids[0]` 指着的对象被放掉 ⇒ UAF：ASan 下 SEGV（地址是空闲链
# 槽号 0xb），非 ASan 直接 rc=139。修法见 codegen 的 `cgSlotHost` / 运行期
# `extc_rc_storeRefUnknown`：宿主证得出来（下标槽的宿主 = 容器对象）就登记跨区边；证不出来
# （视图元素、带调用的基表达式……）就让**目标所在区域整个退出清扫**，绝不在种子不完整时照常扫。
# 判据：
#   · 开关开：N 与 4N 都 rc=0、`ok=true`（sink = Σi），且峰值比 < 1.5（跨区边不把旧对象钉住）；
#   · 开关关：同形状峰值随轮数涨（≈4×）⇒ 这条红（有牙）。
echo "-- ①d cross-region 下标槽：a[i] 里的跨区堆→堆边必须登记（种子完整）--"
ELEM_WANT_N="rounds=200000 sink=19999900000 want=19999900000 ok=true"
ELEM_WANT_F="rounds=800000 sink=319999600000 want=319999600000 ok=true"

el_build() { # <on|off> <tag> → EL_C=ok/fail；开关开时另给 EL_SEED（生成物里槽的种子形态）
    EL_C=fail; EL_SEED=-
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map tests/reclaim-shapes/cross_region_elem.extc \
            -o "$TMP/el-$2.c" >"$TMP/el-$2.build.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/cross_region_elem.extc \
            -o "$TMP/el-$2.c" >"$TMP/el-$2.build.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/el-$2" "$TMP/el-$2.c" \
            >>"$TMP/el-$2.build.log" 2>&1; then EL_C=ok; fi
    # 生成物层面的判据（比"跑得恰好没崩"更直接）：往下标槽 `s.kids[...]` 里存 `keeper` 那一条，
    # 要么登记（owner = 容器对象 `s`），要么走保守路径（`storeRefUnknown`：目标区域退出清扫）。
    # 落到 `storeRefOwned(..., (void *)(0))`（旧写法）就是"seed 缺一条还照常扫"——必红。
    if [ "$1" = on ] && [ "$EL_C" = ok ]; then
        el_kids=$(grep -E 'storeRef(Owned|Unknown)' "$TMP/el-$2.c" | grep -F 'kids' | grep -F 'keeper')
        case "$el_kids" in
            *storeRefUnknown*) EL_SEED="保守路径（目标区域退出清扫）" ;;
            *'(void *)(s));'*) EL_SEED="登记（owner = 容器对象 s）" ;;
            *)                 EL_SEED="bad" ;;
        esac
    fi
}
el_run() { # <tag> <N> → EL_RC / EL_RSS / EL_OUT
    local tag=$1 n=$2
    EL_RC=-; EL_RSS=""; EL_OUT=""
    "$TIME" -v -o "$TMP/el-$tag.time" "$TMP/el-$tag" "$n" >"$TMP/el-$tag.out" 2>"$TMP/el-$tag.err"
    EL_RC=$?
    EL_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/el-$tag.time")
    EL_OUT=$(cat "$TMP/el-$tag.out")
}

el_build on  r; el_on_c=$EL_C; el_seed=$EL_SEED
el_build off o; el_off_c=$EL_C
el_run r 200000; ern_rc=$EL_RC; ern_out=$EL_OUT
el_run o 200000; eon_rc=$EL_RC; eon_out=$EL_OUT

# 语义：开关开/关的输出必须逐字一致（回收只换运行期）。
if [ "$el_on_c" = ok ] && [ "$el_off_c" = ok ] && [ "$ern_rc" = 0 ] && [ "$eon_rc" = 0 ] \
   && [ "$ern_out" = "$ELEM_WANT_N" ] && [ "$eon_out" = "$ELEM_WANT_N" ]; then
    ok "cross-region-elem：默认与开关下输出逐字一致（N=200000：sink=Σi 全对）"
else
    bad "cross-region-elem：默认/开关语义不一致或没跑对（on[c=$el_on_c rc=$ern_rc out=$ern_out] off[c=$el_off_c rc=$eon_rc out=$eon_out]）"
fi

# 生成物判据：下标槽那条跨区边的种子形态必须是**登记**或**保守路径**之一（旧写法是
# `storeRefOwned(..., owner=0)`：既没登记，也没让目标区域退出清扫 ⇒ seed 缺一条还照常扫）。
if [ "$el_on_c" = ok ] && [ "$el_seed" != bad ] && [ "$el_seed" != - ]; then
    ok "cross-region-elem：下标槽的跨区边种子完整（生成物：$el_seed）"
else
    bad "cross-region-elem：下标槽的跨区边既没登记也没走保守路径（生成物种子形态：$el_seed）—— seed 不完整还照常扫"
fi

# 峰值判据跟本次调用的开关状态走（`EXTC_RECLAIM=0 ./run-switch.sh` 时这条必须红）。
if [ "$sw" = on ]; then el_tag=r; el_name="开关开"; else el_tag=o; el_name="开关关"; fi
el_run "$el_tag" 200000; en_rc=$EL_RC; en_rss=$EL_RSS; en_out=$EL_OUT
el_run "$el_tag" 800000; ef_rc=$EL_RC; ef_rss=$EL_RSS; ef_out=$EL_OUT
if [ "$en_rc" = 0 ] && [ "$ef_rc" = 0 ] && [ "$en_out" = "$ELEM_WANT_N" ] && [ "$ef_out" = "$ELEM_WANT_F" ] \
   && [ -n "$en_rss" ] && [ -n "$ef_rss" ]; then
    ratio=$(ratio_of "$en_rss" "$ef_rss")
    if awk -v a="$en_rss" -v b="$ef_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "cross-region-elem N→4N（$el_name）：$(rss_mb "$en_rss") → $(rss_mb "$ef_rss")（${ratio}× < 1.5×）—— 下标槽的跨区边也是种子"
    else
        bad "cross-region-elem N→4N（$el_name）：$(rss_mb "$en_rss") → $(rss_mb "$ef_rss")（${ratio}× ≥ 1.5×）—— 峰值随轮数涨（门禁的'有牙'那一半）"
    fi
else
    bad "cross-region-elem N→4N：两次运行没同时跑对（N: rc=$en_rc out=$en_out / 4N: rc=$ef_rc out=$ef_out）"
fi

# ①d′ 宿主**证不出来**那一档（`(*id(a)).next = a`：基表达式带调用；视图元素 `v[i].f = x` 同档）。
# 硬要求是"证不出 owner 时那条槽绝不能照常扫"：生成物里该条赋值要么是 `storeRefUnknown`
# （把目标所在区域整个标成 noSweep），要么是登记了一个**真** owner；绝不许落回
# `storeRefOwned(..., (void *)(0))`（owner=0 = 既没登记、也没退出清扫 ⇒ seed 缺一条还照扫）。
echo "-- ①d′ 宿主证不出来：带调用的基表达式必须退出清扫，不许照常扫 --"
UH_WANT="got=7"
uh_build() { # <on|off> <tag> → UH_C=ok/fail；开关开时另给 UH_FORM（生成物里该条的形态）
    UH_C=fail; UH_FORM=-
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map tests/reclaim-shapes/unknown_host.extc \
            -o "$TMP/uh-$2.c" >"$TMP/uh-$2.build.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/unknown_host.extc \
            -o "$TMP/uh-$2.c" >"$TMP/uh-$2.build.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/uh-$2" "$TMP/uh-$2.c" \
            >>"$TMP/uh-$2.build.log" 2>&1; then UH_C=ok; fi
    if [ "$1" = on ] && [ "$UH_C" = ok ]; then
        uh_line=$(grep -E 'storeRef(Owned|Unknown)' "$TMP/uh-$2.c" | grep -F 'id(a)' | grep -F '.next')
        case "$uh_line" in
            *storeRefUnknown*)                 UH_FORM="保守路径（目标区域退出清扫）" ;;
            *storeRefOwned*', (void *)(0));'*) UH_FORM="bad" ;;
            *storeRefOwned*)                   UH_FORM="登记（宿主已证出）" ;;
            *)                                 UH_FORM="bad" ;;
        esac
    fi
}
uh_run() { # <tag> → UH_RC / UH_OUT
    UH_RC=-; UH_OUT=""
    "$TMP/uh-$1" >"$TMP/uh-$1.out" 2>"$TMP/uh-$1.err"; UH_RC=$?
    UH_OUT=$(cat "$TMP/uh-$1.out")
}

uh_build on r; uh_on_c=$UH_C; uh_form=$UH_FORM
uh_build off o; uh_off_c=$UH_C
uh_run r; uhr_rc=$UH_RC; uhr_out=$UH_OUT
uh_run o; uho_rc=$UH_RC; uho_out=$UH_OUT

if [ "$uh_on_c" = ok ] && [ "$uh_form" != bad ] && [ "$uh_form" != - ]; then
    ok "unknown-host：带调用的基表达式槽种子完整（生成物：$uh_form）"
else
    bad "unknown-host：宿主证不出来却既没登记也没退出清扫（生成物形态：$uh_form）—— seed 不完整还照常扫"
fi
if [ "$uh_off_c" = ok ] && [ "$uhr_rc" = 0 ] && [ "$uho_rc" = 0 ] \
   && [ "$uhr_out" = "$UH_WANT" ] && [ "$uho_out" = "$UH_WANT" ]; then
    ok "unknown-host：开关开/关输出逐字一致（$UH_WANT）"
else
    bad "unknown-host：开关开/关语义不一致（on[c=$uh_on_c rc=$uhr_rc out=$uhr_out] off[c=$uh_off_c rc=$uho_rc out=$uho_out]）"
fi

# ── ①e 分片旋钮：语义与单片逐字一致，峰值界不随片数丢 ──────────────────────────
# 分片（`EXTC_RC_SHARDS=N`）把一个逻辑区域切成 N 个**运行期区域**：每片自己的 slab 链、
# 自己的跨区登记位图（`rsbit`，即原型每片的入边集）、自己的标记栈，可以单独清扫 —— 就是
# 停顿旋钮。两条判据：
#   · 语义：`=8` 与 `=1` 的生成物跑出来逐字一致（片号只决定存储落在哪一片，不改语义）；
#   · 界：`=8` 下 N vs 4N 的峰值比仍 < 1.5（每片的压力下限 = 总下限 / 片数 ⇒ 总量上界不因
#     片数改变）。开关关掉时这一条跟 ① / ①b 一样必须红（那一档峰值随轮数涨）。
# 停顿/内存/吞吐的三组数字在 `run-shard.sh`（证据脚本，不是门禁）。
echo "-- ①e 分片（EXTC_RC_SHARDS=8）：语义与 =1 逐字一致，峰值仍在界内 --"

sh_build() { # <片数> <tag> [off] → SH_C=ok/fail；off = 不发射回收运行期
    SH_C=fail
    if [ "${3-}" = off ]; then
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/cyclic_pressure.extc \
            -o "$TMP/sh-$2.c" >"$TMP/sh-$2.log" 2>&1
    else
        EXTC_RECLAIM=1 EXTC_RC_SHARDS="$1" "$EXTC" -w --no-line-map tests/reclaim-shapes/cyclic_pressure.extc \
            -o "$TMP/sh-$2.c" >"$TMP/sh-$2.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/sh-$2" "$TMP/sh-$2.c" \
            >>"$TMP/sh-$2.log" 2>&1; then SH_C=ok; fi
}
sh_run() { # <tag> <N> → SH_RC / SH_RSS / SH_OUT
    SH_RC=-; SH_RSS=""; SH_OUT=""
    "$TIME" -v -o "$TMP/sh-$1.time" "$TMP/sh-$1" "$2" >"$TMP/sh-$1.out" 2>"$TMP/sh-$1.err"
    SH_RC=$?
    SH_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/sh-$1.time")
    SH_OUT=$(cat "$TMP/sh-$1.out")
}

sh_build 1 one;   sh1_c=$SH_C
sh_build 8 eight; sh8_c=$SH_C
sh_build 8 o off
sh_run one 200000;   s1_rc=$SH_RC; s1_out=$SH_OUT
sh_run eight 200000; s8_rc=$SH_RC; s8_out=$SH_OUT
if [ "$sh1_c" = ok ] && [ "$sh8_c" = ok ] && [ "$s1_rc" = 0 ] && [ "$s8_rc" = 0 ] \
   && [ "$s1_out" = "$CYC_WANT_N" ] && [ "$s8_out" = "$CYC_WANT_N" ] && [ "$s1_out" = "$s8_out" ]; then
    ok "shards=8：语义与 shards=1 逐字一致（N=200000，有环形状全对）"
else
    bad "shards=8：语义与 shards=1 不一致或没跑对（=1[c=$sh1_c rc=$s1_rc out=$s1_out] =8[c=$sh8_c rc=$s8_rc out=$s8_out]）"
fi

# 峰值判据跟本次调用的开关状态走：`EXTC_RECLAIM=0 ./run-switch.sh` 时用无回收那一档比 ⇒ 必红。
if [ "$sw" = on ]; then sh_tag=eight; sh_name="开关开 + 分片=8"; else sh_tag=o; sh_name="开关关"; fi
sh_run "$sh_tag" 200000; psn_rc=$SH_RC; psn_rss=$SH_RSS; psn_out=$SH_OUT
sh_run "$sh_tag" 800000; psf_rc=$SH_RC; psf_rss=$SH_RSS; psf_out=$SH_OUT
if [ "$psn_rc" = 0 ] && [ "$psf_rc" = 0 ] && [ "$psn_out" = "$CYC_WANT_N" ] && [ "$psf_out" = "$CYC_WANT_F" ] \
   && [ -n "$psn_rss" ] && [ -n "$psf_rss" ]; then
    ratio=$(ratio_of "$psn_rss" "$psf_rss")
    if awk -v a="$psn_rss" -v b="$psf_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "shards=8 N→4N（$sh_name）：$(rss_mb "$psn_rss") → $(rss_mb "$psf_rss")（${ratio}× < 1.5×）—— 分片没把内存界弄丢"
    else
        bad "shards=8 N→4N（$sh_name）：$(rss_mb "$psn_rss") → $(rss_mb "$psf_rss")（${ratio}× ≥ 1.5×）—— 分片把峰值界弄丢了"
    fi
else
    bad "shards=8 N→4N：两次运行没同时跑对（N: rc=$psn_rc out=$psn_out / 4N: rc=$psf_rc out=$psf_out）"
fi

# ── ①f 整值存储携带引用叶子：`s.w = wrap { n: keeper }` 也必须记账 ──────────────
# 形状见 tests/reclaim-shapes/struct_copy.extc：目标 `s.w` 的类型是**结构体** `wrap`（不是
# `ref T`），旧 codegen 只当它是数据字段赋值，发一条裸的复合字面量拷贝
# `s->w = (wrap){.n = keeper};` —— 那个新槽既没 `inc`、也没登记跨区边。`t` 被 `keeper = t`
# 提到 region 1，`keeper` 随后清空、`t` 还挂自环（计数永不归零）⇒ 唯一的回收者是 region 1 的
# 区域清扫；种子只剩"根 + 登记位"，而 `s.w.n` 这条跨区入边没登记 ⇒ `t` 被当垃圾放掉，
# `(*q).next` 读到空闲链槽号 ⇒ ASan SEGV（非 ASan rc=139）。
# 判据：
#   · 开关开：N 与 4N 都 rc=0、`ok=true`（sink = Σi），且峰值比 < 1.5；
#   · 生成物：往**结构体槽** `s.w.n` 存 `keeper` 的那一条必须是逐叶记账（`storeRefOwned`，
#     owner = 容器对象 `s`）或保守路径，不许是裸的 `s->w = (wrap){.n = keeper};`；
#   · 开关关：同形状峰值随轮数涨（≈3.7×）⇒ 这条红（有牙）。
echo "-- ①f 整值存储（结构体值拷贝）里的引用叶子必须 inc + 登记 --"
SC_WANT_N="rounds=200000 sink=19999900000 want=19999900000 ok=true"
SC_WANT_F="rounds=800000 sink=319999600000 want=319999600000 ok=true"

sc_build() { # <on|off> <tag> → SC_C=ok/fail；开关开时另给 SC_FORM（生成物里那条赋值的形态）
    SC_C=fail; SC_FORM=-
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map tests/reclaim-shapes/struct_copy.extc \
            -o "$TMP/sc-$2.c" >"$TMP/sc-$2.build.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map tests/reclaim-shapes/struct_copy.extc \
            -o "$TMP/sc-$2.c" >"$TMP/sc-$2.build.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/sc-$2" "$TMP/sc-$2.c" \
            >>"$TMP/sc-$2.build.log" 2>&1; then SC_C=ok; fi
    # 生成物层面的判据（比"跑得恰好没崩"更直接）：往结构体槽 `s->w.n` 存 `keeper` 的那一条，
    # 要么逐叶记账（`storeRefOwned` 的 owner = 容器对象 `s`），要么走保守路径。裸的
    # `s->w = (wrap){.n = keeper}` 就是"槽写了但没记账"——必红。
    if [ "$1" = on ] && [ "$SC_C" = ok ]; then
        if grep -qF 's->w = (wrap){.n = keeper}' "$TMP/sc-$2.c"; then
            SC_FORM="bad"
        elif grep -E 'storeRef(Owned|Unknown)' "$TMP/sc-$2.c" | grep -F '(__extc_svv' \
             | grep -qF ', (void *)(s));'; then
            SC_FORM="逐叶记账（owner = 容器对象 s）"
        elif grep -E 'storeRef(Owned|Unknown)' "$TMP/sc-$2.c" | grep -qF '(__extc_svv'; then
            SC_FORM="记账（宿主没证出容器对象）"
        else
            SC_FORM="bad"
        fi
    fi
}
sc_run() { # <tag> <N> → SC_RC / SC_RSS / SC_OUT
    local tag=$1 n=$2
    SC_RC=-; SC_RSS=""; SC_OUT=""
    "$TIME" -v -o "$TMP/sc-$tag.time" "$TMP/sc-$tag" "$n" >"$TMP/sc-$tag.out" 2>"$TMP/sc-$tag.err"
    SC_RC=$?
    SC_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/sc-$tag.time")
    SC_OUT=$(cat "$TMP/sc-$tag.out")
}

sc_build on  r; sc_on_c=$SC_C; sc_form=$SC_FORM
sc_build off o; sc_off_c=$SC_C
sc_run r 200000; scr_rc=$SC_RC; scr_out=$SC_OUT
sc_run o 200000; sco_rc=$SC_RC; sco_out=$SC_OUT

# 语义：开关开/关的输出必须逐字一致（回收只换运行期）。
if [ "$sc_on_c" = ok ] && [ "$sc_off_c" = ok ] && [ "$scr_rc" = 0 ] && [ "$sco_rc" = 0 ] \
   && [ "$scr_out" = "$SC_WANT_N" ] && [ "$sco_out" = "$SC_WANT_N" ]; then
    ok "struct-copy：默认与开关下输出逐字一致（N=200000：sink=Σi 全对）"
else
    bad "struct-copy：默认/开关语义不一致或没跑对（on[c=$sc_on_c rc=$scr_rc out=$scr_out] off[c=$sc_off_c rc=$sco_rc out=$sco_out]）"
fi

# 生成物判据：整值存储里那条引用叶子必须是**记账**过的（不是裸拷贝）。
if [ "$sc_on_c" = ok ] && [ "$sc_form" != bad ] && [ "$sc_form" != - ]; then
    ok "struct-copy：整值存储里的引用叶子逐叶记账（生成物：$sc_form）"
else
    bad "struct-copy：整值存储里的引用叶子没记账（生成物形态：$sc_form）—— 槽写了但没 inc / 没登记"
fi

# 峰值判据跟本次调用的开关状态走（`EXTC_RECLAIM=0 ./run-switch.sh` 时这条必须红）。
if [ "$sw" = on ]; then sc_tag=r; sc_name="开关开"; else sc_tag=o; sc_name="开关关"; fi
sc_run "$sc_tag" 200000; scn_rc=$SC_RC; scn_rss=$SC_RSS; scn_out=$SC_OUT
sc_run "$sc_tag" 800000; scf_rc=$SC_RC; scf_rss=$SC_RSS; scf_out=$SC_OUT
if [ "$scn_rc" = 0 ] && [ "$scf_rc" = 0 ] && [ "$scn_out" = "$SC_WANT_N" ] && [ "$scf_out" = "$SC_WANT_F" ] \
   && [ -n "$scn_rss" ] && [ -n "$scf_rss" ]; then
    ratio=$(ratio_of "$scn_rss" "$scf_rss")
    if awk -v a="$scn_rss" -v b="$scf_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "struct-copy N→4N（$sc_name）：$(rss_mb "$scn_rss") → $(rss_mb "$scf_rss")（${ratio}× < 1.5×）—— 整值存储的跨区边也是种子"
    else
        bad "struct-copy N→4N（$sc_name）：$(rss_mb "$scn_rss") → $(rss_mb "$scf_rss")（${ratio}× ≥ 1.5×）—— 峰值随轮数涨（门禁的'有牙'那一半）"
    fi
else
    bad "struct-copy N→4N：两次运行没同时跑对（N: rc=$scn_rc out=$scn_out / 4N: rc=$scf_rc out=$scf_out）"
fi

# ── ①g 超大块（槽 > 512 KiB）：不 trap，且语义与开关关逐字一致 ──────────────────
# 背景：slab 载荷固定 1 MiB，槽按 2 的幂取整；槽 > 512 KiB 时取整到 1 MiB，
# `(1 MiB - 头) >> 20 == 0` ⇒ 旧运行期在 `extc_rc_slabNew` 里
# `extc_rc_oops("slot is larger than a slab")`，rc=1。真实语料里有这一类分配：
# `tests/arena/control-flow` 的 `alloc<i32>(250000)`（1,000,000 B）、
# `bench/oi/persist/p3834` 的 `new [N]pnode`（252 MB）、`p3834_nodes` 的
# `new [1000001]?ref pnode`（8 MB）。修法：超大块的**载荷单独 malloc**，slab 头与
# 位图/每槽描述符/计数/登记/标记/清扫全部照旧 —— 它不是第二条回收路径，只是"载荷在哪"
# 不同（`EXTC_RC_SLAB_DATA`）。
# 判据：
#   · `big_slot`：①`alloc<i32>(250000)` ②600 KB 大槽里的元素读写 ③4 MiB 的 `alloc<u8>`
#     ④600 KB **引用类型**大槽链起来走一遍。开关开/关都 rc=0 且输出逐字一致。
#   · `big_slot_reclaim`：24 轮 × 4 个 1 MiB 大槽的丢弃压力，跨轮只由**局部根**指着的大槽
#     必须活下来（登记/标记/清扫对超大块一视同仁），输出仍与开关关逐字一致。
#   两档都要求 rc=0：这是修掉的 trap，不是"开关才有的行为"。
echo "-- ①g 超大块（槽 > 512 KiB）：不 trap，语义与开关关逐字一致 --"
BIG_WANT="alloc1m=41
blob=23
huge=9
chain=3"
BIGR_WANT="keeper=7 rounds=24 bad=0 z=0"

big_build() { # <on|off> <形状> <tag> → BIG_C=ok/fail
    BIG_C=fail
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "tests/reclaim-shapes/$2.extc" \
            -o "$TMP/big-$3.c" >"$TMP/big-$3.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "tests/reclaim-shapes/$2.extc" \
            -o "$TMP/big-$3.c" >"$TMP/big-$3.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/big-$3" "$TMP/big-$3.c" \
            >>"$TMP/big-$3.log" 2>&1; then BIG_C=ok; fi
}
big_run() { # <tag> → BIG_RC / BIG_OUT
    BIG_RC=-; BIG_OUT=""
    "$TMP/big-$1" >"$TMP/big-$1.out" 2>"$TMP/big-$1.err"; BIG_RC=$?
    BIG_OUT=$(cat "$TMP/big-$1.out")
}

big_build on  big_slot         bs_on;  bs_on_c=$BIG_C
big_build off big_slot         bs_off; bs_off_c=$BIG_C
big_run bs_on;  bs_on_rc=$BIG_RC;  bs_on_out=$BIG_OUT
big_run bs_off; bs_off_rc=$BIG_RC; bs_off_out=$BIG_OUT
if [ "$bs_on_c" = ok ] && [ "$bs_off_c" = ok ] && [ "$bs_on_rc" = 0 ] && [ "$bs_off_rc" = 0 ] \
   && [ "$bs_on_out" = "$BIG_WANT" ] && [ "$bs_off_out" = "$BIG_WANT" ]; then
    ok "big-slot：槽 > 512 KiB 不 trap 且语义正确（alloc<i32>(250000) / 4 MiB / 600 KB 引用槽；开关开=关）"
else
    bad "big-slot：trap 或语义不一致（on[c=$bs_on_c rc=$bs_on_rc out=$(printf '%s' "$bs_on_out" | tr '\n' '|')] off[c=$bs_off_c rc=$bs_off_rc out=$(printf '%s' "$bs_off_out" | tr '\n' '|')]）"
fi

big_build on  big_slot_reclaim bsr_on;  bsr_on_c=$BIG_C
big_build off big_slot_reclaim bsr_off; bsr_off_c=$BIG_C
big_run bsr_on;  bsr_on_rc=$BIG_RC;  bsr_on_out=$BIG_OUT
big_run bsr_off; bsr_off_rc=$BIG_RC; bsr_off_out=$BIG_OUT
if [ "$bsr_on_c" = ok ] && [ "$bsr_off_c" = ok ] && [ "$bsr_on_rc" = 0 ] && [ "$bsr_off_rc" = 0 ] \
   && [ "$bsr_on_out" = "$BIGR_WANT" ] && [ "$bsr_off_out" = "$BIGR_WANT" ]; then
    ok "big-slot-reclaim：有压力时超大块照样被登记/标记/清扫（跨轮的局部根大槽活下来）"
else
    bad "big-slot-reclaim：语义不一致（on[c=$bsr_on_c rc=$bsr_on_rc out=$(printf '%s' "$bsr_on_out" | tr '\n' '|')] off[c=$bsr_off_c rc=$bsr_off_rc out=$(printf '%s' "$bsr_off_out" | tr '\n' '|')]）"
fi

# ── ①h 泛型实例方法体：替换上下文必须还原（B：不许发未替换的模板类型）────────────
# 背景：`rcScanType` / `rcCollectOffsets` / `rcRootLeaves` / `rcLeafPaths` 探测类型时用
# `substEnter` 进一层实例上下文；早期 `substLeave` 把它**清空**而不是还原，于是
# `emitInstanceBodies` 的 `substEnter(实例)` 被嵌套探测抹掉，同一具方法体余下的部分以
# `T` 未替换的形态发出：`static inline int *slice_T_index(slice_T v, …)`、
# `static void varArray_grow(varArray_T * self, …)` ⇒ gcc `unknown type name 'slice_T'`。
# `examples/list-return.extc`（正例语料）就是这么红的。
# 判据（生成物 + 语义两条）：
#   · 开关开：生成 C **编得过**，且不含未替换的模板名（`slice_T` / `varArray_T` 词边界）；
#   · 开关开与关输出逐字一致。
echo "-- ①h 泛型实例方法体：替换上下文还原（B：不许 \`slice_T\` / \`varArray_T\`）--"
GID_WANT="n=8 head=7"

gid_build() { # <on|off> <源文件> <tag> → GID_C=ok/fail
    GID_C=fail
    if [ "$1" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$2" -o "$TMP/gid-$3.c" >"$TMP/gid-$3.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "$2" -o "$TMP/gid-$3.c" >"$TMP/gid-$3.log" 2>&1
    fi
    if [ $? -eq 0 ] && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/gid-$3" "$TMP/gid-$3.c" \
            >>"$TMP/gid-$3.log" 2>&1; then GID_C=ok; fi
}
gid_run() { # <tag> → GID_RC / GID_OUT
    GID_RC=-; GID_OUT=""
    "$TMP/gid-$1" >"$TMP/gid-$1.out" 2>"$TMP/gid-$1.err"; GID_RC=$?
    GID_OUT=$(cat "$TMP/gid-$1.out")
}

gid_build on  tests/reclaim-shapes/generic_instance_desc.extc gid_on;  gid_on_c=$GID_C
gid_build off tests/reclaim-shapes/generic_instance_desc.extc gid_off; gid_off_c=$GID_C
if [ "$gid_on_c" = ok ] && [ "$gid_off_c" = ok ]; then
    # 生成物判据：模板名没被替换就说明嵌套探测抹掉了上下文（旧写法）。
    if grep -qE '\bslice_T\b|\bvarArray_T\b' "$TMP/gid-on.c"; then
        bad "generic-instance-desc：生成 C 里有未替换的模板名（$(grep -oE '\b(slice_T|varArray_T)\b' "$TMP/gid-on.c" | sort -u | tr '\n' ' ')）—— 替换上下文被嵌套探测抹掉"
    else
        gid_run gid_on;  gid_on_rc=$GID_RC;  gid_on_out=$GID_OUT
        gid_run gid_off; gid_off_rc=$GID_RC; gid_off_out=$GID_OUT
        if [ "$gid_on_rc" = 0 ] && [ "$gid_off_rc" = 0 ] \
           && [ "$gid_on_out" = "$GID_WANT" ] && [ "$gid_off_out" = "$GID_WANT" ]; then
            ok "generic-instance-desc：泛型实例方法体编得过、跑得对（生成物无 slice_T/varArray_T；开关开=关）"
        else
            bad "generic-instance-desc：语义不一致（on[rc=$gid_on_rc out=$(printf '%s' "$gid_on_out" | tr '\n' '|')] off[rc=$gid_off_rc out=$(printf '%s' "$gid_off_out" | tr '\n' '|')]）"
        fi
    fi
else
    bad "generic-instance-desc：生成/编译失败（on=$gid_on_c off=$gid_off_c）—— 见 $TMP/gid-on.log"
fi

# ── ①i C 洞（真实语料）：persist_nodes —— 内层帧不得扫外层帧的家区域 ────────────
# 形状见 `persist_nodes.extc`（`bench/oi/persist/p3834_nodes.extc` 的最小化版，N=50000，
# 根数组 400008 B ⇒ 槽 512 KiB，**全程无超大块**）：`update` 递归建可持久化线段树，每个内层帧
# `new pnode` 拿到的 `n` 是活引用，但它在**父帧的局部量**里，而触发点的静态根表按词法作用域
# 拼（`update` 的生成物是 `{ z, prev }`）⇒ 看不见调用帧的 `n`。内层帧一触发就用这张不完整的
# 种子扫**外层帧的家 arena**（家 arena 一路传下来，是同一个区域）⇒ 把父帧还建着的子树放掉
# ⇒ 返回后 `prev` 悬垂 ⇒ rc=139。修法：触发只在**本帧拥有**的区域上下手（家 arena 不发触发）。
# 判据：开档（=1）与关档（=0）都 rc=0，且 5 行输出**逐字一致**（回收只换运行期，不改语义）。
PN_WANT="in    = 15541333246929082085
n     = 50000
q     = 1000
nodes = 834490
ans   = 13048423833364183566"

hole_build_run() { # <开关 1|0> <tag> <源文件> [程序参数...] → HB_C / HB_RC / HB_OUT
    local mode=$1 tag=$2 src=$3; shift 3
    HB_C=fail; HB_RC=-; HB_OUT=""
    if EXTC_RECLAIM=$mode "$EXTC" -w --no-line-map "$src" -o "$TMP/hole-$tag.c" \
            >"$TMP/hole-$tag.log" 2>&1 \
       && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/hole-$tag" "$TMP/hole-$tag.c" \
            >>"$TMP/hole-$tag.log" 2>&1; then
        HB_C=ok
        timeout 60 "$TMP/hole-$tag" "$@" >"$TMP/hole-$tag.out" 2>"$TMP/hole-$tag.err"
        HB_RC=$?
        HB_OUT=$(cat "$TMP/hole-$tag.out")
    fi
}

echo "-- ①i C 洞（真实语料最小化）：内层帧不得扫外层帧的家区域（开档=关档）--"
hole_build_run 1 pn_on  tests/reclaim-shapes/persist_nodes.extc
pn_on_c=$HB_C; pn_on_rc=$HB_RC; pn_on_out=$HB_OUT
hole_build_run 0 pn_off tests/reclaim-shapes/persist_nodes.extc
pn_off_c=$HB_C; pn_off_rc=$HB_RC; pn_off_out=$HB_OUT
if [ "$pn_on_c" = ok ] && [ "$pn_on_rc" = 0 ] && [ "$pn_on_out" = "$PN_WANT" ] \
   && [ "$pn_off_c" = ok ] && [ "$pn_off_rc" = 0 ] && [ "$pn_off_out" = "$PN_WANT" ]; then
    ok "persist-nodes：开档/关档都 rc=0 且输出逐字一致（N=50000 / Q=1000；修之前开档 rc=139）"
else
    bad "persist-nodes：开档/关档没同时跑对（on[c=$pn_on_c rc=$pn_on_rc out=$(printf '%s' "$pn_on_out" | tr '\n' '|')] off[c=$pn_off_c rc=$pn_off_rc out=$(printf '%s' "$pn_off_out" | tr '\n' '|')]）"
fi

# ── ①j C 洞（最小递归 / 非递归）：`hole_c.extc` 与 `caller_frame.extc` ─────────
# `hole_c`：外层调用帧的 `t` 连着已建好的子树，内层帧的触发根表是空的
# （`maybeCollect(区域, (void *const *)0, 0)`）⇒ 修之前整棵放掉、rc=139。
# `caller_frame`：同一个洞的**非递归**形态 —— `main` 手里的链头是活引用，被调 `fresh()` 只
# 负责 `new`；修之前是挂住（rc=124）。只对递归打补丁会漏掉这一半。
# 判据：开档与关档都 rc=0，输出分别逐字 `n = 131071` / `n = 70000`（两档都真跑、不空转）。
echo "-- ①j C 洞（最小递归 + 非递归）：外层帧的活引用不得被放掉 --"
hole_build_run 1 hc_on  tests/reclaim-shapes/hole_c/hole_c.extc
hc_on_c=$HB_C; hc_on_rc=$HB_RC; hc_on_out=$HB_OUT
hole_build_run 0 hc_off tests/reclaim-shapes/hole_c/hole_c.extc
hc_off_c=$HB_C; hc_off_rc=$HB_RC; hc_off_out=$HB_OUT
if [ "$hc_on_c" = ok ] && [ "$hc_on_rc" = 0 ] && [ "$hc_on_out" = "n = 131071" ] \
   && [ "$hc_off_c" = ok ] && [ "$hc_off_rc" = 0 ] && [ "$hc_off_out" = "n = 131071" ]; then
    ok "hole_c：开档/关档都 rc=0，输出逐字 n = 131071（修之前开档 rc=139）"
else
    bad "hole_c：开档/关档没同时跑对（on[c=$hc_on_c rc=$hc_on_rc out=$hc_on_out] off[c=$hc_off_c rc=$hc_off_rc out=$hc_off_out]）"
fi

hole_build_run 1 cf_on  tests/reclaim-shapes/hole_c/caller_frame.extc
cf_on_c=$HB_C; cf_on_rc=$HB_RC; cf_on_out=$HB_OUT
hole_build_run 0 cf_off tests/reclaim-shapes/hole_c/caller_frame.extc
cf_off_c=$HB_C; cf_off_rc=$HB_RC; cf_off_out=$HB_OUT
if [ "$cf_on_c" = ok ] && [ "$cf_on_rc" = 0 ] && [ "$cf_on_out" = "n = 70000" ] \
   && [ "$cf_off_c" = ok ] && [ "$cf_off_rc" = 0 ] && [ "$cf_off_out" = "n = 70000" ]; then
    ok "caller_frame：开档/关档都 rc=0，输出逐字 n = 70000（修之前开档 rc=124 挂住）"
else
    bad "caller_frame：开档/关档没同时跑对（on[c=$cf_on_c rc=$cf_on_rc out=$cf_on_out] off[c=$cf_off_c rc=$cf_off_rc out=$cf_off_out]）"
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
