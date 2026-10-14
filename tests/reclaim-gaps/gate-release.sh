#!/usr/bin/env bash
# tests/reclaim-gaps/gate-release.sh —— 「覆盖点定点释放」的判据（先红后绿）。
#
# 形状 `shapes/ring_per_round.extc`：每轮在循环体里建一条 256 结点的**闭环**，环头存进
# 长命槽 `keep`（`keep = head`），下一轮这个槽被覆盖 ⇒ 上一轮的环再也没有外部持有者。
# 定点释放就是在覆盖点沿类型描述符把那一环的槽逐个 `extc_rc_freeSlot`，不必等区域清扫。
#
# 四条判据（默认档 = `EXTC_RECLAIM=1`）：
#   A 清扫次数 ≤ 1：垃圾在覆盖点就没了 ⇒ 第 2 轮起区域占用到不了压力线。今天（没有定点释放）
#     是 13 / 55 次（N=20 / 80）。**剩下的那 1 次是第 2 轮循环头的启动空扫**：`keep` 的旧 head
#     到第 2 轮的 `keep = head` 才被判死，而第 2 轮循环体开头的分配触发点先到压力线（那一次
#     `freed=0`，自学习还把 `scale` 翻倍）—— 那是触发策略自己的事，不是"释放晚了"。所以这一条
#     **先红后绿**：红 = 13/55，绿 = ≤1 且**不随 N 增长**（N=20 与 N=80 各 ≤1）。
#   B N(20) vs 4N(80) 峰值 RSS 比 < 1.5：界与轮数解耦。今天已经绿，留着是钉子 —— 定点释放
#     是把"清扫给的界"换成"释放给的界"，不许把界弄坏（口径与 run-gaps.sh 的 SCALE 段相同）。
#   C 语义：默认档的退出码与 stdout 与关档（`EXTC_RECLAIM=0`）逐字相同。
#   D **释放槽数 > 0**：光看 A 分不出"释放生效"与"发射空转"（把发射关掉，峰值靠清扫也平）
#     ⇒ 必须同时证明**真的放掉了槽**。数来自生成物的 `EXTC_RC_STATS=1`
#     （`extc_rc_stats … releases=N release_slots=M`）；一行至少放掉整整一环（256 个槽）才说明
#     沿类型描述符走完了一棵树。
#
# 口径（与 tests/reclaim-shapes/run-switch.sh / tests/reclaim-gaps/run-gaps.sh 逐字一致）：
#   · 清扫次数 = 生成物喂给 `tools/rc-pause-probe.py` 后运行，读 `PROBE sweeps=N`；
#   · 峰值 = `/usr/bin/time -v` 的 Maximum resident set size，量的是**生成物二进制**；
#   · 每张表都打印**程序自己的 stdout**（仪器与被测程序两边对得上）。
# 用法：bash tests/reclaim-gaps/gate-release.sh
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
PROBE=${PROBE:-tools/rc-pause-probe.py}
SRC=tests/reclaim-gaps/shapes/ring_per_round.extc
N_SMALL=20
N_BIG=80
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok=0; fail=0
say() { printf '%s\n' "$*"; }
check() { # check <判据名> <0=通过>
    if [ "$2" -eq 0 ]; then ok=$((ok + 1)); say "  ok  $1"
    else fail=$((fail + 1)); say "  FAIL  $1"; fi
}

build() { # build <on|off>
    local mode=$1 v=0
    [ "$mode" = on ] && v=1
    EXTC_RECLAIM=$v "$EXTC" -w --no-line-map "$SRC" -o "$TMP/$mode.c" >"$TMP/$mode.build.log" 2>&1 || return 1
    "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$mode" "$TMP/$mode.c" >>"$TMP/$mode.build.log" 2>&1 || return 1
}

R_RSS=""; R_OUT=""; R_RC=-; R_SWEEPS=""
run_plain() { # run_plain <mode> <n> → R_RSS / R_OUT / R_RC
    R_RSS=""; R_OUT=""; R_RC=-
    "$TIME" -v -o "$TMP/$1.$2.time" "$TMP/$1" "$2" >"$TMP/$1.$2.out" 2>"$TMP/$1.$2.err"
    R_RC=$?
    R_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/$1.$2.time")
    R_OUT=$(cat "$TMP/$1.$2.out")
}
run_sweeps() { # run_sweeps <mode> <n> → R_SWEEPS / R_RELSLOTS / R_RELEASES / R_STDOUT
    R_SWEEPS=""; R_RELSLOTS=""; R_RELEASES=""; R_STDOUT=""
    python3 "$PROBE" "$TMP/$1.c" >"$TMP/$1.probe.c" 2>/dev/null || return 0
    "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/$1.probe" \
        "$TMP/$1.probe.c" >"$TMP/$1.probe.log" 2>&1 || return 0
    EXTC_RC_STATS=1 "$TMP/$1.probe" "$2" >"$TMP/$1.$2.probe.out" 2>"$TMP/$1.$2.probe.err"
    R_STDOUT=$(cat "$TMP/$1.$2.probe.out")
    R_SWEEPS=$(cat "$TMP/$1.$2.probe.out" "$TMP/$1.$2.probe.err" 2>/dev/null | \
        awk 'match($0, / sweeps=[0-9]+/){ s=substr($0, RSTART+8, RLENGTH-8); print s }' | tail -1)
    R_RELEASES=$(cat "$TMP/$1.$2.probe.err" 2>/dev/null | \
        awk 'match($0, / releases=[0-9]+/){ s=substr($0, RSTART+10, RLENGTH-10); print s }' | tail -1)
    R_RELSLOTS=$(cat "$TMP/$1.$2.probe.err" 2>/dev/null | \
        awk 'match($0, / release_slots=[0-9]+/){ s=substr($0, RSTART+15, RLENGTH-15); print s }' | tail -1)
}
rss_mb() { awk -v k="${1:-}" 'BEGIN { if (k == "") printf "-"; else printf "%.2f", k / 1024 }'; }
num_or() { [ -n "${1:-}" ] && echo "$1" || echo "-"; }

say "== 定点释放判据：ring_per_round（每轮一条 256 结点闭环，覆盖点应释放）=="
if ! build on;  then say "  FAIL  默认档编译/构建失败";  exit 1; fi
if ! build off; then say "  FAIL  关档编译/构建失败";    exit 1; fi

run_plain on  "$N_SMALL"; on_rss_s=$R_RSS; on_out_s=$R_OUT; on_rc_s=$R_RC
run_plain on  "$N_BIG";   on_rss_b=$R_RSS; on_out_b=$R_OUT; on_rc_b=$R_RC
run_plain off "$N_SMALL"; off_rss_s=$R_RSS; off_out_s=$R_OUT; off_rc_s=$R_RC
run_plain off "$N_BIG";   off_rss_b=$R_RSS; off_out_b=$R_OUT; off_rc_b=$R_RC
run_sweeps on "$N_SMALL"; sw_s=$R_SWEEPS; rel_s=$R_RELEASES; relslots_s=$R_RELSLOTS; probe_out_s=$R_STDOUT
run_sweeps on "$N_BIG";   sw_b=$R_SWEEPS; rel_b=$R_RELEASES; relslots_b=$R_RELSLOTS; probe_out_b=$R_STDOUT

ratio=$(awk -v a="$on_rss_s" -v b="$on_rss_b" \
    'BEGIN { if (a == "" || b == "" || a + 0 == 0) printf "-"; else printf "%.2f", b / a }')

say "  默认档 N=$N_SMALL：sweeps=$(num_or "$sw_s")  releases=$(num_or "$rel_s")  release_slots=$(num_or "$relslots_s")"
say "           RSS=$(rss_mb "$on_rss_s")MB  rc=$on_rc_s  stdout=[$probe_out_s]"
say "  默认档 N=$N_BIG  ：sweeps=$(num_or "$sw_b")  releases=$(num_or "$rel_b")  release_slots=$(num_or "$relslots_b")"
say "           RSS=$(rss_mb "$on_rss_b")MB  rc=$on_rc_b  N vs 4N=$ratio×  stdout=[$probe_out_b]"
say "  关档   N=$N_SMALL：RSS=$(rss_mb "$off_rss_s")MB  stdout=[$on_out_s]"
say "  关档   N=$N_BIG  ：RSS=$(rss_mb "$off_rss_b")MB  stdout=[$off_out_b]"

# A：清扫次数 ≤ 1（先红：今天 13 / 55；那 1 次是第 2 轮启动空扫，见文件头）
if [ -z "$sw_s" ] || [ -z "$sw_b" ]; then
    check "A 清扫次数 ≤ 1（探针没拿到 sweeps：$(num_or "$sw_s")/$(num_or "$sw_b")）" 1
else
    check "A 清扫次数 ≤ 1（实测 $(num_or "$sw_s")/$(num_or "$sw_b")）" \
        "$(awk -v a="$sw_s" -v b="$sw_b" 'BEGIN { exit !(a <= 1 && b <= 1) }'; echo $?)"
fi

# B：N vs 4N 峰值比 < 1.5
if [ "$ratio" = "-" ]; then
    check "B N vs 4N 峰值比（没量到 RSS）" 1
else
    check "B N vs 4N 峰值比 $ratio× < 1.5" \
        "$(awk -v r="$ratio" 'BEGIN { exit !(r + 0 < 1.5) }'; echo $?)"
fi

# C：语义逐字一致（开档 vs 关档）
sem=1
if [ "$on_rc_s" != "$off_rc_s" ] || [ "$on_rc_b" != "$off_rc_b" ] || \
   [ "$on_out_s" != "$off_out_s" ] || [ "$on_out_b" != "$off_out_b" ]; then sem=1; else sem=0; fi
check "C 默认档语义与关档逐字相同（rc/stdout）" "$sem"

# D：这次释放真放掉了槽（不是发射空转）；一行至少放掉整整一环 256 个槽
if [ -z "$relslots_s" ] || [ -z "$relslots_b" ]; then
    check "D 释放槽数 > 0（没拿到 release_slots：$(num_or "$relslots_s")/$(num_or "$relslots_b")）" 1
else
    check "D 释放槽数 > 0（releases $(num_or "$rel_s")/$(num_or "$rel_b")，槽 $(num_or "$relslots_s")/$(num_or "$relslots_b")）" \
        "$(awk -v a="$relslots_s" -v b="$relslots_b" 'BEGIN { exit !(a >= 256 && b >= 256) }'; echo $?)"
fi

say "通过 $ok / $((ok + fail)) 项"
[ "$fail" -eq 0 ]
