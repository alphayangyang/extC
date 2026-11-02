#!/usr/bin/env bash
# bench/crazy —— 四语言横评：同一份输入、同一行结果。
# 单核段（parse/聚合/JSON/单线程归约）固定在 core 0；并行段（8 线程归约）单独放开 core 0-7。
set -u
cd "$(dirname "$0")/../.." || exit 1
B=bench/crazy
RUNS=${RUNS:-5}
PIN=${PIN:-0}

if [ ! -f "$B/data/events.tsv" ]; then
  echo "== generating data"
  ( cd "$B" && python3 gen.py data/events.tsv 1500000 )
fi

echo "== building"
./build/extc -w --no-line-map -o /tmp/crazy.c "$B/extc/main.extc" || exit 1
cc -O2 -std=c11 -fwrapv -o /tmp/crazy_extc /tmp/crazy.c || exit 1
rustc -O -o /tmp/crazy_rust "$B/rust/main.rs" || exit 1

: > /tmp/crazy_out.tsv

run_one() { # lang pin threads cmd...
  local lang=$1 pin=$2 threads=$3; shift 3
  taskset -c "$pin" env EXTC_PAR_THREADS="$threads" CRAZY_THREADS="$threads" "$@" 2>/dev/null
}

echo "== single-core phases (min of $RUNS, core $PIN)"
for lang in extc rust node python; do
  case $lang in
    extc)   cmd=(/tmp/crazy_extc) ;;
    rust)   cmd=(/tmp/crazy_rust) ;;
    node)   cmd=(node "$B/node/main.js") ;;
    python) cmd=(python3 "$B/python/main.py") ;;
  esac
  for r in $(seq 1 "$RUNS"); do
    out=$(run_one "$lang" "$PIN" 1 "${cmd[@]}")
    line1=$(printf '%s\n' "$out" | sed -n 1p)
    line2=$(printf '%s\n' "$out" | sed -n 2p)
    parse=$(printf '%s\n' "$line2" | sed -n 's/.*parse_us=\([0-9]*\).*/\1/p')
    agg=$(printf '%s\n' "$line2" | sed -n 's/.*agg_us=\([0-9]*\).*/\1/p')
    json=$(printf '%s\n' "$line2" | sed -n 's/.*json_us=\([0-9]*\).*/\1/p')
    par=$(printf '%s\n' "$line2" | sed -n 's/.*par_us=\([0-9]*\).*/\1/p')
    printf 'single\t%s\t%s\t%s\t%s\t%s\t%s\n' "$lang" "$parse" "$agg" "$json" "$par" "$line1" >> /tmp/crazy_out.tsv
  done
done

echo "== 8-thread reduce (min of $RUNS, cores 0-7)"
for lang in extc rust; do
  if [ "$lang" = extc ]; then cmd=(/tmp/crazy_extc); else cmd=(/tmp/crazy_rust); fi
  for r in $(seq 1 "$RUNS"); do
    line2=$(run_one "$lang" "0-7" 8 "${cmd[@]}" | sed -n 2p)
    par=$(printf '%s\n' "$line2" | sed -n 's/.*par_us=\([0-9]*\).*/\1/p')
    printf 'par8\t%s\t%s\n' "$lang" "$par" >> /tmp/crazy_out.tsv
  done
done

python3 "$B/report.py" /tmp/crazy_out.tsv
