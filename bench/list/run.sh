#!/usr/bin/env bash
# bench/list —— 链表写法的三种代价（手写 C，不涉及 extC 代码生成）
#
# e1 扫一遍按序号摘链：朴素 prev 跟踪 vs 两星指针（node **）
# e2 给定节点 O(1) 摘链：指针 hlist / 双向 list_head / 索引 hlist（i32 next + i32 pprev），随机顺序与顺序各一组
# e3 遍历一遍：同样三种布局
#
# 判据：① 同一形状各臂的 verdict（rem / chk / sum）必须逐字相同，否则是摘链逻辑写错；
#      ② 计时由程序内部跑 REPS 轮取最小值，ns 单位；
#      ③ 峰值 RSS 取 /usr/bin/time 的 ru_maxrss（一次即可，确定性）。
# 用法： bash bench/list/run.sh [--reps N] [--pin N]
set -u
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
REPS=${REPS:-7}
PIN=${PIN:-6}
OUTDIR=${OUTDIR:-bench/list/out}
while [ $# -gt 0 ]; do
    case $1 in
        --reps) REPS=$2; shift 2 ;;
        --pin)  PIN=$2; shift 2 ;;
        -h|--help) sed -n '2,11p' $0; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
B=${B:-$(mktemp -d)}
trap 'rm -rf "$B"' EXIT

$CC -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra \
    -o "$B/listops" bench/list/listops.c || { echo "FAIL 编不过" >&2; exit 1; }

rss=$( { /usr/bin/time -f '%M' taskset -c "$PIN" "$B/listops" "$REPS" >"$B/out.txt" 2>"$B/err.txt"; } 2>&1; tail -1 "$B/err.txt" )
if [ -z "$rss" ]; then rss=0; fi

mkdir -p "$OUTDIR"
printf 'shape\tarm\tns\trss_kb\tverdict\toutput\n' > "$OUTDIR/RESULTS.tsv"
fail=0
while IFS=$'\t' read -r shape arm ns rest; do
    case $shape in \#*|"") continue ;; esac
    printf '%s\t%s\t%s\t%s\tmin-of-%s\t%s\n' "$shape" "$arm" "$ns" "$rss" "$REPS" "$rest" >> "$OUTDIR/RESULTS.tsv"
    echo "  $shape $arm  ${ns} ns  $rest"
done < "$B/out.txt"

for s in $(awk -F'\t' '!/^#/ && NF>=4 {print $1}' "$B/out.txt" | sort -u); do
    ref=$(awk -F'\t' -v s="$s" '$1==s {print $4; exit}' "$B/out.txt")
    awk -F'\t' -v s="$s" -v ref="$ref" -v shape="$s" \
        '$1==shape && $4!=ref {printf "  FAIL %s: [%s] vs [%s]\n", shape, ref, $4; bad=1} END{exit bad?1:0}' \
        "$B/out.txt" || fail=1
done
echo
echo "结果写在 $OUTDIR/RESULTS.tsv（程序内 min-of-$REPS）"
[ "$fail" = 0 ] && echo "各臂结构类结果一致" || echo "有失败项"
exit $fail
