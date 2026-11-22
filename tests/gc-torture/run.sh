#!/usr/bin/env bash
# tests/gc-torture/run.sh —— 语料门禁：真实程序 + 对抗程序
#
# 用法：
#   ./tests/gc-torture/run.sh              # all：逐案精确 stdout + RSS 上界 + 停顿通道
#   ./tests/gc-torture/run.sh quick        # 只跑最小的档（编辑循环用）
#   ./tests/gc-torture/run.sh asan         # ASan+UBSan 档（t05/t07/t10 + datalog small）
#   ./tests/gc-torture/run.sh ladder       # 规模阶梯：每案两档，检查 RSS 上界
#   ./tests/gc-torture/run.sh one <name>   # 单案（调试用）
#
# 判据（三条，全部是硬判据）：
#   ① 精确 stdout：与 expected/<name>.txt 逐字相同（同输入 ⇒ 同输出，跨语言可对拍）
#   ② 峰值 RSS：<= 表里的上限（kB），由 /usr/bin/time -f %M 测得
#   ③ 停顿通道（PAUSE 构建）：[histo] over1ms=0；[wbudget] pop_max<=512 /
#      push_max<=8448 / mscap_max<=16384（编译期常量封顶，见 GC-ROOFLINE §6.13）
#
# 依赖：build/extc（先 make build/extc）、cc、/usr/bin/time、taskset（可选）。
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-/tmp/gt-run}
EXT=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
PIN=${PIN:-6}
MODE=${1:-all}

# name|args|rss_max_kb|stdin_kind|xfail
#   stdin_kind: none 或 "graph <edges> <queries> <nodes>"
#   xfail: 空 = 严格；非空 = 已归档的开放 RED（原因写在表里，行尾会打印 XFAIL 而不是 ok）
CASES=(
  "t01_sparse_pin|1000000 200|64000|none"
  "t02_fanout|40|32000|none"
  "t03_deepring|1000000 12|100000|none"
  "t04_bigchurn|120|24000|none"
  "t05_copyrefs|800000|8192|none"
  "t07_tasks|20000|6144|none"
  "t08_mixed_sizes|400 20000|8192|none"
  "t09_cycle_pressure|200000 20|24000|none"
  "t12_overwrite_arena|20 64|220000|none"
  "t13_growth_copy|20 8|260000|none"
  "datalog_lite|200000|32768|graph 200000 32 50000"
  "datalog_lite_small|2000|8192|graph 2000 4 2000"
)

pass=0; fail=0; skip=0; xf=0
declare -a FAILED=()

fail_case() {
  fail=$((fail + 1))
  FAILED+=("$1: $2")
  echo "  RED  $1 —— $2"
}

build_plain() { # src out
  local s=$1 o=$2
  "$EXT" "$s" -o "$o.c" >"$o.log" 2>&1 || return 1
  "$CC" -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -o "$o" "$o.c" >>"$o.log" 2>&1 || return 1
  return 0
}

build_pause() { # src out
  local s=$1 o=$2
  "$EXT" "$s" -o "$o.c" >"$o.log" 2>&1 || return 1
  "$CC" -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -DEXTC_RC_PAUSE=1 -o "$o" "$o.c" >>"$o.log" 2>&1 || return 1
  return 0
}

gen_input() { # kind file
  local kind=$1 file=$2
  case "$kind" in
    none) return 0 ;;
    graph\ *)
      set -- $kind
      python3 "$ROOT/tests/gc-torture/gen_graph.py" "$2" "$3" "$4" > "$file" ;;
    *) echo "unknown stdin kind: $kind"; return 1 ;;
  esac
}

run_one() { # name args rss stdin_kind mode [xfail]
  local name=$1 args=$2 rss=$3 stdin_kind=$4 mode=$5 xfail=${6:-}
  local src="$ROOT/tests/gc-torture/src/$name.extc"
  local base="$name"
  if [ "$name" = "datalog_lite_small" ]; then src="$ROOT/tests/gc-torture/src/datalog_lite.extc"; fi
  local c="$OUT/c/$base" b="$OUT/bin/$base" log="$OUT/log/$base"
  mkdir -p "$OUT/c" "$OUT/bin" "$OUT/log" "$OUT/in"
  local in_file=""
  if [ "$stdin_kind" != "none" ]; then
    in_file="$OUT/in/$base.txt"
    gen_input "$stdin_kind" "$in_file" || { fail_case "$name" "input generation"; return; }
  fi
  if ! build_plain "$src" "$b"; then fail_case "$name" "build (plain)"; sed -n 1,5p "$b.log"; return; fi
  local t0 t1 rss_kb rc
  t0=$(date +%s%N)
  if [ -n "$in_file" ]; then
    /usr/bin/time -f "%M" -o "$log.rss" taskset -c "$PIN" "$b" $args < "$in_file" > "$log.out" 2> "$log.err"
    rc=$?
  else
    /usr/bin/time -f "%M" -o "$log.rss" taskset -c "$PIN" "$b" $args > "$log.out" 2> "$log.err"
    rc=$?
  fi
  t1=$(date +%s%N)
  local wall_ms=$(( (t1 - t0) / 1000000 ))
  if [ $rc -ne 0 ]; then fail_case "$name" "exit $rc"; sed -n 1,5p "$log.err"; return; fi
  rss_kb=$(tail -1 "$log.rss" 2>/dev/null | tr -dc 0-9)
  if [ -z "$rss_kb" ]; then fail_case "$name" "no RSS reading"; return; fi
  if [ "${GT_SKIP_GOLDEN:-0}" != "1" ] && ! diff -q "$ROOT/tests/gc-torture/expected/$base.txt" "$log.out" > /dev/null 2>&1; then
    fail_case "$name" "stdout differs from expected/$base.txt"
    diff "$ROOT/tests/gc-torture/expected/$base.txt" "$log.out" | sed -n 1,6p
    return
  fi
  if [ "$rss_kb" -gt "$rss" ]; then fail_case "$name" "peak RSS ${rss_kb} kB > ${rss} kB"; return; fi
  if [ "$mode" = "pause" ]; then
    if ! build_pause "$src" "$b.p"; then fail_case "$name" "build (PAUSE)"; return; fi
    if [ -n "$in_file" ]; then
      EXTC_RC_PAUSE=1 taskset -c "$PIN" "$b.p" $args < "$in_file" > /dev/null 2> "$log.pause"
    else
      EXTC_RC_PAUSE=1 taskset -c "$PIN" "$b.p" $args > /dev/null 2> "$log.pause"
    fi
    local over pop push mscap
    over=$(grep -o "over1ms=[0-9]*" "$log.pause" | head -1 | cut -d= -f2)
    if [ -n "${over:-}" ] && [ "$over" != "0" ]; then
      if [ -n "$xfail" ]; then
        xf=$((xf + 1))
        printf "  xfail %-20s over1ms=%s (documented open RED: %s)\n" "$name" "$over" "$xfail"
        return
      fi
      fail_case "$name" "over1ms=$over (contract: 0)"; return
    fi
    pop=$(grep -o "pop_max=[0-9]*" "$log.pause" | head -1 | cut -d= -f2)
    push=$(grep -o "push_max=[0-9]*" "$log.pause" | head -1 | cut -d= -f2)
    mscap=$(grep -o "mscap_max=[0-9]*" "$log.pause" | head -1 | cut -d= -f2)
    if [ -n "${pop:-}" ] && [ "$pop" -gt 512 ]; then fail_case "$name" "pop_max=$pop > 512"; return; fi
    if [ -n "${push:-}" ] && [ "$push" -gt 8448 ]; then fail_case "$name" "push_max=$push > 8448"; return; fi
    if [ -n "${mscap:-}" ] && [ "$mscap" -gt 16384 ]; then fail_case "$name" "mscap_max=$mscap > 16384"; return; fi
  fi
  pass=$((pass + 1))
  printf "  ok   %-20s rss=%6s kB  wall=%5s ms%s\n" "$name" "$rss_kb" "$wall_ms" "${over:+ over1ms=$over}"
}

run_asan() { # name args stdin_kind
  local name=$1 args=$2 stdin_kind=$3
  local src="$ROOT/tests/gc-torture/src/$name.extc"
  local c="$OUT/asan/$name"
  mkdir -p "$OUT/asan" "$OUT/in"
  "$EXT" "$src" -o "$c.c" >"$c.log" 2>&1 || { fail_case "asan:$name" "build"; return; }
  "$CC" -O1 -g -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer -o "$c" "$c.c" >>"$c.log" 2>&1 \
    || { fail_case "asan:$name" "cc"; return; }
  if [ "$stdin_kind" != "none" ]; then gen_input "$stdin_kind" "$OUT/in/$name.txt"; fi
  local rc=0
  if [ "$stdin_kind" != "none" ]; then
    taskset -c "$PIN" "$c" $args < "$OUT/in/$name.txt" > "$c.out" 2> "$c.err" || rc=$?
  else
    taskset -c "$PIN" "$c" $args > "$c.out" 2> "$c.err" || rc=$?
  fi
  if [ $rc -ne 0 ]; then fail_case "asan:$name" "exit $rc"; sed -n 1,10p "$c.err"; return; fi
  if grep -q "AddressSanitizer\|runtime error:\|LeakSanitizer" "$c.err"; then
    fail_case "asan:$name" "sanitizer report"; sed -n 1,12p "$c.err"; return
  fi
  pass=$((pass + 1))
  echo "  ok   asan:$name"
}

mkdir -p "$OUT"
echo "== gc-torture ($MODE) =="
echo "-- compiler: $EXT"

if [ "$MODE" = "one" ]; then
  want=${2:-}
  [ -n "$want" ] || { echo "usage: run.sh one <name>"; exit 2; }
  for row in "${CASES[@]}"; do
    IFS="|" read -r name args rss stdin_kind xr <<< "$row"
    if [ "$name" = "$want" ]; then run_one "$name" "$args" "$rss" "$stdin_kind" pause "${xr:-}"; fi
  done
elif [ "$MODE" = "asan" ]; then
  run_asan t07_tasks "5000" none
  run_asan t05_copyrefs "20000" none
  run_asan t01_sparse_pin "20000 4" none
  run_asan datalog_lite "20000" "graph 20000 4 8000"
elif [ "$MODE" = "quick" ]; then
  run_one datalog_lite_small 2000 8192 "graph 2000 4 2000" pause
  run_one t07_tasks 20000 6144 none pause
elif [ "$MODE" = "ladder" ]; then
  # 规模阶梯：同一形状两档，检查峰值 RSS 仍在上界内（容量归还的验收口径）。
  # 两档的参数不同，stdout 必然不同：这一档只判 RSS 与停顿通道，不判 golden。
  export GT_SKIP_GOLDEN=1
  ladder_one() { # name small_args large_args rss1 rss2 stdin_kind
    local n=$1 a1=$2 a2=$3 r1=$4 r2=$5 k=$6
    echo "-- ladder $n: $a1 -> $a2"
    run_one "$n" "$a1" "$r1" "$k" pause
    run_one "$n" "$a2" "$r2" "$k" pause
  }
  ladder_one t01_sparse_pin "250000 100" "1000000 400" 16000 64000 none
  ladder_one t03_deepring "250000 4" "1000000 12" 32000 100000 none
  ladder_one t09_cycle_pressure "50000 8" "200000 20" 12000 24000 none
  ladder_one datalog_lite "50000" "200000" 16384 32768 "graph 200000 32 50000"
else
  for row in "${CASES[@]}"; do
    IFS="|" read -r name args rss stdin_kind xr <<< "$row"
    run_one "$name" "$args" "$rss" "$stdin_kind" pause "${xr:-}"
  done
fi

echo
if [ "$xf" -gt 0 ]; then
  echo "已归档的开放 RED（xfail，不算通过也不算失败）：$xf 条 —— 见 ../extc-docs/devlog/analysis/CORPUS-TORTURE.md"
fi
echo "通过 $pass，xfail $xf，失败 $fail"
if [ $fail -ne 0 ]; then
  printf "  %s\n" "${FAILED[@]}"
  exit 1
fi
exit 0