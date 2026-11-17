#!/usr/bin/env bash
# bench/containers —— 容器存储基质的 A/B：池板块（旧）vs buf<T> 预留（新）vs C++ std::vector
#
# 三臂编译**同一份 .extc 源**：
#   old = 池版本编译器（git worktree 检出 f910d14d 并自建），EXTC_STD 指向该树的 stdlib
#   new = 当前树的 ./build/extc
#   cpp = g++ -O2 -std=c++17
# 判据：① 三臂的输出逐字相同（等价工作）；② 交错多轮取最小值（毫秒）；③ 峰值 RSS 取 ru_maxrss。
# 用法：bash bench/containers/run.sh [--reps N] [--pin N] [--old DIR] [--shapes v1,v2,...]
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
REPS=${REPS:-5}
PIN=${PIN:-6}
OLD=${OLD:-/tmp/ct/wt-old}
SHAPES=${SHAPES:-v1,v2,v3,v4,s1,s2,s3}
OUTDIR=${OUTDIR:-bench/containers}
TSV=${TSV:-RESULTS.tsv}
while [ $# -gt 0 ]; do
    case $1 in
        --reps) REPS=$2; shift 2 ;;
        --pin)  PIN=$2; shift 2 ;;
        --old)  OLD=$2; shift 2 ;;
        --shapes) SHAPES=$2; shift 2 ;;
        --tsv)  TSV=$2; shift 2 ;;
        -h|--help) sed -n '2,10p' $0; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
if [ ! -x "$OLD/build/extc" ]; then
    echo "缺旧编译器：$OLD/build/extc（先 git worktree add -f $OLD f910d14d && make -C $OLD build/extc -j8）" >&2
    exit 2
fi
B=${B:-$(mktemp -d)}; trap 'rm -rf "$B"' EXIT

build_arm() {   # build_arm <extc源|.cpp> <old|new|cpp>
    local n=$1 arm=$2
    case $n in
        *.extc)
            case $arm in
                old) EXTC_STD="$OLD/stdlib" "$OLD/build/extc" -w --no-line-map -o "$B/$arm.c" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
                new) "$EXTC" -w --no-line-map -o "$B/$arm.c" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
            esac
            $CC -O2 -fwrapv -std=c11 -o "$B/$arm" "$B/$arm.c" >/dev/null 2>&1 || return 1 ;;
        *.cpp)
            $CXX -O2 -std=c++17 -o "$B/$arm" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
    esac
}

wall() { local a=$1 out; out=$( { TIMEFORMAT='%3R'; time taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>/dev/null; } 2>&1 ); printf '%s\n' "$out"; }
once() { local a=$1 st; st=$( { /usr/bin/time -f '%M' taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>"$B/$a.err"; } 2>&1; tail -1 "$B/$a.err" ); printf '%s %s\n' "$st" "$(cat "$B/$a.out")"; }

printf 'shape\tarm\tsec\trss_kb\tverdict\toutput\n' > "$OUTDIR/$TSV"
fail=0
for shape in $(echo "$SHAPES" | tr ',' ' '); do
    case $shape in
        v1) desc='8e6 个 i64：从容量 4 增长并求和' ; src=v1_grow.extc; csrc=v1_vec.cpp ;;
        v2) desc='2e6 装满后连做 5 次克隆' ; src=v2_clone.extc; csrc=v2_vec.cpp ;;
        v3) desc='8e6 装满 → clear → 再装满' ; src=v3_reuse.extc; csrc=v3_vec.cpp ;;
        v4) desc='20 万个小容器（容量 8）生死循环' ; src=v4_churn.extc; csrc=v4_vec.cpp ;;
        s1) desc='50 万次短串（15 字节）构造+读回，不该分配' ; src=s1_sso.extc; csrc=s1_sso.cpp ;;
        s2) desc='3 条 1 MiB 长串，64 字节块 append 16384 次' ; src=s2_growstr.extc; csrc=s2_growstr.cpp ;;
        s3) desc='128 KiB 串 clone 5 次（深拷贝）' ; src=s3_clone_str.extc; csrc=s3_clone_str.cpp ;;
        *) echo "unknown shape: $shape" >&2; exit 2 ;;
    esac
    echo "== $shape：$desc =="
    built=""
    for arm in old new cpp; do
        f=$src; [ $arm = cpp ] && f=$csrc
        if build_arm "$f" "$arm"; then built="$built $arm"; else echo "  FAIL $arm 编不过（$f）"; fail=1; fi
    done
    for a in $built; do
        eval "best_$a="
    done
    for k in $(seq "$REPS"); do
        for a in $built; do
            ms=$(wall "$a")
            eval "cur=\$best_$a"
            if [ -z "$cur" ] || awk "BEGIN{exit !($ms < $cur)}"; then eval "best_$a=$ms"; fi
        done
    done
    first=""; ref=""
    for a in $built; do
        info=$(once "$a"); rss=${info%% *}; out=${info#* }
        eval "best=\$best_$a"
        printf '%s\t%s\t%s\t%s\tmin-of-%s\t%s\n' "$shape" "$a" "$best" "$rss" "$REPS" "$out" >> "$OUTDIR/$TSV"
        echo "  $a  min=${best}s  rss=${rss}KB  $out"
        if [ -z "$first" ]; then first=$out; ref=$a; elif [ "$out" != "$first" ]; then echo "  FAIL 输出不一致：$ref=[$first] vs $a=[$out]"; fail=1; fi
    done
done
echo
echo "结果写在 $OUTDIR/$TSV（交错 min-of-$REPS）"
[ "$fail" = 0 ] && echo '三臂输出逐字一致' || echo '有失败项'
exit $fail