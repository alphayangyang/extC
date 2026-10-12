#!/usr/bin/env bash
# tests/reclaim-gaps/run-gaps.sh —— 「现在还有哪些情况回收不掉」的**实测矩阵**一键复跑。
#
# 口径（与 tests/reclaim-shapes/run-switch.sh 完全一致，便于横向对照）：
#   · 开关开 = `EXTC_RECLAIM=1`，开关关 = `EXTC_RECLAIM=0`（同一份源码，只换运行期）；
#   · 峰值内存 = `/usr/bin/time -v` 的 Maximum resident set size，量的是**生成物二进制**；
#   · 语义 = 退出码 + stdout（开档与关档逐字比）；
#   · 「清扫次数」= 把生成物喂给 `tools/rc-pause-probe.py` 后运行，读 `PROBE sweeps=N`。
#     **只有这一条能证明「触发了并且真收了」**；生成物里有没有触发是另一回事，分开写。
#
# 本脚本**不做门禁、永远 exit 0**（跑了并打印结果就是成功）；红条目在矩阵里用 ✗ 标。
# 用法：
#   tests/reclaim-gaps/run-gaps.sh            # 全部形状
#   tests/reclaim-gaps/run-gaps.sh <name>...  # 只跑指定形状
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
PROBE=${PROBE:-tools/rc-pause-probe.py}
HERE=tests/reclaim-gaps
OUTDIR=${OUTDIR:-$HERE/out}
SHAPES=${SHAPES:-$HERE/shapes}
mkdir -p "$OUTDIR"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 只跑这些（空 = 全部）
WANT=("$@")

# ── 形状表：name | 源文件名（shapes/ 下）| 程序参数（`:` 分隔）| 标记 ───────────
# 标记：asan = 额外跑一遍 ASan/UBSan（只看 rc / 报告，不看峰值）；reject = 预期编译被拒。
# 每个形状只给**一个规模参数 N**；N vs 4N 用两条 spec 表达（同一把尺子的两个数）。
SPECS=(
  # ══ G1/G2：拥有者只调一次 / 被调者是长循环 ⇒ 家的房间里的环垃圾没人收 ══════
  "home_cycle_r1_n40000|home_cycle|1:40000|asan"
  "home_cycle_r1_n160000|home_cycle|1:160000|asan"
  # ══ G3：拥有者反复调 ⇒ 子集 B 的销账点应当每轮收一次（阳性对照）═════════
  "home_cycle_r64_n40000|home_cycle|64:40000|asan"
  "home_cycle_r256_n40000|home_cycle|256:40000|asan"
  # ══ G4：环在本帧拥有的房间里（阳性对照）═════════════════════════════════
  "own_room_cycle_n40000|own_room_cycle|40000|asan"
  "own_room_cycle_n160000|own_room_cycle|160000|asan"
  # ══ G5：圈跨作用域 ═══════════════════════════════════════════════════════
  "cross_scope_cycle_n40000|cross_scope_cycle|40000|asan"
  "cross_scope_cycle_n160000|cross_scope_cycle|160000|asan"
  # ══ G6：下标槽 a[i]（宿主认得出来）══════════════════════════════════════
  "slot_elem_known_n40000|slot_elem_known|40000|asan"
  "slot_elem_known_n160000|slot_elem_known|160000|asan"
  # ══ G7：视图元素槽 / 基表达式带调用（宿主追不到 ⇒ noSweep）══════════════
  "slot_elem_unknown_n40000|slot_elem_unknown|40000|asan"
  "slot_elem_unknown_n160000|slot_elem_unknown|160000|asan"
  "base_call_unknown_n40000|base_call_unknown|40000|asan"
  "base_call_unknown_n160000|base_call_unknown|160000|asan"
  # ══ G8：整值存储 / 按值返回 carrying ref ═════════════════════════════════
  "struct_copy_slot_n40000|struct_copy_slot|40000|asan"
  "struct_copy_slot_n160000|struct_copy_slot|160000|asan"
  "struct_copy_ret_home_n40000|struct_copy_ret_home|40000|asan"
  "struct_copy_ret_home_n160000|struct_copy_ret_home|160000|asan"
  # ══ G9：全局量里的引用（编译期拒）═══════════════════════════════════════
  "global_ref_reject|global_slot||reject"
  # ══ G10：C 层定长数组槽 ══════════════════════════════════════════════════
  "carray_slot_n40000|carray_slot|40000|asan"
  "carray_slot_n160000|carray_slot|160000|asan"
  # ══ G11：@overwrite 格子 ════════════════════════════════════════════════
  "owcell_garbage_n40000|owcell_garbage|40000|asan"
  "owcell_garbage_n160000|owcell_garbage|160000|asan"
  # ══ G12：协程帧 ═════════════════════════════════════════════════════════
  "coro_frame_cycle_n40000|coro_frame_cycle|40000|asan"
  "coro_frame_cycle_n160000|coro_frame_cycle|160000|asan"
  # ══ G13：ext/spawn 帧 ═══════════════════════════════════════════════════
  "ext_frame_cycle_n40000|ext_frame_cycle|40000|asan"
  "ext_frame_cycle_n160000|ext_frame_cycle|160000|asan"
  # ══ G14：match 绑定引用载荷（根不可靠）═════════════════════════════════
  "match_payload_n40000|match_payload|40000|asan"
  "match_payload_n160000|match_payload|160000|asan"
  # ══ G15：按值返回的纯临时量 carrying ref ════════════════════════════════
  "byval_temp_n40000|byval_temp|40000|asan"
  "byval_temp_n160000|byval_temp|160000|asan"
  # ══ G16：TLS / parallel worker 房间（新写的正向程序）═════════════════════
  "par_tls_cycle_n40000|par_tls_cycle|40000|asan"
  "par_tls_cycle_n160000|par_tls_cycle|160000|asan"
  # ══ G17：池容器里装的引用（pool<mut ref T>）══════════════════════════════
  "pool_ref_slot_n40000|pool_ref_slot|40000|asan"
  "pool_ref_slot_n160000|pool_ref_slot|160000|asan"
  # ══ G18：dyn 载荷 carrying ref ═══════════════════════════════════════════
  "dyn_payload_n40000|dyn_payload|40000|asan"
  "dyn_payload_n160000|dyn_payload|160000|asan"
  # ══ G19：嵌套闭包持有链 ═════════════════════════════════════════════════
  "closure_chain_n40000|closure_chain|40000|asan"
  "closure_chain_n160000|closure_chain|160000|asan"
)

ok_pick() { # 是否要跑这个形状
    [ ${#WANT[@]} -eq 0 ] && return 0
    local w
    for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

extract() { grep -c "$1" "$2" 2>/dev/null || true; }

# 编译一个 .extc 成可执行：build_one <src> <on|off> <tag> → B_C=ok/fail
B_C=fail
build_one() {
    local src=$1 mode=$2 tag=$3
    B_C=fail
    local log="$TMP/$tag.build.log"
    if [ "$mode" = on ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$src" -o "$TMP/$tag.c" >"$log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "$src" -o "$TMP/$tag.c" >"$log" 2>&1
    fi
    if [ $? -ne 0 ]; then return 0; fi
    "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$tag" "$TMP/$tag.c" >>"$log" 2>&1 || return 0
    B_C=ok
}

# 运行：run_one <tag> [args...] → R_RC / R_RSS / R_OUT
R_RC=-; R_RSS=""; R_OUT=""
run_one() {
    local tag=$1; shift
    R_RC=-; R_RSS=""; R_OUT=""
    "$TIME" -v -o "$TMP/$tag.time" "$TMP/$tag" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err"
    R_RC=$?
    R_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/$tag.time")
    R_OUT=$(cat "$TMP/$tag.out")
}

# 探针：扫次数 → P_SWEEPS（空 = 没插上 / 没跑）
P_SWEEPS=""
probe_sweeps() { # <tag> <args...>
    local tag=$1; shift
    P_SWEEPS=""
    python3 "$PROBE" "$TMP/$tag.c" >"$TMP/$tag.probe.c" 2>/dev/null || return 0
    "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/$tag.probe" \
        "$TMP/$tag.probe.c" >"$TMP/$tag.probe.log" 2>&1 || return 0
    "$TMP/$tag.probe" "$@" >"$TMP/$tag.probe.out" 2>"$TMP/$tag.probe.err"
    P_SWEEPS=$(cat "$TMP/$tag.probe.out" "$TMP/$tag.probe.err" 2>/dev/null | \
        awk 'match($0, /sweeps=[0-9]+/){ s=substr($0, RSTART+7, RLENGTH-7); print s }' | tail -1)
}

# ASan：只看 rc 与报告抬头
A_RC=-; A_HEAD=""
asan_run() { # <tag> <args...>
    local tag=$1; shift
    A_RC=-; A_HEAD=""
    "$CC" -std=c11 -fwrapv -O1 -g -fsanitize=address,undefined -o "$TMP/$tag.asan" \
        "$TMP/$tag.c" >"$TMP/$tag.asan.log" 2>&1 || { A_RC=CCFAIL; return 0; }
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=0 \
        "$TMP/$tag.asan" "$@" >"$TMP/$tag.asan.out" 2>"$TMP/$tag.asan.err"
    A_RC=$?
    A_HEAD=$(grep -m1 -E 'ERROR: AddressSanitizer|runtime error:' "$TMP/$tag.asan.err" | cut -c1-70)
}

rss_mb() { awk -v k="$1" 'BEGIN { if (k == "") printf "-"; else printf "%.2f", k/1024 }'; }
num_or() { if [ -z "$1" ]; then printf "-"; else printf "%s" "$1"; fi; }

printf '形状\t开关\trc\tsweeps\tRSS(MB)\t语义\tmaybeCollect\towe\tcheckOwed\tsrUnknown\tsrOwned\tsrCons\tinitRef\n' \
    >"$OUTDIR/matrix.tsv"
: >"$OUTDIR/raw.txt"

echo "== reclaim-gaps 实测矩阵 =="
echo "仓库 $(pwd) · HEAD $(git rev-parse --short HEAD 2>/dev/null || echo '?') · 编译器 $EXTC"
echo

for spec in "${SPECS[@]}"; do
    name=${spec%%|*}; rest=${spec#*|}
    srcbase=${rest%%|*}; rest=${rest#*|}
    args=${rest%%|*}; flags=${rest#*|}
    ok_pick "$name" || continue
    src="$SHAPES/$srcbase.extc"
    [ -f "$src" ] || { echo "!! 缺形状 $src"; continue; }
    IFS=':' read -r -a argv <<<"$args"

    on_out=""; off_out=""
    # on
    build_one "$src" on "$name-on"; on_c=$B_C
    if [ "$on_c" = ok ]; then
        run_one "$name-on" "${argv[@]}"; on_rc=$R_RC; on_rss=$R_RSS; on_out=$R_OUT
        probe_sweeps "$name-on" "${argv[@]}"; on_sw=$P_SWEEPS
    else
        on_rc=-; on_rss=""; on_sw=""
    fi
    on_err=""
    if [ "$on_c" != ok ]; then
        on_err=$(grep -m1 -E 'error:' "$TMP/$name-on.build.log" 2>/dev/null | cut -c1-110)
    fi
    # off
    build_one "$src" off "$name-off"; off_c=$B_C
    if [ "$off_c" = ok ]; then
        run_one "$name-off" "${argv[@]}"; off_rc=$R_RC; off_rss=$R_RSS; off_out=$R_OUT
    else
        off_rc=-; off_rss=""
    fi
    off_err=""
    if [ "$off_c" != ok ]; then
        off_err=$(grep -m1 -E 'error:' "$TMP/$name-off.build.log" 2>/dev/null | cut -c1-110)
    fi

    sem="?"
    if [ "$on_c" = ok ] && [ "$off_c" = ok ]; then
        if [ "$on_rc" = "$off_rc" ] && [ "$on_out" = "$off_out" ]; then sem="一致"; else sem="**不一致**"; fi
    fi

    mc=""; owe=""; co=""; su=""; so=""; sc=""; ir=""
    if [ "$on_c" = ok ]; then
        c="$TMP/$name-on.c"
        mc=$(extract 'extc_rc_maybeCollect(__extc_rcg' "$c")     # 触发点调用（不含运行期定义）
        owe=$(extract 'extc_rc_owe(__extc_rcw' "$c")             # 欠收记账调用
        co=$(extract 'extc_rc_checkOwed(__extc_rcc' "$c")        # 拥有者销账调用
        # 下面四条只匹配**调用**（定义是单括号 `(void **slot`，不会命中 `((void **)`）
        su=$(extract 'extc_rc_storeRefUnknown((void \*\*)' "$c")   # 追不到宿主 ⇒ 目标区域 noSweep
        so=$(extract 'extc_rc_storeRefOwned((void \*\*)' "$c")
        sc=$(extract 'extc_rc_storeStructCons((void \*)' "$c")      # 整值保守存储
        ir=$(extract 'extc_rc_initRef((void \*\*)' "$c")
    fi

    # ASan（只看正确性）
    asan_note=""
    if [ "$on_c" = ok ] && [[ ";$flags;" == *";asan;"* ]]; then
        asan_run "$name-on" "${argv[@]}"
        asan_note=" asan=${A_RC}${A_HEAD:+ [$A_HEAD]}"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" on "$on_rc" "$(num_or "$on_sw")" "$(rss_mb "$on_rss")" "$sem" \
        "$(num_or "$mc")" "$(num_or "$owe")" "$(num_or "$co")" "$(num_or "$su")" \
        "$(num_or "$so")" "$(num_or "$sc")" "$(num_or "$ir")" >>"$OUTDIR/matrix.tsv"

    if [ "$on_c" = ok ]; then
        printf '%-26s on rc=%-4s rss=%-9s sweeps=%-5s | off rc=%-4s rss=%-9s | sem=%s%s\n' \
            "$name" "$on_rc" "$(rss_mb "$on_rss")" "$(num_or "$on_sw")" \
            "$off_rc" "$(rss_mb "$off_rss")" "$sem" "$asan_note"
    else
        printf '%-26s 编译被拒（on/off 都拒）: %s\n' "$name" "$on_err"
    fi

    # 原始产物落盘（只留小的：stdout/stderr/生成物）
    {
        echo "### $name  args=${args}"
        echo "--- on: compile=$on_c rc=$on_rc rss_kb=$on_rss sweeps=${on_sw:-}"
        echo "$on_out"
        echo "--- off: compile=$off_c rc=$off_rc rss_kb=$off_rss"
        echo "$off_out"
        [ -n "$on_err" ] && echo "--- on error: $on_err"
        [ -n "$off_err" ] && echo "--- off error: $off_err"
        [ -n "$asan_note" ] && echo "--- asan:$asan_note"
    } >>"$OUTDIR/raw.txt"
    [ "$on_c" = ok ] && cp "$TMP/$name-on.c" "$OUTDIR/$name.on.c" 2>/dev/null
done

# ── 规模判据：每个形状 N=160000 vs N=640000（4×），on 档峰值比 < 1.5 = 收得掉 ──────
# 为什么不用矩阵里的 40k/160k：那两个规模常常还在 1 MiB 压力下限以下，比的是"底噪"。
# 每条给 on/off 两个峰值、on 档清扫次数（探针）、以及判定。
SCALE=(
  "home_cycle_r1|home_cycle|1:160000|1:640000"
  "home_cycle_r64|home_cycle|64:40000|256:40000"
  "own_room_cycle|own_room_cycle|160000|640000"
  "cross_scope_cycle|cross_scope_cycle|160000|640000"
  "slot_elem_known|slot_elem_known|160000|640000"
  "slot_elem_unknown|slot_elem_unknown|160000|640000"
  "base_call_unknown|base_call_unknown|160000|640000"
  "struct_copy_slot|struct_copy_slot|160000|640000"
  "struct_copy_ret_home|struct_copy_ret_home|160000|640000"
  "carray_slot|carray_slot|160000|640000"
  "owcell_garbage|owcell_garbage|160000|640000"
  "coro_frame_cycle|coro_frame_cycle|160000|640000"
  "ext_frame_cycle|ext_frame_cycle|160000|640000"
  "match_payload|match_payload|160000|640000"
  "byval_temp|byval_temp|160000|640000"
  "par_tls_cycle|par_tls_cycle|160000|640000"
  "pool_ref_slot|pool_ref_slot|160000|640000"
  "dyn_payload|dyn_payload|160000|640000"
  "closure_chain|closure_chain|160000|640000"
)

echo
echo "== 规模判据（N=160000 → 640000，同一档内比；on 档峰值比 < 1.5 = 界与规模解耦）=="
printf '形状\ton160(MB)\ton640(MB)\ton比\toff160(MB)\toff640(MB)\tsweeps160\tsweeps640\t判定\n' \
    >"$OUTDIR/scale.tsv"
for spec in "${SCALE[@]}"; do
    label=${spec%%|*}; r=${spec#*|}; srcbase=${r%%|*}; r=${r#*|}
    a1=${r%%|*}; a2=${r#*|}
    IFS=':' read -r -a v1 <<<"$a1"
    IFS=':' read -r -a v2 <<<"$a2"
    src="$SHAPES/$srcbase.extc"
    [ -f "$src" ] || continue
    build_one "$src" on "sc-$label"; [ "$B_C" = ok ] || continue
    run_one "sc-$label" "${v1[@]}"; n_rss=$R_RSS
    probe_sweeps "sc-$label" "${v1[@]}"; n_sw=$P_SWEEPS
    run_one "sc-$label" "${v2[@]}"; f_rss=$R_RSS
    probe_sweeps "sc-$label" "${v2[@]}"; f_sw=$P_SWEEPS
    build_one "$src" off "sco-$label"
    o_n=""; o_f=""
    if [ "$B_C" = ok ]; then
        run_one "sco-$label" "${v1[@]}"; o_n=$R_RSS
        run_one "sco-$label" "${v2[@]}"; o_f=$R_RSS
    fi
    ratio=$(awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { if (a=="" || b=="") printf "-"; else printf "%.2f", b/a }')
    verdict="?"
    if [ -n "$n_rss" ] && [ -n "$f_rss" ]; then
        if awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { exit !(b < a * 1.5) }'; then
            verdict="收得掉(有界)"
        else
            verdict="**收不掉(随规模涨)**"
        fi
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$(rss_mb "$n_rss")" "$(rss_mb "$f_rss")" "$ratio" \
        "$(rss_mb "$o_n")" "$(rss_mb "$o_f")" "$(num_or "$n_sw")" "$(num_or "$f_sw")" \
        "$verdict" >>"$OUTDIR/scale.tsv"
    printf '%-24s on %-9s→%-9s (%s×) sweeps %-4s/%-4s off %-9s→%-9s  %s\n' \
        "$label" "$(rss_mb "$n_rss")" "$(rss_mb "$f_rss")" "$ratio" \
        "$(num_or "$n_sw")" "$(num_or "$f_sw")" "$(rss_mb "$o_n")" "$(rss_mb "$o_f")" "$verdict"
done
echo
echo "规模判据 → $OUTDIR/scale.tsv"

echo
echo "矩阵 → $OUTDIR/matrix.tsv ；原始输出 → $OUTDIR/raw.txt"
exit 0
