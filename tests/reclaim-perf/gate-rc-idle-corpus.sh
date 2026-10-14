#!/usr/bin/env bash
# tests/reclaim-perf/gate-rc-idle-corpus.sh —— 「**不需要回收 ⇒ 整单元不发射 rc 运行期**」这条
# 判据（回收文档 §三点七）在**固定语料清单**上的**能力棘轮**。
#
# 为什么要有这条：②的收益是"产物里一个 rc 调用都没有"，而这是一条**只许往前走**的能力线 ——
# 每放开一类形状（例如"循环里建、每轮丢"的细化），清单里"产物无 rc"的形状数就该 +1。此前这个进度只散在
# 各条动态判据里（`gate-idle-bounded` 的形状名单、`gate-tax` 的税比），没有一个直接数"今天
# 到底能拿掉多少个形状"。数字不钉住就会**悄悄退回去**：某次改动让一个已经 idle 的形状重新
# 发射 rc，没有一条判据会红。
#
# 判据（两格）：
#   ① 固定清单里**产物里 `extc_rc_` 出现次数为 0** 的形状数 >= MIN_IDLE（棘轮，只许往上拧。
#      基线 8（"不需要回收"那一档已判 idle 的形状）→ 细化 (a′)+(b′)（"循环里建、每轮丢"）后
#      **14**：`rebuild` 两个源、`churn`、`closure_chain`、`big_slot_reclaim`、`rebuild_per_round`）；
#   ② **对照**：清单里必须至少有一个形状**仍然发射 rc**（`cyclic_pressure`）—— 否则这条判据
#      就退化成了"每个形状都 idle"，它证明不了任何取舍（防判据瞎）。
#
# 清单是**固定**的（不改写、不按目录扫）：它是棘轮的刻度，加形状 = 拧紧棘轮，删形状 = 放松，
# 两者都要在 diff 里看得见。`MIN_IDLE` 同理，只许往上。
#
# 口径：默认档（`EXTC_RECLAIM` 不设）**整串**数 `extc_rc_` 的出现次数（不是调用点、不是行数）。
# ②的证明成立时产物与 `EXTC_RECLAIM=0` 逐字节相同 ⇒ 计数为 0；`gate-idle-bounded` 另有一条
# 判据钉"逐字节相同"这件事本身。
#
# 用法：tests/reclaim-perf/gate-rc-idle-corpus.sh
#   EXTC=…       换编译器
#   MIN_IDLE=…   覆盖棘轮线（调试 / 自检"闸门有牙"）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
EXTC=${EXTC:-$ROOT/build/extc}
MIN_IDLE=${MIN_IDLE:-17}

# 固定语料清单（形状名|源文件）。**只许增长**。
SHAPES="
bounded_churn|tests/reclaim-shapes/bounded_churn.extc
bt|bench/heavy/bt.extc
cdq|bench/bigmatrix/src/cdq.extc
mandel|bench/bigmatrix/src/mandel.extc
radix|bench/bigmatrix/src/radix.extc
log|bench/app/log.extc
route|bench/app/route.extc
session|bench/app/session.extc
rebuild_gc|bench/gc/src/rebuild.extc
rebuild_bigmatrix|bench/bigmatrix/src/rebuild.extc
churn|bench/gc/src/churn.extc
closure_chain|tests/reclaim-gaps/shapes/closure_chain.extc
big_slot_reclaim|tests/reclaim-shapes/big_slot_reclaim.extc
rebuild_per_round|tests/reclaim-perf/shapes/rebuild_per_round.extc
struct_copy_ret_home|tests/reclaim-gaps/shapes/struct_copy_ret_home.extc
persist_nodes|tests/reclaim-shapes/persist_nodes.extc
p3834_nodes|bench/oi/persist/p3834_nodes.extc
"
# 对照：必须仍在回收档（产物里有 rc 运行期）—— 防"判据一律放行"。
CONTROL="cyclic_pressure|tests/reclaim-shapes/cyclic_pressure.extc"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== ② 的能力棘轮：固定语料清单里产物无 rc 的形状数 ≥ $MIN_IDLE =="

fail=0
idle=0
for row in $SHAPES; do
    name=${row%%|*}; src=$ROOT/${row#*|}
    if [ ! -f "$src" ]; then bad "$name：形状不存在（$src）"; fail=1; continue; fi
    out=$TMP/$name.c
    if ! "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$src" -o "$out" >"$TMP/$name.log" 2>&1; then
        bad "$name：默认档生成失败（$(sed -n '1p' "$TMP/$name.log")）"; fail=1; continue
    fi
    c=$(grep -c 'extc_rc_' "$out")
    if [ "$c" = 0 ]; then
        idle=$((idle + 1)); printf '  ok   %-20s rc=0（整单元不发射运行期）\n' "$name"
    else
        printf '  --   %-20s rc=%s（仍在回收档）\n' "$name" "$c"
    fi
done

echo
cname=${CONTROL%%|*}; csrc=$ROOT/${CONTROL#*|}
if ! "$EXTC" -w --no-line-map -I "$ROOT/stdlib" "$csrc" -o "$TMP/ctrl.c" >/dev/null 2>&1; then
    bad "对照 $cname：生成失败 —— 这条对照没量到，不算通过"; fail=1
else
    cc=$(grep -c 'extc_rc_' "$TMP/ctrl.c")
    if [ "$cc" -gt 0 ]; then
        ok "对照 $cname：仍在回收档（rc=$cc）—— 判据没有一律放行"
    else
        bad "对照 $cname：它**也被判成 idle** —— 判据瞎了（这条形状会丢引用）"; fail=1
    fi
fi

echo
if [ "$idle" -ge "$MIN_IDLE" ]; then
    ok "能力棘轮：$idle 个形状产物无 rc（≥ $MIN_IDLE）"
else
    bad "能力棘轮：只有 $idle 个形状产物无 rc（要 ≥ $MIN_IDLE）—— ②退回了一步"
    fail=1
fi
echo "  （清单与 MIN_IDLE 都是棘轮：加形状/往上调 = 拧紧，删形状/往下调 = 放松，diff 里看得见）"
exit "$fail"
