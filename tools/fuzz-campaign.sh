#!/usr/bin/env bash
# 跑一轮 fuzz 战役：三种模式 × 若干种子，每片有独立的时间上限。
#
# 为什么要这个脚本：2026-09-28 那次战役有**两片被 `timeout` 静默掐掉** —— 进程被杀、日志里
# 什么线索都没有，只有"开始的片数 18、有完成行的片数 16"这种差额能看出来。这里把每片的结果
# （ok / 失败 N 条 / **超时被杀**）都写进日志，并在结尾给一张汇总表。
#
# 用法：
#     tools/fuzz-campaign.sh                          # 默认 400 次/片，三模式 × 6 个种子
#     tools/fuzz-campaign.sh 600                      # 每片 600 次
#     tools/fuzz-campaign.sh 400 mutate gen 101 202   # 指定模式与种子
#
# 产物一律落 ~/extc-fuzz（**不要放 /tmp**：那里常常是 tmpfs，写满之后连 bash 都起不来，
# 见 docs/topics/HARDENING.md 第四节）；日志落 ~/extc-work/。
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

iters=${1:-400}
shift || true
if [ "$#" -ge 2 ]; then
    modes=("$1"); shift
    seeds=("$@")
else
    modes=(mutate gen modules)
    seeds=(101 202 303 404 505 606)
fi

out="${EXTC_FUZZ_OUT:-$HOME/extc-fuzz}"
log="${EXTC_FUZZ_LOG:-$HOME/extc-work/fuzz-$(date +%m%d-%H%M).log}"
slice_timeout=${EXTC_FUZZ_SLICE_TIMEOUT:-3000}
mkdir -p "$out" "$(dirname "$log")"

echo "战役：模式 ${modes[*]} · 种子 ${seeds[*]} · 每片 $iters 次 · 产物 $out" | tee "$log"
fail_slices=(); timeout_slices=(); ok_slices=0
for mode in "${modes[@]}"; do
    for sd in "${seeds[@]}"; do
        tag="$mode/$sd"
        echo "=== $tag ===" >> "$log"
        if timeout "$slice_timeout" python3 "$root/tools/fuzz.py" --mode "$mode" \
                --iters "$iters" --seed "$sd" --jobs 8 --out "$out" >> "$log" 2>&1; then
            n=$(grep -a '失败 [0-9]* 条' "$log" | tail -1 | sed 's/.*失败 \([0-9]*\) 条.*/\1/')
            if [ "${n:-0}" = 0 ]; then ok_slices=$((ok_slices + 1)); echo "  ok    $tag（0 失败）" | tee -a "$log"
            else fail_slices+=("$tag:$n"); echo "  失败  $tag（$n 条）" | tee -a "$log"; fi
        else
            rc=$?
            if [ "$rc" = 124 ]; then
                timeout_slices+=("$tag")
                echo "  **超时** $tag —— 被 ${slice_timeout}s 上限掐掉，这一片的结论无效" | tee -a "$log"
            else
                fail_slices+=("$tag:rc=$rc")
                echo "  错误  $tag（fuzzer 退出码 $rc）" | tee -a "$log"
            fi
        fi
    done
done

{
    echo "--- 汇总 ---"
    echo "  通过片数: $ok_slices"
    echo "  有失败的片: ${fail_slices[*]:-无}"
    echo "  被超时掐掉的片: ${timeout_slices[*]:-无}"
    echo "  分诊: python3 tools/fuzz-triage.py $out"
} | tee -a "$log"
[ "${#fail_slices[@]}" = 0 ] && [ "${#timeout_slices[@]}" = 0 ]
