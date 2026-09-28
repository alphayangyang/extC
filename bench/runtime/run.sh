#!/usr/bin/env bash
# bench/runtime/run.sh —— 语言核心运行期 + 产物质量（含矩阵乘法归因）：一把量完。
#
# ⚠️ 加进仓库时**没有重跑**：RESULTS.md/raw.txt 里的数字是 2026-09-28 手工量的，这里保留脚本以便复核。
# 口径：单核绑一个 P 核（默认 2）· 每次 RUNS 次取最好 · extC 产物按契约旗子
#       `gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror`；对照是**同次序同开关**的手写 C / C++20。
# 用法：bench/runtime/run.sh          （RUNS=7）· CPU=3 RUNS=3 bench/runtime/run.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
DIR=bench/runtime
EXTC=${EXTC:-./build/extc}
RUNS=${RUNS:-7}
CPU=${CPU:-2}
OUT=${OUT:-$DIR/raw.txt}
[ -x "$EXTC" ] || { echo "找不到 extC 前端：$EXTC（先 make）" >&2; exit 1; }
EXTC=$(readlink -f "$EXTC")          # 下面要 cd 进临时目录 ⇒ 先钉成绝对路径
work=$(mktemp -d /tmp/bench_runtime.XXXXXX) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
cp "$DIR"/*.c "$DIR"/*.cpp "$DIR"/*.extc "$work"/ 2>/dev/null
cd "$work" || exit 1
best() { local b=999999999 s e t; for _ in $(seq 1 "$RUNS"); do s=$(date +%s%N)
    taskset -c "$CPU" "$@" >/dev/null 2>&1; e=$(date +%s%N); t=$(( (e-s)/1000 ))
    [ "$t" -lt "$b" ] && b=$t; done; echo "$b"; }
echo "== 构建 =="
gcc -O2 -o l l.c && gcc -O2 -o sm sm.c && g++ -O2 -std=c++20 -fcoroutines -o co co.cpp || exit 1
$EXTC -w --no-line-map -o co_ext.c co.extc && gcc -O2 -std=c11 -fwrapv -o co_ext co_ext.c || exit 1
$EXTC -w --no-line-map -o bck.c bounds_ck.extc && gcc -O2 -std=c11 -fwrapv -o bck bck.c || exit 1
$EXTC -w --no-line-map -o bun.c bounds_un.extc && gcc -O2 -std=c11 -fwrapv -o bun bun.c || exit 1
$EXTC -w --no-line-map -o af.c a_fresh.extc && gcc -O2 -std=c11 -fwrapv -o af af.c || exit 1
$EXTC -w --no-line-map -o ar.c a_reuse.extc && gcc -O2 -std=c11 -fwrapv -o ar ar.c || exit 1
gcc -O2 -o ac a_c.c && gcc -O2 -o ac2 a_c2.c && gcc -O2 -std=c11 -fwrapv -o mmf2 mm_fair.c || exit 1
gcc -O3 -march=native -o mmf3 mm_fair.c || exit 1
$EXTC -w --no-line-map -o mme.c matmul.extc && gcc -O2 -std=c11 -fwrapv -o mme mme.c || exit 1
$EXTC -w --no-line-map -o mmu.c matmul.extc && gcc -O3 -march=native -o mmu mmu.c || exit 1
$EXTC -w --no-line-map -o mmu2.c matmul_unchecked.extc && gcc -O2 -std=c11 -fwrapv -o mmu2 mmu2.c || exit 1
: > "$OUT"
echo "== 协程 resume（1e6 次）=="
echo "resume loop $(best ./l)"           >> "$OUT"
echo "resume statemachine $(best ./sm)"  >> "$OUT"
echo "resume cpp20 $(best ./co)"         >> "$OUT"
echo "resume extc $(best ./co_ext)"      >> "$OUT"
echo "== 边界检查 =="
echo "bounds checked $(best ./bck)"      >> "$OUT"
echo "bounds unchecked $(best ./bun)"    >> "$OUT"
echo "== arena =="
echo "arena extc_fresh $(best ./af)"     >> "$OUT"
echo "arena c_calloc $(best ./ac)"       >> "$OUT"
echo "arena c_malloc_memset $(best ./ac2)" >> "$OUT"
echo "arena extc_reuse $(best ./ar)"     >> "$OUT"
echo "arena c_reuse $(best ./ac x)"      >> "$OUT"
echo "== 矩阵乘法（计时换算成 GFLOP/s）=="
gf() { local t; t=$(best "$1"); python3 -c "print(f'{2*768**3/($t/1e6)/1e9:.2f} $t')"; }
echo "matmul handc_O2 $(gf ./mmf2)"                 >> "$OUT"
echo "matmul handc_O3native $(gf ./mmf3)"           >> "$OUT"
echo "matmul extc_O2 $(gf ./mme)"                   >> "$OUT"
echo "matmul extc_O2_unchecked $(gf ./mmu2)" >> "$OUT"
echo "matmul extc_O3native $(gf ./mmu)"             >> "$OUT"
echo "== 编译速度 =="
mapfile -t files < <(find tests -name '*.extc')
s=$(date +%s%N); n=0; for f in "${files[@]}"; do "$EXTC" -w --no-line-map -o /dev/null "$f" >/dev/null 2>&1 && n=$((n+1)); done; e=$(date +%s%N)
echo "compile $n $(( (e-s)/1000000 ))" >> "$OUT"
echo "== 完成：$OUT =="
python3 "$DIR/report.py" "$OUT"
