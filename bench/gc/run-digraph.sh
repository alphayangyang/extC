#!/usr/bin/env bash
# bench/gc/run-digraph.sh —— 形状 7 digraph 的**新**卡具（不动既有 run.sh / measure.py）。
#
# 口径（照 SHAPES.md / DIGRAPH.md）：
#   - 时间：best-of-REPS（默认 5），绑核（sched_setaffinity，子进程继承）；
#   - 峰值 RSS：wait4 的 rusage.ru_maxrss = **内核值**（不用 RUSAGE_CHILDREN 差值）；
#     时间与 RSS 都由自带的 C 垫片 digraph_run 取：clock_gettime 只包被测程序，
#     不加 taskset/date/shell 的启动开销；垫片自身 RSS ~1.5MB，
#     避免"python fork 出 ~10MB 前的 COW 被算进子进程 maxrss"那类量具地板假象。
#   - 丢弃计数**分因**：rc!=0 · 行数≠2 · 格式不对（各自计数，见 selftest 校准）；
#   - 同一 N/seed 下所有实现输出必须逐字节一致，否则**不印时间表**（数字不算数）；
#   - 每组都带上"程序自己的输出"。
#
# 用法：
#   bash bench/gc/run-digraph.sh align        # 只做对齐检查（不测时间）
#   bash bench/gc/run-digraph.sh selftest     # 丢弃分类校准（喂坏生产者，不测真形状）
#   bash bench/gc/run-digraph.sh              # 对齐 + 计时（best-of-REPS）
#   N1=2000 N2=8000 REPS=3 bash bench/gc/run-digraph.sh
#   SKIP_BUILD=1 bash bench/gc/run-digraph.sh
#   EXTC_BIN=/tmp/gcbench/extc.pin1 bash bench/gc/run-digraph.sh
set -u
cd "$(dirname "$0")"

MODE=${1:-bench}
N1=${N1:-250000}
N2=${N2:-1000000}
SEED=${SEED:-20261013}
REPS=${REPS:-5}
PIN=${PIN:-0}
B=build
SKIP_BUILD=${SKIP_BUILD:-0}
EXTC_BIN=${EXTC_BIN:-../../build/extc}
STDLIB=${STDLIB:-../../stdlib}
CXX=${CXX:-g++}
CC=${CC:-cc}
RUSTC=${RUSTC:-rustc}
GO=${GO:-go}
JAVAC=${JAVAC:-javac}
JAVA=${JAVA:-java}
NODE=${NODE:-node}
SIZES=${SIZES:-"$N1 $N2"}
PROBE=${PROBE:-../../tools/rc-pause-probe.py}
RAW=$B/digraph-raw.txt

mkdir -p "$B"

# ---------- 编译（命令原文会被记录） ----------
if [ "$SKIP_BUILD" != "1" ]; then
    echo "== 编译 =="
    if [ ! -x "$EXTC_BIN" ]; then
        echo "ERROR: extC 编译器不存在/不可执行：$EXTC_BIN" >&2
        exit 2
    fi
    echo "  extC 编译器: $EXTC_BIN  md5=$(md5sum "$EXTC_BIN" | cut -d' ' -f1)  mtime=$(date -r "$EXTC_BIN" '+%F %T')"
    set -x
    env -u EXTC_RECLAIM -u EXTC_RC_SHARDS "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/digraph_extc.c" src/digraph.extc \
      && $CC -O2 -o "$B/digraph_extc" "$B/digraph_extc.c"
    env -u EXTC_RC_SHARDS EXTC_RECLAIM=0 "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/digraph_extc_noreclaim.c" src/digraph.extc \
      && $CC -O2 -o "$B/digraph_extc_noreclaim" "$B/digraph_extc_noreclaim.c"
    env -u EXTC_RECLAIM EXTC_RC_SHARDS=4 "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/digraph_extc_shards4.c" src/digraph.extc \
      && $CC -O2 -o "$B/digraph_extc_shards4" "$B/digraph_extc_shards4.c"
    env -u EXTC_RECLAIM -u EXTC_RC_SHARDS EXTC_RC_NOCOUNT=0 "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/digraph_extc_count.c" src/digraph.extc \
      && $CC -O2 -o "$B/digraph_extc_count" "$B/digraph_extc_count.c"
    $CXX -O2 -std=c++17 -DDIGRAPH_NAIVE   src/digraph.cpp -o "$B/digraph_cpp_naive"
    $CXX -O2 -std=c++17 -DDIGRAPH_CAREFUL src/digraph.cpp -o "$B/digraph_cpp_careful"
    $RUSTC -O --edition 2021               src/digraph.rs -o "$B/digraph_rs_naive"
    $RUSTC -O --edition 2021 --cfg careful src/digraph.rs -o "$B/digraph_rs_careful"
    $GO build -o "$B/digraph_go" src/digraph.go
    $CC -O2 -o "$B/digraph_run" digraph_run.c
    # JIT 家族（task-23）：Java 需 javac；Node 是解释执行、零依赖（不用 npm）
    $JAVAC -d "$B/classes" src/digraph.java
    "$NODE" --check src/digraph.js
    if [ "$MODE" = "pause" ]; then
        # 停顿分布用的**诊断副本**（不是 10 臂之一）：把探测脚本插到 pin 住的生成物上。
        for spec in "digraph_extc:" "digraph_extc_count:EXTC_RC_NOCOUNT=0" "digraph_extc_shards4:EXTC_RC_SHARDS=4"; do
            n=${spec%%:*}; e=${spec#*:}
            env -u EXTC_RECLAIM $e "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/$n.dist.c" src/digraph.extc \
              && python3 "$PROBE" "$B/$n.dist.c" > "$B/$n.probe.c" \
              && $CC -O2 -D_POSIX_C_SOURCE=200809L -o "$B/$n.probe" "$B/$n.probe.c"
        done
        # EXTC_RECLAIM=0 不发射回收运行期 ⇒ 探测脚本会拒插桩（这是**结构性 0**，不是缺数据）。
        grep -c "extc_rc_collect" "$B/digraph_extc_noreclaim.c" > "$B/noreclaim_collect_callsites.txt" || true
    fi
    set +x
    echo
fi

# ---------- 行定义：label|env 前缀|命令 ----------
row_defs() {
    printf '%s\n' \
      "extC default|env|$B/digraph_extc" \
      "Java|env|$JAVA -cp $B/classes digraph" \
      "Node bigint|env DIGRAPH_JS_MODE=bigint|$NODE src/digraph.js" \
      "Node split|env DIGRAPH_JS_MODE=split|$NODE src/digraph.js" \
      "extC RECLAIM=0|env|$B/digraph_extc_noreclaim" \
      "extC 计数开|env|$B/digraph_extc_count" \
      "extC RC_SHARDS=4|env|$B/digraph_extc_shards4" \
      "C++ naive|env|$B/digraph_cpp_naive" \
      "C++ careful|env|$B/digraph_cpp_careful" \
      "Rust naive|env|$B/digraph_rs_naive" \
      "Rust careful|env|$B/digraph_rs_careful" \
      "Go GOGC default|env -u GOGC|$B/digraph_go" \
      "Go GOGC=off|env GOGC=off|$B/digraph_go"
}

# output_problem <file> -> none | missing | lines | fmt
output_problem() {
    local f="$1"
    [ -f "$f" ] || { echo missing; return; }
    [ "$(wc -l < "$f")" -eq 2 ] || { echo lines; return; }
    head -1 "$f" | grep -qE '^nodes=[0-9]+ edges=[0-9]+ sum=[0-9]+$' || { echo fmt; return; }
    tail -1 "$f" | grep -qE '^backedges=[0-9]+ groups_made=[0-9]+ nodes_made=[0-9]+ dropped_groups=[0-9]+ dropped_nodes=[0-9]+ rejected_external=[0-9]+ cycles_dropped=[0-9]+$' || { echo fmt; return; }
    echo none
}

# bench_one <label> <envpfx> <cmd> <N>：REPS 次采样；不合格样本**分因**丢弃
bench_one() {
    local label="$1" envpfx="$2" cmd="$3" N="$4"
    local cmdline="$envpfx $cmd $N $SEED"
    local best_ms="" max_kb=0 valid=0 d_rc=0 d_lines=0 d_fmt=0
    local samples="" _r outf res ms kb rc prob
    for _r in $(seq 1 "$REPS"); do
        outf=$(mktemp /tmp/gcbench.out.XXXXXX)
        # shellcheck disable=SC2086
        res=$($envpfx ./"$B"/digraph_run --pin "$PIN" --out "$outf" -- $cmd "$N" "$SEED" 2>/dev/null)
        ms=$(printf '%s\n' "$res" | sed -n 's/^ms=\([0-9.]*\) .*$/\1/p')
        kb=$(printf '%s\n' "$res" | sed -n 's/^ms=[0-9.]* rss_kb=\([0-9]*\) .*$/\1/p')
        rc=$(printf '%s\n' "$res" | sed -n 's/^ms=.* rc=\([-0-9]*\)$/\1/p')
        prob=$(output_problem "$outf")
        if [ -z "$res" ] || [ -z "$rc" ]; then
            d_rc=$((d_rc + 1)); samples="$samples [垫片无输出:丢弃]"; rm -f "$outf"; continue
        fi
        if [ "$rc" != "0" ]; then
            d_rc=$((d_rc + 1)); samples="$samples [rc=$rc:丢弃]"; rm -f "$outf"; continue
        fi
        if [ "$prob" != "none" ]; then
            case "$prob" in
                lines) d_lines=$((d_lines + 1)) ;;
                fmt)   d_fmt=$((d_fmt + 1)) ;;
                *)     d_lines=$((d_lines + 1)) ;;
            esac
            samples="$samples [$prob:丢弃]"; rm -f "$outf"; continue
        fi
        valid=$((valid + 1))
        samples="$samples ${ms}ms/${kb}KB"
        if [ -z "$best_ms" ] || awk -v a="$ms" -v b="$best_ms" 'BEGIN{exit !(a<b)}'; then best_ms=$ms; fi
        if [ "$kb" -gt "$max_kb" ]; then max_kb=$kb; fi
        rm -f "$outf"
    done
    if [ "$valid" -eq 0 ]; then
        printf '  %-18s ERROR：%d/%d 样本全部丢弃 [rc=%d lines=%d fmt=%d]\n' \
               "$label" "$((d_rc + d_lines + d_fmt))" "$REPS" "$d_rc" "$d_lines" "$d_fmt"
        printf '      cmd: %s\n' "$cmdline"
        printf '%s\tALL-DISCARD\trc=%d lines=%d fmt=%d\n' "$cmdline" "$d_rc" "$d_lines" "$d_fmt" >> "$RAW"
    else
        local mib
        mib=$(awk -v k="$max_kb" 'BEGIN{printf "%.1f", k/1024}')
        printf '  %-18s %8s ms  %8s MiB   [有效 %d/%d，丢弃 rc=%d lines=%d fmt=%d]\n' \
               "$label" "$best_ms" "$mib" "$valid" "$REPS" "$d_rc" "$d_lines" "$d_fmt"
        printf '      cmd: %s\n' "$cmdline"
        printf '%s\t样本:%s\t丢弃 rc=%d lines=%d fmt=%d\n' \
               "$cmdline" "$samples" "$d_rc" "$d_lines" "$d_fmt" >> "$RAW"
    fi
}

# ---------- jit：JIT 家族的冷/暖 + 启动 + GC/JIT 采集（task-23） ----------
# 锚定前缀（**禁松规则**）：诊断轮剥 trace 用它；未匹配行必须为 0。
JAVA_ANCHORS='^\[[0-9.]+s\]\[(info|warning|error)\]\[(gc|jit)'
# ⚠️ 锚定集是**实测枚举**出来的：已抓过两次漏网（`[completed optimizing`、间歇出现的 `[aborted compiling]`）
# ⇒ 每次诊断轮都必须过"未匹配行=0"这条自检（见 strip_check），新前缀出现时在这里补。
V8_ANCHORS='^\[(completed |aborted |failed |)compiling |^\[marking |^\[completed optimizing |^\[bailout |^\[deoptimiz|^\[removing optimized code |^\[evicting |^\[found optimized code |^\[[0-9a-fx]+:[0-9a-fx]+\] +[0-9]+ ms: '
PROD1='^nodes=[0-9]+ edges=[0-9]+ sum=[0-9]+$'
PROD2='^backedges=[0-9]+ groups_made='

jit_rows() {
    printf '%s\n' \
      "Java|env|$JAVA -cp $B/classes digraph" \
      "Node bigint|env DIGRAPH_JS_MODE=bigint|$NODE src/digraph.js" \
      "Node split|env DIGRAPH_JS_MODE=split|$NODE src/digraph.js"
}

# strip_check <family> <raw-out> <stripped-out> <reference-out> -> "ok" 或 FAIL 原因
strip_check() {
    local fam="$1" raw="$2" st="$3" ref="$4"
    local anchors="$V8_ANCHORS"
    [ "$fam" = "java" ] && anchors="$JAVA_ANCHORS"
    local total matched
    total=$(wc -l < "$raw")
    matched=$(grep -cE "$anchors|$PROD1|$PROD2" "$raw" || true)
    if [ "$total" != "$matched" ]; then
        local sample
        sample=$(grep -vE "$anchors|$PROD1|$PROD2" "$raw" | head -3 | tr '\n' '|' | cut -c1-220)
        echo "未匹配行=$((total - matched)) ≠ 0（有没识别的 trace 前缀）：$sample"; return 1
    fi
    grep -vE "$anchors" "$raw" > "$st"
    if [ "$(wc -l < "$st")" != "2" ] || ! grep -qE "$PROD1" "$st" || ! grep -qE "$PROD2" "$st"; then
        echo "剥完不是恰好 2 行产物"; return 1
    fi
    if ! cmp -s "$st" "$ref"; then
        echo "剥完与普通轮 stdout 不一致"; return 1
    fi
    echo "ok"
}

jit_block() {
    local N="$1"
    local reps="${JIT_REPS:-$REPS}"
    echo "-- JIT 家族（N=$N seed=$SEED, core=$PIN, REPS=$reps, warm k=${WARM_K:-3}）--"
    local k="${WARM_K:-3}"
    while IFS='|' read -r label envpfx cmd; do
        [ -n "${label:-}" ] || continue
        local cmdline="$envpfx $cmd $N $SEED"
        local outf errf res ms kb rc
        # ---- 冷（WARM=0，新进程）----
        local coldbest="" coldrss=0 coldsha="" coldok=0 coldfail=0
        local i
        for i in $(seq 1 "$reps"); do
            outf=$(mktemp /tmp/gcbench.jit.out.XXXXXX); errf=$(mktemp /tmp/gcbench.jit.err.XXXXXX)
            # shellcheck disable=SC2086
            res=$($envpfx ./"$B"/digraph_run --pin "$PIN" --out "$outf" --err "$errf" -- $cmd "$N" "$SEED" 2>/dev/null)
            ms=$(printf '%s\n' "$res" | sed -n 's/^ms=\([0-9.]*\).*/\1/p')
            kb=$(printf '%s\n' "$res" | sed -n 's/^ms=[0-9.]* rss_kb=\([0-9]*\).*/\1/p')
            rc=$(printf '%s\n' "$res" | sed -n 's/^ms=.* rc=\([-0-9]*\)$/\1/p')
            if [ "$rc" != "0" ] || [ "$(wc -l < "$outf")" != "2" ]; then coldfail=$((coldfail+1)); rm -f "$outf" "$errf"; continue; fi
            coldok=$((coldok+1)); [ -z "$coldsha" ] && coldsha=$(sha256sum "$outf" | cut -c1-16)
            if [ -z "$coldbest" ] || awk -v a="$ms" -v b="$coldbest" 'BEGIN{exit !(a<b)}'; then coldbest="$ms"; fi
            [ "$kb" -gt "$coldrss" ] && coldrss=$kb
            printf 'JIT\t%s\tN=%s\tCOLD\tms=%s\trss_kb=%s\tsha=%s\n' "$label" "$N" "$ms" "$kb" "$coldsha" >> "$RAW"
            rm -f "$outf" "$errf"
        done
        # ---- 暖（WARM=k，取末遍；同时记整进程）----
        local warmbest="" warmrss=0 warmlast="" warmproc="" warmfail=0 warmseq="" steady_ok=0 warmok=0
        for i in $(seq 1 "$reps"); do
            outf=$(mktemp /tmp/gcbench.jit.out.XXXXXX); errf=$(mktemp /tmp/gcbench.jit.err.XXXXXX)
            # shellcheck disable=SC2086
            res=$($envpfx env WARM=$k WARM_STATS=1 ./"$B"/digraph_run --pin "$PIN" --out "$outf" --err "$errf" -- $cmd "$N" "$SEED" 2>/dev/null)
            ms=$(printf '%s\n' "$res" | sed -n 's/^ms=\([0-9.]*\).*/\1/p')
            kb=$(printf '%s\n' "$res" | sed -n 's/^ms=[0-9.]* rss_kb=\([0-9]*\).*/\1/p')
            local seq last prev
            seq=$(sed -n 's/.*iters_ms=\(.*\)$/\1/p' "$errf" | head -1)
            last=$(printf '%s' "$seq" | awk -F, '{print $NF}')
            prev=$(printf '%s' "$seq" | awk -F, '{print $(NF-1)}')
            if [ -z "$last" ] || [ "$(wc -l < "$outf")" != "2" ]; then warmfail=$((warmfail+1)); rm -f "$outf" "$errf"; continue; fi
            warmok=$((warmok+1))
            warmseq="$seq"
            if [ -z "$warmlast" ] || awk -v a="$last" -v b="$warmlast" 'BEGIN{exit !(a<b)}'; then warmlast="$last"; fi
            if awk -v a="$last" -v b="$prev" 'BEGIN{ if (b<=0) exit 1; d=(a-b)/b; if (d<0) d=-d; exit !(d<=0.05) }'; then
                steady_ok=$((steady_ok+1))
            fi
            if [ -z "$warmproc" ] || awk -v a="$ms" -v b="$warmproc" 'BEGIN{exit !(a<b)}'; then warmproc="$ms"; fi
            [ "$kb" -gt "$warmrss" ] && warmrss=$kb
            printf 'JIT\t%s\tN=%s\tWARM\tproc_ms=%s\tlast_ms=%s\tprev_ms=%s\trss_kb=%s\tseq=%s\n' "$label" "$N" "$ms" "$last" "$prev" "$kb" "$seq" >> "$RAW"
            rm -f "$outf" "$errf"
        done
        local steady
        if [ "$warmok" -gt 0 ] && [ "$steady_ok" -eq "$warmok" ]; then steady="稳态($steady_ok/$warmok)"
        else steady="未达稳态($steady_ok/$warmok)"; fi
        # ---- 诊断轮（GC/JIT 分开）----
        local diag=""
        if [ "$label" = "Java" ]; then
            outf=$(mktemp /tmp/gcbench.jit.out.XXXXXX); errf=$(mktemp /tmp/gcbench.jit.err.XXXXXX)
            $JAVA -Xlog:gc:stderr -Xlog:jit+compilation:stderr -cp "$B/classes" digraph "$N" "$SEED" >"$outf" 2>"$errf"
            local gcj jitj
            gcj=$(python3 jit_stats.py java-gc "$errf" 2>&1); jitj=$(python3 jit_stats.py java-jit "$errf" 2>&1)
            diag="java: $(printf '%s' "$gcj" | head -1) | $(printf '%s' "$jitj" | head -1)"
            printf 'JIT\t%s\tN=%s\tDIAG\t%s\t%s\n' "$label" "$N" "$gcj" "$jitj" >> "$RAW"
            rm -f "$outf" "$errf"
        else
            local refout refsha st v8gc v8opt
            refout=$(mktemp /tmp/gcbench.jit.ref.XXXXXX)
            # shellcheck disable=SC2086
            $envpfx $cmd "$N" "$SEED" > "$refout" 2>/dev/null
            refsha=$(sha256sum "$refout" | cut -c1-16)
            for pair in "gc:--trace-gc" "opt:--trace-opt --trace-deopt"; do
                local tag="${pair%%:*}" flags="${pair#*:}"
                outf=$(mktemp /tmp/gcbench.jit.out.XXXXXX); errf=$(mktemp /tmp/gcbench.jit.err.XXXXXX)
                st=$(mktemp /tmp/gcbench.jit.st.XXXXXX)
                # shellcheck disable=SC2086
                $envpfx $NODE $flags src/digraph.js "$N" "$SEED" >"$outf" 2>"$errf"
                local chk
                chk=$(strip_check node "$outf" "$st" "$refout")
                if [ "$tag" = "gc" ]; then v8gc=$(python3 jit_stats.py node-gc "$outf" 2>&1); else v8opt=$(python3 jit_stats.py node-opt "$outf" 2>&1); fi
                printf 'JIT\t%s\tN=%s\tDIAG-%s\tstrip=%s\tref_sha=%s\t%s\n' "$label" "$N" "$tag" "$chk" "$refsha" "$([ "$tag" = gc ] && printf '%s' "$v8gc" | head -1 || printf '%s' "$v8opt" | head -1)" >> "$RAW"
                rm -f "$outf" "$errf" "$st"
            done
            diag="node: $(printf '%s' "$v8gc" | head -1) | $(printf '%s' "$v8opt" | head -1)"
            rm -f "$refout"
        fi
        local stmin="-" coldnet="-" warmnet="-"
        case "$label" in Java) stmin="$START_JAVA_MIN";; *) stmin="$START_NODE_MIN";; esac
        if [ -n "$coldbest" ] && [ -n "$stmin" ]; then
            coldnet=$(awk -v c="$coldbest" -v s="$stmin" 'BEGIN{printf "%.1f", c-s}')
        fi
        [ -n "$warmlast" ] && warmnet="$warmlast"
        printf '  %-13s 冷含启动=%-9s 冷扣启动=%-9s 暖含启动=%-9s 暖扣启动=%-9s 稳态=%-16s RSS冷/暖=%s/%sMiB (启动min=%s)\n' \
               "$label" "${coldbest:-FAIL}" "${coldnet}" "${warmproc:-FAIL}" "$warmnet" "$steady" "$((coldrss/1024))" "$((warmrss/1024))" "$stmin"
        printf '      cold cmd: %s\n      warm cmd: %s WARM=%s WARM_STATS=1\n      diag    : %s\n' \
               "$cmdline" "$cmdline" "$k" "$diag"
        printf '      cold sha=%s (有效 %d/%d)  warm 有效 %d/%d\n' "$coldsha" "$coldok" "$reps" "$((reps-warmfail))" "$reps"
    done < <(jit_rows)
}

# 启动探针（空程序，min/中位）
startup_block() {
    local reps="${START_REPS:-5}"
    echo "-- 启动探针（空程序，core=$PIN，$reps 次）--"
    local vals="" i t
    for i in $(seq 1 "$reps"); do
        t=$(./"$B"/digraph_run --pin "$PIN" --out /dev/null -- $JAVA -cp "$B/classes" empty | sed -n 's/^ms=\([0-9.]*\).*/\1/p')
        vals="$vals $t"
    done
    START_JAVA_VALS="$vals"
    START_JAVA_MIN=$(printf '%s\n' $vals | sort -n | head -1)
    echo "  Java 空程序: 序列=$vals  min=$START_JAVA_MIN ms  中位=$(printf '%s\n' $vals | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}') ms"
    vals=""
    for i in $(seq 1 "$reps"); do
        t=$(./"$B"/digraph_run --pin "$PIN" --out /dev/null -- $NODE src/empty.js | sed -n 's/^ms=\([0-9.]*\).*/\1/p')
        vals="$vals $t"
    done
    START_NODE_VALS="$vals"
    START_NODE_MIN=$(printf '%s\n' $vals | sort -n | head -1)
    echo "  Node 空脚本: 序列=$vals  min=$START_NODE_MIN ms  中位=$(printf '%s\n' $vals | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}') ms"
}

# ---------- pause：停顿分布（每臂都必须产出**可解析的统计行**，否则 FAIL） ----------
# family: extc(probe) / go(gctrace+memstats) / break(careful 断开那一趟) / none(无 STW 机制) / zero(结构性 0)
pause_rows() {
    printf '%s\n' \
      "extC default|env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1|$B/digraph_extc.probe|extc" \
      "extC RECLAIM=0|env|$B/digraph_extc_noreclaim|zero" \
      "extC 计数开|env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1|$B/digraph_extc_count.probe|extc" \
      "extC RC_SHARDS=4|env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1|$B/digraph_extc_shards4.probe|extc" \
      "C++ naive|env|$B/digraph_cpp_naive|none" \
      "C++ careful|env DIGRAPH_BREAK_STATS=1|$B/digraph_cpp_careful|break" \
      "Rust naive|env|$B/digraph_rs_naive|none" \
      "Rust careful|env DIGRAPH_BREAK_STATS=1|$B/digraph_rs_careful|break" \
      "Go GOGC default|env -u GOGC GODEBUG=gctrace=1 DIGRAPH_GO_STATS=1|$B/digraph_go|go" \
      "Go GOGC=off|env GOGC=off GODEBUG=gctrace=1 DIGRAPH_GO_STATS=1|$B/digraph_go|go"
}

# 从 stderr 里抽一行 PAUSE_DIST / GOSTATS / BREAK_DIST 的字段
field() { sed -n "s/.*$2=\([^ ]*\).*/\1/p" "$1" | head -1; }

pause_block() {
    local N="$1"
    echo "-- 停顿分布（N=$N seed=$SEED, taskset cpu=$PIN, best/max of $REPS）--"
    while IFS='|' read -r label envpfx cmd fam; do
        [ -n "${label:-}" ] || continue
        local cmdline="$envpfx $cmd $N $SEED"
        local ok=0 fail=0
        local best_max="" best_tot="" best_share="" best_n="" best_raw="" best_wall=""
        local i outf errf res wall
        for i in $(seq 1 "$REPS"); do
            outf=$(mktemp /tmp/gcbench.pout.XXXXXX); errf=$(mktemp /tmp/gcbench.perr.XXXXXX)
            # shellcheck disable=SC2086
            res=$($envpfx ./"$B"/digraph_run --pin "$PIN" --out "$outf" --err "$errf" -- $cmd "$N" "$SEED" 2>/dev/null)
            wall=$(printf '%s\n' "$res" | sed -n 's/^ms=\([0-9.]*\).*/\1/p')
            local pn="" pmax="" ptot="" pshare="" praw="" okline=1
            case "$fam" in
            extc)
                if grep -q "^PAUSE_DIST " "$errf"; then
                    praw=$(grep -m1 "^PAUSE_DIST " "$errf")
                    pn=$(printf '%s\n' "$praw" | sed -n 's/.* n=\([0-9]*\) overflow=.*/\1/p')
                    pmax=$(printf '%s\n' "$praw" | sed -n 's/.* max_us=\([0-9.]*\).*/\1/p')
                    local tus
                    tus=$(printf '%s\n' "$praw" | sed -n 's/.* total_us=\([0-9.]*\).*/\1/p')
                    ptot=$(awk -v u="$tus" 'BEGIN{printf "%.3f", u/1000.0}')
                    pshare=$(awk -v u="$tus" -v w="$wall" 'BEGIN{if(w>0)printf "%.5f", (u/1000.0)/w; else print "0"}')
                else
                    okline=0
                fi
                ;;
            go)
                if grep -q "^GOSTATS " "$errf" && python3 pause_stats.py gctrace "$errf" > "$errf.go" 2>/dev/null; then
                    local gline xline
                    gline=$(grep -m1 "^GO_DIST " "$errf.go")
                    xline=$(grep -m1 "^GO_XCHECK " "$errf.go")
                    praw="$gline | $xline"
                    pn=$(printf '%s\n' "$gline" | sed -n 's/.* n=\([0-9]*\) .*/\1/p')
                    pmax=$(printf '%s\n' "$gline" | sed -n 's/.* max_us=\([0-9.]*\).*/\1/p')
                    ptot=$(printf '%s\n' "$gline" | sed -n 's/.* total_ms=\([0-9.]*\).*/\1/p')
                    pshare=$(printf '%s\n' "$xline" | sed -n 's/.* memstats_share=\([0-9.]*\).*/\1/p')
                else
                    okline=0
                fi
                ;;
            break)
                if grep -q "^BREAK_DIST " "$errf"; then
                    praw=$(grep -m1 "^BREAK_DIST " "$errf")
                    pn=$(printf '%s\n' "$praw" | sed -n 's/.* n=\([0-9]*\) overflow=.*/\1/p')
                    pmax=$(printf '%s\n' "$praw" | sed -n 's/.* max_ns=\([0-9.]*\).*/\1/p')
                    ptot=$(printf '%s\n' "$praw" | sed -n 's/.* total_ms=\([0-9.]*\).*/\1/p')
                    pshare=$(awk -v t="$ptot" -v w="$wall" 'BEGIN{if(w>0)printf "%.5f", t/w; else print "0"}')
                    praw="$praw  ⚠非 STW（这是 careful 的显式断开那一趟）"
                else
                    okline=0
                fi
                ;;
            zero)
                pn=0; praw="结构性 0：不发射回收运行期（生成物里 extc_rc_collect 出现 $(cat "$B/noreclaim_collect_callsites.txt" 2>/dev/null || echo '?') 次 ⇒ 0 次清扫）"
                ;;
            none)
                pn=0; praw="n/a：无 STW 机制（引用计数只做计数归零，不扫描）"
                ;;
            esac
            if [ "$okline" = "0" ]; then
                fail=$((fail+1))
                printf '%s\tN=%s\tMISSING\t%s\n' "$label" "$N" "$cmdline" >> "$RAW"
            else
                ok=$((ok+1))
                # 聚合：max 取 max-of-N（数值比较交给 awk）
                if [ -z "$best_max" ] || awk -v a="$pmax" -v b="$best_max" 'BEGIN{exit !(a>b)}'; then
                    best_max="$pmax"; best_raw="$praw"; best_wall="$wall"
                fi
                if [ -z "$best_tot" ] || awk -v a="$ptot" -v b="$best_tot" 'BEGIN{exit !(a>b)}'; then best_tot="$ptot"; fi
                if [ -z "$best_share" ] || awk -v a="$pshare" -v b="$best_share" 'BEGIN{exit !(a>b)}'; then best_share="$pshare"; fi
                if [ -z "$best_n" ] || awk -v a="$pn" -v b="$best_n" 'BEGIN{exit !(a>b)}'; then best_n="$pn"; fi
                printf '%s\tN=%s\twall_ms=%s\tn=%s\tmax=%s\ttotal_ms=%s\tshare=%s\t%s\n' \
                       "$label" "$N" "$wall" "$pn" "$pmax" "$ptot" "$pshare" "$praw" >> "$RAW"
            fi
            # 产物 stdout：程序自己的两行必须在（probe 副本会多一行 PROBE，允许）
            if ! grep -qE '^nodes=[0-9]+ edges=[0-9]+ sum=[0-9]+$' "$outf" || \
               ! grep -qE '^backedges=[0-9]+ groups_made=' "$outf"; then
                echo "  ✗ $label 产物 stdout 异常 ⇒ FAIL"; fail=$((fail+1))
            fi
            rm -f "$outf" "$errf" "$errf.go"
        done
        if [ "$fail" -gt 0 ]; then
            printf '  %-18s FAIL：%d/%d 个样本缺停顿统计行（或 stdout 异常）\n' "$label" "$fail" "$REPS"
        else
            local unit="µs"
            case "$fam" in break) unit="ns";; esac
            printf '  %-18s n=%-7s max=%-10s%s total_ms=%-9s 占墙钟=%-9s  [%d/%d]\n' \
                   "$label" "$best_n" "$best_max" "$unit" "$best_tot" "$best_share" "$ok" "$REPS"
            printf '      cmd: %s\n' "$cmdline"
            printf '      raw: %s\n' "$(printf '%s' "$best_raw" | cut -c1-160)"
        fi
    done < <(pause_rows)
}

# ---------- selftest：丢弃分类校准（喂坏生产者，不测真形状） ----------
if [ "$MODE" = "selftest" ]; then
    if [ ! -x "$B/digraph_run" ]; then
        $CC -O2 -o "$B/digraph_run" digraph_run.c
    # JIT 家族（task-23）：Java 需 javac；Node 是解释执行、零依赖（不用 npm）
    $JAVAC -d "$B/classes" src/digraph.java
    "$NODE" --check src/digraph.js
    fi
    if [ ! -x "$B/digraph_extc" ]; then
        env -u EXTC_RECLAIM -u EXTC_RC_SHARDS "$EXTC_BIN" -O2 -I "$STDLIB" -w -o "$B/digraph_extc.c" src/digraph.extc \
          && $CC -O2 -o "$B/digraph_extc" "$B/digraph_extc.c"
    fi
    cat > "$B/bad_rc.sh" <<'EOS'
#!/bin/sh
exit 3
EOS
    cat > "$B/bad_lines.sh" <<'EOS'
#!/bin/sh
printf 'nodes=1 edges=2 sum=3\n'
EOS
    cat > "$B/bad_fmt.sh" <<'EOS'
#!/bin/sh
printf 'nodes=1 edges=2\nbackedges=3\n'
EOS
    chmod +x "$B"/bad_rc.sh "$B"/bad_lines.sh "$B"/bad_fmt.sh
    : > "$RAW"
    REPS=${REPS:-3}
    echo "== selftest：丢弃分类校准（REPS=$REPS, PIN=$PIN）=="
    echo "   期望：好样本 valid=REPS；坏 rc → 丢弃 rc=REPS；坏 1 行 → lines=REPS；格式错 → fmt=REPS"
    bench_one "好样本 extC"      env "$B/digraph_extc"  2000
    bench_one "坏 退出码 3"      env "$B/bad_rc.sh"     2000
    bench_one "坏 只有 1 行"     env "$B/bad_lines.sh"  2000
    bench_one "坏 2 行格式错"    env "$B/bad_fmt.sh"    2000
    echo
    exit 0
fi

: > "$RAW"

for N in $SIZES; do
    echo "=================================================================="
    echo "== N=$N seed=$SEED  (sched_setaffinity cpu=$PIN, best of $REPS) =="
    echo "=================================================================="

    # ---- 1) 对齐检查：每个实现跑一次，比对逐字节 ----
    echo "-- 程序输出（每行原文）--"
    ref_out=""
    ref_label=""
    align_fail=0
    while IFS='|' read -r label envpfx cmd; do
        [ -n "${label:-}" ] || continue
        outf=$(mktemp /tmp/gcbench.out.XXXXXX)
        # shellcheck disable=SC2086
        $envpfx $cmd "$N" "$SEED" > "$outf" 2>/dev/null
        rc=$?
        prob=$(output_problem "$outf")
        if [ "$rc" != "0" ] || [ "$prob" != "none" ]; then
            printf '  %-18s rc=%s problem=%s  <无输出可比>\n' "$label" "$rc" "$prob"
            align_fail=1
            rm -f "$outf"
            continue
        fi
        printf '  %-18s %s\n' "$label" "$(head -1 "$outf")"
        printf '  %-18s %s\n' "" "$(tail -1 "$outf")"
        if [ -z "$ref_out" ]; then
            ref_out="$outf"; ref_label="$label"
            continue
        fi
        if ! diff -q "$ref_out" "$outf" >/dev/null; then
            echo "  ✗ 与 $ref_label 不一致："
            diff "$ref_out" "$outf" | sed 's/^/      /'
            align_fail=1
        fi
        rm -f "$outf"
    done < <(row_defs)

    if [ "$align_fail" != "0" ]; then
        echo "  ✗ 对齐失败 ⇒ **本 N 不测时间**（数字不算数）"
        [ -n "$ref_out" ] && rm -f "$ref_out"
        echo
        continue
    fi
    echo "  ✓ 全部实现逐字节一致（每格产物 = 2 行）"
    [ -n "$ref_out" ] && rm -f "$ref_out"

    [ "$MODE" = "align" ] && { echo; continue; }
    [ "$MODE" = "pause" ] && { pause_block "$N"; continue; }
    [ "$MODE" = "jit" ] && {
        if [ "$N" = "$(printf '%s\n' $SIZES | head -1)" ]; then startup_block; fi
        jit_block "$N"; continue
    }

    # ---- 2) 计时 + 内核峰值 RSS ----
    echo "-- 时间（ms, best）/ 峰值 RSS（MiB, max）/ 有效样本 --"
    while IFS='|' read -r label envpfx cmd; do
        [ -n "${label:-}" ] || continue
        bench_one "$label" "$envpfx" "$cmd" "$N"
    done < <(row_defs)
    echo
done

if [ "$MODE" != "align" ]; then
    echo "原始样本（含每次 ms/rss_kb/程序输出/丢弃分因）：$RAW"
fi
