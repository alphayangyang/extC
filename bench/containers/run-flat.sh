#!/usr/bin/env bash
# bench/containers/run-flat.sh —— hashMap 的三方对照：extC vs C++ std::unordered_map vs C++ 手写开放寻址表
#
# 为什么要有第三个臂：libstdc++ 的 `unordered_map` 是链式节点表（每元素一次分配、查找沿指针追），
# 拿它当「C++ 的上界」会把 extC 的优势说大。这里用 `h_flat.cpp`（同一类算法：2 的幂容量、
# 线性探测、墓碑、0.5 装载率）当**同算法参考**，回答「extC 的差距是算法还是列布局」。
#
# 形状：h1..h5 = bench/containers/h1_cap..h5_miss.extc；h6 = 只插入（把 h1 拆成插入侧与查询侧），
# C++ 侧对应 `h_flat p1`。
# 判据：① 同一形状各臂输出逐字相同；② 交错多轮取最小值；③ 峰值 RSS 取 ru_maxrss。
# 用法：bash bench/containers/run-flat.sh [--reps N] [--pin N] [--tsv FILE]
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
REPS=${REPS:-7}
PIN=${PIN:-6}
OUTDIR=${OUTDIR:-bench/containers}
TSV=${TSV:-RESULTS-hash-flat.tsv}
while [ $# -gt 0 ]; do
    case $1 in
        --reps) REPS=$2; shift 2 ;;
        --pin)  PIN=$2; shift 2 ;;
        --tsv)  TSV=$2; shift 2 ;;
        -h|--help) sed -n '2,15p' $0; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
B=${B:-$(mktemp -d)}; trap 'rm -rf "$B"' EXIT

build() {   # build <name> <源>
    local n=$1 src=$2
    case $src in
        *.extc) $EXTC -w --no-line-map -o "$B/$n.c" "$src" >/dev/null 2>&1 || return 1
                $CC -O2 -fwrapv -std=c11 -o "$B/$n" "$B/$n.c" >/dev/null 2>&1 || return 1 ;;
        *.cpp)  $CXX -O2 -std=c++17 -o "$B/$n" "$src" >/dev/null 2>&1 || return 1 ;;
    esac
}
wall() { local a=$1; shift; local out; out=$( { TIMEFORMAT='%3R'; time taskset -c "$PIN" "$B/$a" "$@" >"$B/$a.out" 2>/dev/null; } 2>&1 ); printf '%s\n' "$out"; }
once() { local a=$1; shift; local st; st=$( { /usr/bin/time -f '%M' taskset -c "$PIN" "$B/$a" "$@" >"$B/$a.out" 2>"$B/$a.err"; } 2>&1; tail -1 "$B/$a.err" ); printf '%s|%s\n' "$st" "$(cat "$B/$a.out")"; }

# 每个形状：<形状名>|<extC 源>|<C++ 源>|<C++ 源参数>
rows="
h1|bench/containers/h1_cap.extc|bench/containers/h1_cap.cpp|h1
h2|bench/containers/h2_grow.extc|bench/containers/h2_grow.cpp|h2
h3|bench/containers/h3_churn.extc|bench/containers/h3_churn.cpp|h3
h4|bench/containers/h4_iter.extc|bench/containers/h4_iter.cpp|h4
h5|bench/containers/h5_miss.extc|bench/containers/h5_miss.cpp|h5
h6|bench/containers/h6_putonly.extc|bench/containers/h_flat.cpp|p1
"
printf 'shape\tarm\tsec\trss_kb\tverdict\toutput\n' > "$OUTDIR/$TSV"
fail=0
for row in $rows; do
    shape=$(echo "$row" | cut -d'|' -f1)
    esrc=$(echo "$row" | cut -d'|' -f2)
    csrc=$(echo "$row" | cut -d'|' -f3)
    carg=$(echo "$row" | cut -d'|' -f4)
    build "${shape}_extc" "$esrc" || { echo "FAIL ${shape}: extC 编不过"; fail=1; continue; }
    build "${shape}_cpp" "$csrc" || { echo "FAIL ${shape}: C++ 编不过"; fail=1; continue; }
    build "${shape}_flat" bench/containers/h_flat.cpp || { echo "FAIL ${shape}: h_flat 编不过"; fail=1; continue; }
    arms="${shape}_extc ${shape}_cpp ${shape}_flat"
    [ "$shape" = h6 ] && arms="${shape}_extc ${shape}_flat"
    for a in $arms; do eval "best_$a="; done
    for k in $(seq "$REPS"); do
        for a in $arms; do
            case $a in
                *_flat) if [ -n "$carg" ]; then ms=$(wall "$a" "$carg"); else ms=$(wall "$a"); fi ;;
                *)      ms=$(wall "$a") ;;
            esac
            eval "cur=\$best_$a"
            if [ -z "$cur" ] || awk "BEGIN{exit !($ms < $cur)}"; then eval "best_$a=$ms"; fi
        done
    done
    first=""
    for a in $arms; do
        case $a in
            *_flat) if [ -n "$carg" ]; then info=$(once "$a" "$carg"); else info=$(once "$a"); fi ;;
            *)      info=$(once "$a") ;;
        esac
        rss=${info%%|*} ; out=${info#*|}
        eval "best=\$best_$a"
        arm=$(echo "$a" | sed "s/^${shape}_//")
        printf '%s\t%s\t%s\t%s\tmin-of-%s\t%s\n' "$shape" "$arm" "$best" "$rss" "$REPS" "$out" >> "$OUTDIR/$TSV"
        [ -z "$first" ] && first=$out
        [ "$out" != "$first" ] && { echo "FAIL ${shape}: 输出不一致 ($out vs $first)"; fail=1; }
    done
    printf '%-4s' "$shape"; for a in $arms; do eval "v=\$best_$a"; printf ' %s=%ss' "$(echo "$a" | sed "s/^${shape}_//")" "$v"; done; echo
done
echo "结果写在 $OUTDIR/$TSV（交错 min-of-$REPS）"
exit $fail
