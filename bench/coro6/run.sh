#!/usr/bin/env bash
# bench/coro6/run.sh —— **六语言 × 协程 × 多进程（1/4/8）** 横评：一把跑完。
#
#   ① 同一个工作量、同一个校验和：K=512 个惰性生产者 × M=20000 个值
#      （v = (i%65536)*65521 + id*40503 + 轮号），六语言必须算出**同一个数** ⇒ 对拍闸门。
#   ② 每语言取各自最惯用的"协程"：extC `coroutine<i64>` · C++20 `co_yield` ·
#      Rust `async/.await`（std-only 最小 executor，每值 2 次 poll）· Go goroutine+无缓冲
#      channel（会合）· Java 25 虚拟线程+SynchronousQueue · Python generator。
#      机制不同 ⇒ 表里同时给"同语言纯循环"下界，读者自己看机制占多少。
#   ③ 单进程：不绑核，best of 3；多进程：每进程绑一个 P 核（0..N-1），best of 2。
#      R 按语言选到"单进程 ≈1 秒"（Java 的机制慢，R=1 就 20 秒）。
#   ④ 只量**运行**（编译不计入）；extC 产物按契约旗子 `gcc -O2 -std=c11 -fwrapv`。
#   ⑤ 原始记录写 raw.txt，RESULTS.md 由 report.py 生成 —— **别手改**。
#
# 用法：bench/coro6/run.sh            # 全部（约 5~8 分钟）
#       RUNS=1 bench/coro6/run.sh     # 快一点
set -u
cd "$(dirname "$0")/../.." || exit 1
DIR=bench/coro6
EXTC=${EXTC:-./build/extc}
RUNS=${RUNS:-2}
OUT=${OUT:-$DIR/raw.txt}
STEPS=10256000                      # 每轮 10.256M 步
if [ ! -x "$EXTC" ]; then echo "找不到 extC 前端：$EXTC（先 make）" >&2; exit 1; fi
# 下面要 cd 进临时目录构建 ⇒ 这里必须先把它钉成绝对路径（否则 ./build/extc 会失效 —— 实测踩过）
EXTC=$(readlink -f "$EXTC")
work=$(mktemp -d /tmp/bench_coro6.XXXXXX) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
cp "$DIR"/*.extc "$DIR"/*.cpp "$DIR"/*.rs "$DIR"/*.go "$DIR"/*.java "$DIR"/*.py "$work"/ 2>/dev/null
cd "$work" || exit 1
R_extc=200; R_cpp=100; R_rust=150; R_go=1; R_java=1; R_py=2
setR() { python3 - "$1" "$2" <<'PY'
import re,sys
R=sys.argv[2]; f=sys.argv[1]
PATS={'extc.extc':r'while r < i64\(R_ROUNDS\) \{','cpp.cpp':r'for\(int64_t r=0;r<R_ROUNDS;r\+\+\)',
 'rust.rs':r'for r in 0\.\.R_ROUNDS \{','go.go':r'for r := 0; r < R_ROUNDS; r\+\+',
 'Java.java':r'for \(int r = 0; r < R_ROUNDS; r\+\+\)','py.py':r'for r in range\(R_ROUNDS\):'}
NEW={'extc.extc':f'while r < i64({R}) {{','cpp.cpp':f'for(int64_t r=0;r<{R};r++)',
 'rust.rs':f'for r in 0..{R} {{','go.go':f'for r := 0; r < {R}; r++',
 'Java.java':f'for (int r = 0; r < {R}; r++)','py.py':f'for r in range({R}):'}
s=open(f,encoding='utf-8').read(); assert len(re.findall(PATS[f],s))==1, f
open(f,'w',encoding='utf-8').write(re.sub(PATS[f],NEW[f],s))
PY
}
setR extc.extc $R_extc; setR cpp.cpp $R_cpp; setR rust.rs $R_rust; setR go.go $R_go; setR Java.java $R_java; setR py.py $R_py
echo "== 构建（六语言 + extC 纯循环对照）=="
$EXTC -w --no-line-map -o extc.c extc.extc && gcc -O2 -std=c11 -fwrapv -o e_extc extc.c || exit 1
$EXTC -w --no-line-map -o loopextc.c loopextc.extc && gcc -O2 -std=c11 -fwrapv -o l_extc loopextc.c || exit 1
g++ -O2 -std=c++20 -fcoroutines -o e_cpp cpp.cpp && g++ -O2 -std=c++20 -o l_cpp loopcpp.cpp || exit 1
rustc --edition 2021 -O -o e_rust rust.rs && rustc --edition 2021 -O -o l_rust looprust.rs || exit 1
go build -o e_go go.go && go build -o l_go loopgo.go || exit 1
javac -d . Java.java && javac -d . LoopJava.java || exit 1
echo "== 对拍（六语言校验和必须一致）=="
sums=$( { ./e_extc; ./e_cpp; ./e_rust; ./e_go; java -cp . Java; python3 py.py; } | sort -u | wc -l )
if [ "$sums" != "1" ]; then echo "✗ 校验和不一致（$sums 种）—— 先修语义再谈性能" >&2; exit 1; fi
echo "  ✓ 六语言同一个校验和"
: > "$OUT"
ms() { local s e; s=$(date +%s%N); "$@" >/dev/null 2>&1; e=$(date +%s%N); echo $(( (e-s)/1000000 )); }
bestof() { local n="$1"; shift; local b=999999999 t; for _ in $(seq 1 "$n"); do t=$(ms "$@"); [ "$t" -lt "$b" ] && b=$t; done; echo "$b"; }
echo "== 单进程（R=1 口径换算成 ns/步）：协程 vs 同语言纯循环 =="
for L in "extc ./e_extc ./l_extc" "cpp ./e_cpp ./l_cpp" "rust ./e_rust ./l_rust" "go ./e_go ./l_go"; do
  set -- $L; c=$(bestof 3 $2); l=$(bestof 3 $3); echo "single $1 $c $l $STEPS" >> "$OUT"
done
jc=$(bestof 3 java -cp . Java); jl=$(bestof 3 java -cp . LoopJava); echo "single java $jc $jl $STEPS" >> "$OUT"
pc=$(bestof 3 python3 py.py); pl=$(bestof 3 python3 loopy.py); echo "single python $pc $pl $STEPS" >> "$OUT"
echo "== 多进程 1/4/8（每进程绑一个 P 核）=="
cmdof() { case "$1" in extc) echo "./e_extc";; cpp) echo "./e_cpp";; rust) echo "./e_rust";;
  go) echo "./e_go";; java) echo "java -cp . Java";; python) echo "python3 py.py";; esac; }
rowof() { case "$1" in extc) echo $R_extc;; cpp) echo $R_cpp;; rust) echo $R_rust;;
  go) echo $R_go;; java) echo $R_java;; python) echo $R_py;; esac; }
run_n() { local c="$1" n="$2" s e; s=$(date +%s%N)
  for i in $(seq 0 $((n-1))); do taskset -c "$i" $c >/dev/null 2>&1 & done; wait
  e=$(date +%s%N); echo $(( (e-s)/1000000 )); }
for L in extc cpp rust go python java; do
  c=$(cmdof "$L"); steps=$(( STEPS * $(rowof "$L") ))
  for N in 1 4 8; do w=$(run_n "$c" $N); echo "scale $L $N $w $steps" >> "$OUT"; done
  rss=$(/usr/bin/time -f "%M" $c 2>&1 >/dev/null | tail -1); echo "rss $L ${rss:-0}" >> "$OUT"
done
echo "== 完成：$OUT =="
python3 "$DIR/report.py" "$OUT" || exit 1
