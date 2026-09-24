#!/usr/bin/env bash
# bench/io/run.sh —— **IO 速度大横评**（extC 流 · C++ cin/cout(C++14 与 latest) · Rust · Go · Java · C）
#
# 四个场景**分开量**（主人要求）：
#   ① stdin 输入   ② 文件输入   ③ stdout 输出   ④ 文件输出
#
# 契约（每个实现都遵守，所以可比 ✓）：
#   in  <文件|->      读所有整数，打印总和
#   out <文件|-> <n>  写 n 行 "i i*i"
#
# 公平性说明（写进 IO-BENCH.md ✓）：
#   - stdout 输出重定向到 **/dev/null**（隔离"库的开销"，不让终端/管道喧宾夺主 ✓）
#   - 文件输出写到同一块盘上的普通文件（页缓存计入 ⇒ 量的是"写系统调用 + 库开销"✓）
#   - 每个场景 **跑 3 次取最快**（去抖动 ✓）· 内存取 `/usr/bin/time` 的峰值 RSS（有则记）
#   - JVM 启动另给一个 no-op 基线（不然 Java 的数字说不清 ✓）
set -u
cd "$(dirname "$0")/../.."
B=bench/io
BIN=$B/bin
DATA=$B/data/big.txt
N=10000000
BYTES=$(stat -c%s "$DATA")
mkdir -p "$BIN" "$B/data"
HAVE_TIME=0; [ -x /usr/bin/time ] && HAVE_TIME=1
# ⚠️ 每一格都限时：**挂住 = 失败**，不是无限等 ✗（第一次跑就因为 stdin 没喂数据而挂住过 ✗）
TO=${TO:-60}

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- 构建
say "== 构建 =="
gcc -O2 -o "$BIN/c_in"  $B/c_in.c  -include unistd.h -include fcntl.h || exit 1
gcc -O2 -o "$BIN/c_out" $B/c_out.c || exit 1
for std in c++14 c++20; do
    for mode in sync fast; do
        tag="${std#c++}-$mode"
        extra=""; [ "$mode" = fast ] && extra="-DFAST"
        g++ -O2 -std=$std $extra -o "$BIN/cpp_in_$tag"  $B/cpp_in.cpp  || exit 1
        g++ -O2 -std=$std $extra -o "$BIN/cpp_out_$tag" $B/cpp_out.cpp || exit 1
    done
done
g++ -O2 -std=c++20 -DUSE_ENDL -o "$BIN/cpp_out_20-endl"      $B/cpp_out.cpp || exit 1
g++ -O2 -std=c++20 -DUSE_ENDL -DFAST -o "$BIN/cpp_out_20-fast-endl" $B/cpp_out.cpp || exit 1
rustc -O -o "$BIN/rs_in"  $B/rs_in.rs  || exit 1
rustc -O -o "$BIN/rs_out" $B/rs_out.rs || exit 1
(cd $B && go build -o bin/go_in go_in.go && go build -o bin/go_out go_out.go) || exit 1
javac -d "$BIN" $B/JavaIn.java $B/JavaOut.java $B/Noop.java || exit 1
for f in extc_in extc_out; do
    ./build/extc $B/$f.extc -o "$BIN/$f.c" || exit 1
    gcc -O2 -o "$BIN/$f" "$BIN/$f.c" || exit 1
done
say "  全部构建完成 ✓"

# ---------------------------------------------------------------- 正确性：所有实现答案必须一致
say "== 校验（所有实现必须给出同一个答案）=="
ref=$("$BIN/c_in" "$DATA"); ok=1
for impl in extc_in cpp_in_20-sync cpp_in_20-fast cpp_in_14-sync cpp_in_14-fast rs_in go_in; do
    got=$("$BIN/$impl" "$DATA")
    [ "$got" = "$ref" ] || { say "  FAIL $impl -> $got ≠ $ref"; ok=0; }
done
got=$(java -cp "$BIN" JavaIn "$DATA")
[ "$got" = "$ref" ] || { say "  FAIL JavaIn -> $got ≠ $ref"; ok=0; }
[ "$ok" = 1 ] && say "  输入侧一致：sum = $ref ✓"
refin=$("$BIN/c_in" - < "$DATA")
[ "$refin" = "$ref" ] && say "  重定向 stdin 与文件路径给出同一个答案 ✓" || say "  FAIL stdin ≠ file"

md5_c=$("$BIN/c_out" "$B/data/out_c.txt" $N && md5sum < "$B/data/out_c.txt" | cut -d' ' -f1)
for impl in extc_out cpp_out_20-sync rs_out go_out; do
    "$BIN/$impl" "$B/data/out_$impl.txt" $N
    m=$(md5sum < "$B/data/out_$impl.txt" | cut -d' ' -f1)
    [ "$m" = "$md5_c" ] || say "  FAIL $impl 输出文件与 C 不一致 ✗"
done
java -cp "$BIN" JavaOut "$B/data/out_java.txt" $N
m=$(md5sum < "$B/data/out_java.txt" | cut -d' ' -f1)
[ "$m" = "$md5_c" ] && say "  输出侧一致（md5 $md5_c ✓，$N 行）" || say "  FAIL 输出不一致 ✗"
OUTBYTES=$(stat -c%s "$B/data/out_c.txt")

# ---------------------------------------------------------------- 计时
# stdin 场景专用：把数据从 stdin 喂进去 ✓（`prog -` 读的就是它）
# ⚠️ 计时**必须**用 bash 内建的 `$EPOCHREALTIME`：`date +%s%N` 每次要 fork 两次，
#    而 WSL 的进程创建 ~50ms ⇒ 第一版整张表都是"107ms / 207ms / 407ms"，
#    量的是**测量开销**而不是程序 ✗（真实值：C 读 11.7MB 只要 9ms ✓）
now_us() { local t=$EPOCHREALTIME; printf '%d' $(( 10#${t%.*} * 1000000 + 10#${t#*.} )); }

# 峰值 RSS 单独一趟量（`/usr/bin/time` 也是一次 fork ⇒ 不放进计时的热路径 ✓）
rss_of() {
    [ "$HAVE_TIME" = 1 ] || { echo 0; return; }
    /usr/bin/time -f '%M' -o "$BIN/.rss" timeout "$TO" "$@" >/dev/null 2>&1
    cat "$BIN/.rss" 2>/dev/null || echo 0
}

bench() {   # bench <stdin数据|-> <标签> <命令…>  ⇒ 打印 "<最快毫秒> <峰值KB>"
    local stdinf=$1 label=$2; shift 2
    local best=99999999
    for i in 1 2 3; do
        local t0 t1 ms
        t0=$(now_us)
        if [ "$stdinf" = "-" ]; then timeout "$TO" "$@" >/dev/null 2>&1
        else                        timeout "$TO" "$@" < "$stdinf" >/dev/null 2>&1; fi
        t1=$(now_us)
        ms=$(( (t1 - t0) / 1000 ))
        [ "$ms" -lt "$best" ] && best=$ms
    done
    local rss=0
    if [ "$stdinf" = "-" ]; then rss=$(rss_of "$@")
    else                        rss=$(rss_of "$@" < "$stdinf"); fi
    printf '%-26s %8d %8d\n' "$label" "$best" "$(( rss / 1024 ))"
}

timeit()       { local l=$1; shift; bench -   "$l" "$@"; }
timeit_stdin() { local l=$1; shift; bench "$DATA" "$l" "$@"; }

header() { printf '%-26s %8s %8s\n' "实现" "毫秒" "峰值MB"; }

say ""
say "== ① stdin 输入（数据 $BYTES 字节，读 $N 行）=="
header
timeit_stdin "extc  cin >> (流)"         "$BIN/extc_in" -
timeit_stdin "C     fread+手写解析"      "$BIN/c_in" -
timeit_stdin "C++14 cin 默认"            "$BIN/cpp_in_14-sync" -
timeit_stdin "C++14 cin sync(false)"     "$BIN/cpp_in_14-fast" -
timeit_stdin "C++20 cin 默认"            "$BIN/cpp_in_20-sync" -
timeit_stdin "C++20 cin sync(false)"     "$BIN/cpp_in_20-fast" -
timeit_stdin "Rust  读整块+split"        "$BIN/rs_in" -
timeit_stdin "Go    Scanner+ScanWords"   "$BIN/go_in" -
# Java 走 stdin：用 shell 重定向喂进去 ✓
timeit_stdin "Java  BufferedReader+ST"   java -cp "$BIN" JavaIn -

say ""
say "== ② 文件输入（同一份数据，按路径打开）=="
header
timeit "extc  fin >> (流)"         "$BIN/extc_in" "$DATA"
timeit "C     fread+手写解析"      "$BIN/c_in" "$DATA"
timeit "C++14 cin 默认(ifstream)"  "$BIN/cpp_in_14-sync" "$DATA"
timeit "C++14 cin sync(false)"     "$BIN/cpp_in_14-fast" "$DATA"
timeit "C++20 cin 默认(ifstream)"  "$BIN/cpp_in_20-sync" "$DATA"
timeit "C++20 cin sync(false)"     "$BIN/cpp_in_20-fast" "$DATA"
timeit "Rust  读整块+split"        "$BIN/rs_in" "$DATA"
timeit "Go    Scanner+ScanWords"   "$BIN/go_in" "$DATA"
timeit "Java  BufferedReader+ST"   java -cp "$BIN" JavaIn "$DATA"

say ""
say "== ③ stdout 输出（写 $N 行，重定向到 /dev/null）=="
header
timeit "extc  cout << (流)"        "$BIN/extc_out" - $N
timeit "C     手写缓冲+write"       "$BIN/c_out" - $N
timeit "C++20 cout 默认 (\\n)"      "$BIN/cpp_out_20-sync" - $N
timeit "C++20 cout sync(false)"    "$BIN/cpp_out_20-fast" - $N
timeit "C++20 cout **endl**"       "$BIN/cpp_out_20-endl" - $N
timeit "C++20 cout fast+**endl**"  "$BIN/cpp_out_20-fast-endl" - $N
timeit "Rust  BufWriter"           "$BIN/rs_out" - $N
timeit "Go    bufio+AppendInt"     "$BIN/go_out" - $N
timeit "Java  BufferedWriter+SB"   java -cp "$BIN" JavaOut - $N

say ""
say "== ④ 文件输出（写 $N 行，$OUTBYTES 字节，同一块盘）=="
header
timeit "extc  fout << (流)"         "$BIN/extc_out" "$B/data/bench_extc.txt" $N
timeit "C     手写缓冲+write"       "$BIN/c_out" "$B/data/bench_c.txt" $N
timeit "C++20 cout sync(false)"     "$BIN/cpp_out_20-fast" "$B/data/bench_cpp.txt" $N
timeit "Rust  BufWriter"            "$BIN/rs_out" "$B/data/bench_rs.txt" $N
timeit "Go    bufio+AppendInt"      "$BIN/go_out" "$B/data/bench_go.txt" $N
timeit "Java  BufferedWriter+SB"    java -cp "$BIN" JavaOut "$B/data/bench_java.txt" $N

say ""
say "== JVM 启动基线（Java 的数字要减掉它才公平）=="
header
timeit "Java  no-op"                java -cp "$BIN" Noop
