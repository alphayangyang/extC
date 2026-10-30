#!/usr/bin/env bash
# bench/fusion/run.sh —— 元素链融合（第 2 层 S1）基准：同一份源码编两次（默认融合 / `--no-fuse`），
# 逐字节对拍后计时，判据 = 融合后不慢于未融合（S1 的期望收益是少一整趟临时数组的读写）。
#
# 用法：bash bench/fusion/run.sh            # 默认 n=1048576（三份 8 MiB f64）：已越过本机 L3
#       SIZES="262144 1048576" RUNS=5 bash bench/fusion/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root"
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
RUNS=${RUNS:-5}
PIN=${PIN:-6}
SIZES=${SIZES:-"1048576"}
ITERS=${ITERS:-8}
dir=bench/fusion
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

"$EXTC" -w --no-line-map -o "$tmp/pf.c" "$dir/pipeline.extc"
"$EXTC" -w --no-line-map --no-fuse -o "$tmp/pu.c" "$dir/pipeline.extc"
"$EXTC" -w --no-line-map -o "$tmp/sf.c" "$dir/stream.extc"
"$EXTC" -w --no-line-map --no-fuse -o "$tmp/su.c" "$dir/stream.extc"
for arm in pf pu sf su; do
    "$CC" -O2 -std=c11 -fwrapv -o "$tmp/$arm" "$tmp/$arm.c" || { echo "FAIL: 编译 $arm 失败"; exit 1; }
done
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/ph" "$dir/pipeline.c" || exit 1
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/sh" "$dir/stream.c" || exit 1
grep -q "slice_f64 tmpv" "$tmp/pf.c" && { echo "FAIL: S1 融合臂的生成 C 里还有临时视图"; exit 1; }
! grep -q "slice_f64 tmpv" "$tmp/pu.c" && { echo "FAIL: S1 未融合臂的临时视图不见了"; exit 1; }
grep -q "slice_f64 tmpv\|slice_f64 tv2" "$tmp/sf.c" && { echo "FAIL: S3 融合臂的生成 C 里还有临时视图"; exit 1; }
! grep -q "slice_f64 tv2" "$tmp/su.c" && { echo "FAIL: S3 未融合臂的第二层临时视图不见了"; exit 1; }

echo "== 对拍：同一形状各臂 stdout 必须逐字节相同"
for n in $SIZES; do
    p=$( "$tmp/pu" "$n" "$ITERS"); [ "$("$tmp/pf" "$n" "$ITERS")" = "$p" ] || { echo "FAIL: S1 n=$n 融合/未融合输出不同"; exit 1; }
    [ "$("$tmp/ph" "$n" "$ITERS")" = "$p" ] || { echo "FAIL: S1 n=$n 手写融合输出不同"; exit 1; }
    s=$( "$tmp/su" "$n" "$ITERS"); [ "$("$tmp/sf" "$n" "$ITERS")" = "$s" ] || { echo "FAIL: S3 n=$n 融合/未融合输出不同"; exit 1; }
    [ "$("$tmp/sh" "$n" "$ITERS")" = "$s" ] || { echo "FAIL: S3 n=$n 手写融合输出不同"; exit 1; }
    echo "  n=$n  S1: $p  |  S3: $s"
done

med() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
run_med() {   # run_med <可执行> <n>
    local xs=() t0 t1
    for r in $(seq "$RUNS"); do
        t0=$(date +%s%N); taskset -c "$PIN" "$1" "$n" "$ITERS" >/dev/null; t1=$(date +%s%N)
        xs+=($(( (t1 - t0) / 1000 )))
    done
    printf '%s\n' "${xs[@]}" | med
}
show() {     # show <n> <形状> <融合 ms> <未融合 ms> <手写 ms>
    printf '%-8s %-8s %11s %11s %11s %12s %11s\n' "$1" "$2" \
        "$(( $3 / 1000 )).$(printf '%03d' $(( $3 % 1000 )))" \
        "$(( $4 / 1000 )).$(printf '%03d' $(( $4 % 1000 )))" \
        "$(( $5 / 1000 )).$(printf '%03d' $(( $5 % 1000 )))" \
        "$(python3 -c "print(f'{$3/$4:.3f}')")" "$(python3 -c "print(f'{$3/$5:.3f}')")"
}
echo "== 计时（RUNS=$RUNS，pin=$PIN，iters=$ITERS，中位数 ms）"
printf '%-8s %-8s %11s %11s %11s %12s %11s\n' n shape fused unfused hand fused/unfused fused/hand
for n in $SIZES; do
    show "$n" S1 "$(run_med "$tmp/pf" "$n")" "$(run_med "$tmp/pu" "$n")" "$(run_med "$tmp/ph" "$n")"
    show "$n" S3 "$(run_med "$tmp/sf" "$n")" "$(run_med "$tmp/su" "$n")" "$(run_med "$tmp/sh" "$n")"
done
