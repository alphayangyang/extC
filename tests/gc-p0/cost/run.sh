#!/usr/bin/env bash
# tests/gc-p0/cost/run.sh —— 四形状「每分配成本」的确定性归因（callgrind）
#
# 产出：每个形状一份 callgrind profile（开启回收）与一份 off 对照（EXTC_RECLAIM=0，
# 分配器退回纯 bump arena），再用 parse_callgrind.py 打印 Ir / 缓存 miss / 每分配分解。
# 这里**不测时间**：墙钟与 GC 时间引自 docs/analysis/MEASUREMENTS-V2-AFTER.md $11.1
# （已标注为"报告值"），见 docs/analysis/ALLOC-COST.md $1。
#
# 用法：
#   tests/gc-p0/cost/run.sh                      # 四形状全跑
#   SHAPES=promote tests/gc-p0/cost/run.sh       # 只跑一个
#   EXTC=./build-emp2/extc OUT=/tmp/gc-alloc-cost tests/gc-p0/cost/run.sh
#   NO_OFF=1 ...                                 # 跳过 off 对照（快一半）
#
# 依赖：valgrind（callgrind + --cache-sim）、cc。缺 valgrind 时打印 SKIP 并退出 0。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
EXTC=${EXTC:-$ROOT/build-emp2/extc}
OUT=${OUT:-/tmp/gc-alloc-cost}
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L}
V=${V:-valgrind}
SHAPES=${SHAPES:-promote sizeratio chase cycleratio}
NO_OFF=${NO_OFF:-0}

# bench 档参数（与 docs/analysis/MEASUREMENTS-V2-AFTER.md $11.1 一致）与分配次数
declare -A ARGS=(
  [promote]="500000 20261013"
  [sizeratio]="2000000 20261013"
  [chase]="1048576 20261013 4000000"
  [cycleratio]="400000 20261013 100"
)
declare -A ALLOCS=(
  [promote]=4550000 [sizeratio]=2000000 [chase]=1111076 [cycleratio]=1821441
)
declare -A STEPS=( [chase]=4000000 )
# 报告值（docs $11.1：GC 时间 / 墙钟），只用于换算 ns，不在这里测量
declare -A WALL_MS=( [promote]=40.8 [sizeratio]=23.4 [chase]=173.2 [cycleratio]=15.2 )
declare -A GC_MS=(   [promote]=16.4 [sizeratio]=2.41 [chase]=6.19 [cycleratio]=1.52 )

command -v "$V" >/dev/null 2>&1 || { echo "SKIP: valgrind not found (callgrind needed)"; exit 0; }
[ -x "$EXTC" ] || { echo "ERROR: EXTC=$EXTC not executable"; exit 2; }

mkdir -p "$OUT"/c "$OUT"/bin "$OUT"/cg "$OUT"/log
echo "EXTC=$EXTC  OUT=$OUT  CFLAGS=$CFLAGS"
echo "shapes: $SHAPES   off-profile: $([ "$NO_OFF" = 0 ] && echo yes || echo no)"

for s in $SHAPES; do
  echo
  echo "== $s  (args: ${ARGS[$s]})"
  # 1) 开启回收
  "$EXTC" "$ROOT/bench/gc/v2/src/$s.extc" -o "$OUT/c/$s.c" > "$OUT/log/$s.compile.log" 2>&1 \
    || { echo "  ERROR: extc failed for $s (see $OUT/log/$s.compile.log)"; tail -3 "$OUT/log/$s.compile.log"; continue; }
  $CC $CFLAGS -o "$OUT/bin/$s" "$OUT/c/$s.c" >> "$OUT/log/$s.compile.log" 2>&1 \
    || { echo "  ERROR: cc failed for $s"; tail -5 "$OUT/log/$s.compile.log"; continue; }
  "$OUT/bin/$s" ${ARGS[$s]} > "$OUT/log/$s.stdout" 2> "$OUT/log/$s.stderr"
  echo "  shape: $(head -1 "$OUT/log/$s.stdout")"
  "$V" --tool=callgrind --cache-sim=yes --callgrind-out-file="$OUT/cg/$s.out" \
       "$OUT/bin/$s" ${ARGS[$s]} > /dev/null 2> "$OUT/log/$s.callgrind.err" \
    || { echo "  ERROR: callgrind failed for $s"; continue; }
  callgrind_annotate --auto=no --show-percs=no --threshold=99.99 "$OUT/cg/$s.out" \
      > "$OUT/log/$s.annot.txt" 2>&1

  # 2) off 对照（关闭回收；分配器换成纯 bump arena，没有 GC）
  OFFARG=""
  if [ "$NO_OFF" = 0 ]; then
    EXTC_RECLAIM=0 "$EXTC" "$ROOT/bench/gc/v2/src/$s.extc" -o "$OUT/c/${s}_off.c" >> "$OUT/log/$s.compile.log" 2>&1
    $CC $CFLAGS -o "$OUT/bin/${s}_off" "$OUT/c/${s}_off.c" >> "$OUT/log/$s.compile.log" 2>&1 \
      || { echo "  ERROR: cc failed for ${s}_off"; continue; }
    "$V" --tool=callgrind --cache-sim=yes --callgrind-out-file="$OUT/cg/${s}_off.out" \
         "$OUT/bin/${s}_off" ${ARGS[$s]} > /dev/null 2> "$OUT/log/${s}_off.callgrind.err" \
      || { echo "  ERROR: callgrind failed for ${s}_off"; continue; }
    callgrind_annotate --auto=no --show-percs=no --threshold=99.99 "$OUT/cg/${s}_off.out" \
        > "$OUT/log/${s}_off.annot.txt" 2>&1
    echo "  off profile: $OUT/cg/${s}_off.out"
    OFFARG="--off $OUT/cg/${s}_off.out"
  fi

  # 3) 解析
  STEPARG=""
  if [ -n "${STEPS[$s]:-}" ]; then STEPARG="--steps ${STEPS[$s]}"; fi
  python3 "$HERE/parse_callgrind.py" "$OUT/cg/$s.out" $OFFARG \
      --allocs "${ALLOCS[$s]}" $STEPARG \
      --wall-ms "${WALL_MS[$s]}" --gc-ms "${GC_MS[$s]}" \
      --json > "$OUT/log/$s.json"
  python3 "$HERE/parse_callgrind.py" "$OUT/cg/$s.out" $OFFARG \
      --allocs "${ALLOCS[$s]}" $STEPARG \
      --wall-ms "${WALL_MS[$s]}" --gc-ms "${GC_MS[$s]}"
done

echo
echo "done: profiles in $OUT/cg, annotations in $OUT/log, json in $OUT/log/<shape>.json"
echo "note: no timing is measured by this script; wall/GC ms are reported values from docs §11.1"
