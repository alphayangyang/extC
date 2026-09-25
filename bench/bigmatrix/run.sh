#!/usr/bin/env bash
# bench/bigmatrix/run.sh —— 六语言重负载横评
#
#   构建（计时） → 对拍（六语言输出必须逐字节一致） → 计时（best of RUNS + 峰值 RSS）
#   → 生成 RESULTS.md
#
# 口径见 SPEC.md：数据全部从 stdin 读 · 每语言开满自己的优化 · 只看运行时间与 RSS。
# ⚠️ 这个脚本**故意串行**跑计时：并行会让数字互相污染 ✗
set -u
cd "$(dirname "$0")"
ROOT=../..
EXTC=$ROOT/build/extc
B=build
RUNS=${RUNS:-5}
SHAPES=${SHAPES:-"radix cdq bt mandel rebuild"}
mkdir -p "$B"

# ---------------------------------------------------------------- 输入
if [ ! -f in/radix.txt ] || [ ! -f in/cdq.txt ]; then
    cc -O2 -o "$B/gen" gen.c || exit 1
    "$B/gen" in || exit 1
fi
[ -f in-small/radix.txt ] || { cc -O2 -o "$B/gen" gen.c && "$B/gen" small in-small; }

# ---------------------------------------------------------------- 构建（计时）
build() {   # build <标签> <命令…>；时间写进 $B/<标签>.buildms，失败写 ERR
    local label="$1"; shift
    local t0 t1
    t0=$(date +%s%N)
    if "$@" >"$B/$label.build.log" 2>&1; then
        t1=$(date +%s%N)
        echo $(( (t1 - t0) / 1000000 )) > "$B/$label.buildms"
    else
        echo ERR > "$B/$label.buildms"
        echo "  ✗ 构建失败：$label（看 $B/$label.build.log）" >&2
    fi
}

build_all() {
    local s=$1
    # C
    build "${s}_c"   cc -O3 -march=native -fwrapv -o "$B/${s}_c" "src/$s.c"
    # C++
    build "${s}_cpp" g++ -O3 -march=native -std=c++20 -o "$B/${s}_cpp" "src/$s.cpp"
    # Rust
    build "${s}_rs"  rustc -C opt-level=3 -C target-cpu=native -C codegen-units=1 \
                            -o "$B/${s}_rs" "src/$s.rs"
    # Go（file 模式，不需要 module）
    build "${s}_go"  go build -o "$B/${s}_go" "src/$s.go"
    # Java（javac 单独计时；跑的时候 JVM 启动算在运行时间里）
    local cls
    cls=$(printf '%s' "$s" | sed 's/^./\U&/')
    build "${s}_java" javac -d "$B/java" "src/$cls.java"
    JAVA_CLASS[$s]=$cls
    # extC：前端 + 编它吐的 C，两段分开记（用户感受到的是两段之和）
    local t0 t1
    t0=$(date +%s%N)
    if $EXTC "src/$s.extc" -o "$B/${s}_extc.c" >"$B/${s}_extc.build.log" 2>&1; then
        t1=$(date +%s%N); echo $(( (t1 - t0) / 1000000 )) > "$B/${s}_extc.fe_ms"
        t0=$(date +%s%N)
        if cc -O3 -march=native -fwrapv -o "$B/${s}_extc" "$B/${s}_extc.c" >>"$B/${s}_extc.build.log" 2>&1; then
            t1=$(date +%s%N); echo $(( (t1 - t0) / 1000000 )) > "$B/${s}_extc.buildms"
        else
            echo ERR > "$B/${s}_extc.buildms"
        fi
    else
        echo ERR > "$B/${s}_extc.buildms"; echo ERR > "$B/${s}_extc.fe_ms"
        echo "  ✗ extC 构建失败：$s" >&2
    fi
}

# ---------------------------------------------------------------- 计时
measure() { # measure <命令…>：输出 "ms rss rc"（best of RUNS + 峰值 RSS）
    # ⚠️ 用本目录的 measure.py（`/usr/bin/time -f %M`），不用 bench/gc 那个：
    #    那个走 RUSAGE_CHILDREN 差值 ⇒ 小程序的 RSS 会被 Python 自己的页顶成 ~13MB 地板 ✗
    N=$RUNS STDIN_FILE="in/$SHAPE.txt" python3 measure.py "$@" 2>/dev/null
}

# ---------------------------------------------------------------- 主流程
declare -A JAVA_CLASS
echo "== 构建（每语言自己的满优化）=="
for s in $SHAPES; do
    SHAPE=$s
    echo "-- $s"
    build_all "$s"
done

echo
echo "== 对拍（小规模输入 + cdq 的边界输入，六语言输出必须逐字节一致）=="
verify_fail=0
verify() {  # verify <形状> <输入文件>
    local s=$1 input=$2
    local ref line
    ref=$(cc -O2 -fwrapv -o "$B/${s}_ref" "src/$s.c" && "$B/${s}_ref" < "$input")
    line="$(basename "$input")"
    for v in extc c cpp rs go java; do
        case $v in
        java) out=$(java -Xmx4g -cp "$B/java" "${JAVA_CLASS[$s]}" < "$input" 2>/dev/null);;
        *)    out=$("$B/${s}_$v" < "$input" 2>/dev/null);;
        esac
        if [ "$out" = "$ref" ]; then line="$line $v=✓"
        else line="$line $v=✗($out)"; verify_fail=1; fi
    done
    echo "  $s: $line"
}
for s in $SHAPES; do verify "$s" "in-small/$s.txt"; done
# cdq 的随机小输入**测不到** c = 10^6 这个边界（那正是 BIT 下标越界的地方，
# 全规模才发现）⇒ 另有一条人工边界输入，真值来自暴力 O(n^2)（check_brute.c）✓
[ -f in-small/cdq-edge.txt ] && verify cdq in-small/cdq-edge.txt

echo
echo "== 计时（$RUNS 次取最快；RSS = 峰值）=="
: > "$B/raw.tsv"
for s in $SHAPES; do
    SHAPE=$s
    for v in extc c cpp rs go java; do
        case $v in
        java) r=$(measure java -Xmx4g -cp "$B/java" "${JAVA_CLASS[$s]}");;
        *)    r=$(measure "$B/${s}_$v");;
        esac
        ms=$(echo "$r" | cut -f1); rss=$(echo "$r" | cut -f2)
        printf '%s\t%s\t%s\t%s\n' "$s" "$v" "$ms" "$rss" >> "$B/raw.tsv"
        printf '  %-8s %-5s %10s ms %10s MB\n' "$s" "$v" "$ms" "$rss"
    done
done

# ---------------------------------------------------------------- 交付文件 / 源码 大小
# 口径跟 bench/oi 那份报告一致：二进制 KB、源码**行数**、extC 另给**生成 C** 的大小 ✓
echo
echo "== 交付文件 / 源码大小 =="
# 空程序的生成 C = **固定运行时开销**（拆"生成 C 有多大"时必须减掉它，否则全是 preamble ✓）
printf 'fn main() -> i32 { return 0 }\n' > "$B/empty.extc"
"$EXTC" "$B/empty.extc" -o "$B/empty_extc.c" 2>/dev/null && wc -l < "$B/empty_extc.c" > "$B/empty_extc.lines"
: > "$B/sizes.tsv"
for s in $SHAPES; do
    cls=${JAVA_CLASS[$s]}
    # 交付文件（Java 是 .class，可能不止一个：内部类另出一份 ⇒ 一起加 ✓）
    num() { stat -c%s "$1" 2>/dev/null || echo 0; }
    b_extc=$(num "$B/${s}_extc");   b_c=$(num "$B/${s}_c");   b_cpp=$(num "$B/${s}_cpp")
    b_rs=$(num "$B/${s}_rs");       b_go=$(num "$B/${s}_go")
    b_java=$(cat "$B/java/$cls"*.class 2>/dev/null | wc -c)
    # extC 生成的 C
    cbytes=$(num "$B/${s}_extc.c"); clines=$(wc -l < "$B/${s}_extc.c" 2>/dev/null || echo 0)
    # 源码（每个形状自己的那个文件；C 家族另有共享的 fastio.h，单独注明）
    s_extc=$(num "src/$s.extc");   l_extc=$(wc -l < "src/$s.extc")
    s_c=$(num "src/$s.c");         l_c=$(wc -l < "src/$s.c")
    s_cpp=$(num "src/$s.cpp");     l_cpp=$(wc -l < "src/$s.cpp")
    s_rs=$(num "src/$s.rs");       l_rs=$(wc -l < "src/$s.rs")
    s_go=$(num "src/$s.go");       l_go=$(wc -l < "src/$s.go")
    s_java=$(num "src/$cls.java"); l_java=$(wc -l < "src/$cls.java")
    printf '%s\t%s %s %s %s %s %s\t%s %s %s %s %s %s\t%s %s\t%s %s %s %s %s %s\n' \
        "$s" \
        "$b_extc" "$b_c" "$b_cpp" "$b_rs" "$b_go" "$b_java" \
        "$s_extc" "$s_c" "$s_cpp" "$s_rs" "$s_go" "$s_java" \
        "$cbytes" "$clines" \
        "$l_extc" "$l_c" "$l_cpp" "$l_rs" "$l_go" "$l_java" >> "$B/sizes.tsv"
done
column -t -s $'\t' "$B/sizes.tsv" | sed 's/^/  /'

# ---------------------------------------------------------------- 出结果文件
python3 mkreport.py "$RUNS" "$verify_fail" > RESULTS.md
echo
echo "→ bench/bigmatrix/RESULTS.md 已生成"
[ "$verify_fail" = 0 ]
