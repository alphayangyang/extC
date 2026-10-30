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

"$EXTC" -w --no-line-map -o "$tmp/f.c" "$dir/pipeline.extc"
"$EXTC" -w --no-line-map --no-fuse -o "$tmp/u.c" "$dir/pipeline.extc"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/f" "$tmp/f.c"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/u" "$tmp/u.c"
grep -q "slice_f64 tmpv" "$tmp/f.c" && { echo "FAIL: 融合臂的生成 C 里还有临时视图"; exit 1; }
! grep -q "slice_f64 tmpv" "$tmp/u.c" && { echo "FAIL: 未融合臂的临时视图不见了"; exit 1; }

echo "== 对拍：两臂 stdout 必须逐字节相同"
for n in $SIZES; do
    rf=$("$tmp/f" "$n" "$ITERS"); ru=$("$tmp/u" "$n" "$ITERS")
    [ "$rf" = "$ru" ] || { echo "FAIL: n=$n 输出 'f=$rf' vs 'u=$ru'"; exit 1; }
    echo "  n=$n  $rf"
done

med() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
echo "== 计时（RUNS=$RUNS，pin=$PIN，iters=$ITERS，中位数 ms）"
printf '%-10s %12s %12s %10s\n' n fused unfused ratio
for n in $SIZES; do
    f=(); u=()
    for r in $(seq "$RUNS"); do
        t0=$(date +%s%N); taskset -c "$PIN" "$tmp/f" "$n" "$ITERS" >/dev/null; t1=$(date +%s%N)
        f+=($(( (t1 - t0) / 1000 )))
        t0=$(date +%s%N); taskset -c "$PIN" "$tmp/u" "$n" "$ITERS" >/dev/null; t1=$(date +%s%N)
        u+=($(( (t1 - t0) / 1000 )))
    done
    fm=$(printf '%s\n' "${f[@]}" | med); um=$(printf '%s\n' "${u[@]}" | med)
    printf '%-10s %12s %12s %10s\n' "$n" "$((fm / 1000)).$(printf '%03d' $((fm % 1000)))" \
        "$((um / 1000)).$(printf '%03d' $((um % 1000)))" "$(python3 -c "print(f'{$fm/$um:.3f}')")"
done
