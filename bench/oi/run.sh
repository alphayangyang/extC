#!/usr/bin/env bash
# bench/oi/run.sh —— **OI 数量级四语言横评**（2026-09-22）
#
# 负载：**三维偏序**（就是模板题「陌上花开」）—— CDQ 分治 + 权值树状数组。
#   · 它是"综合题"：排序 + 分治 + 树状数组 + 大量随机内存访问，四样都压到 ✓
#   · n = 4·10^6、值域 10^6 ⇒ C/C++/Rust 的常见写法跑 **~3 秒**（主人要的"顶满 2~3s"）✓
#   · extC **还没有 IO** ⇒ 数据用**同一个 pcg32 种子**在程序内生成，
#     四个语言必须打出**完全相同的四行输出**（gen 校验和 + m + max + 答案校验和）✓
#
# 量什么维度：
#   ① 构建时长   —— extC 还要单独看**前端**（extC→C）那一段
#   ② 运行时间   —— best of RUNS（`/usr/bin/time`）
#   ③ 峰值 RSS   —— `/usr/bin/time -v` 的 Maximum resident set size
#   ④ 二进制大小 —— 生成的 C 行数也一起看（extC 特有）
#   ⑤ 源码行数   —— 总行数 / 去掉注释空行（粗糙但一致，用来量"写法有多长"）
#   ⑥ 正确性     —— 四语言的输出**必须逐字节一致**，不一致就 FAIL
#
# ⚠️ 排序那一格**有意分组**（其余每一行都是同一套算法）：
#   · 主组「手写归并」：四个语言都手写同一套归并排序 ⇒ 苹果对苹果，比的是语言/编译器 ✓
#   · 对照组「std 排序」：C `qsort` · C++ `std::sort` · Rust `sort_unstable_by`
#     （extC **没有 std 排序** ⇒ 它只在主组里，报告里会写明）
#
# 用法： ./bench/oi/run.sh              （默认 N=4e6，跑全套）
#        N=500000 ./bench/oi/run.sh     （缩小规模 —— check.sh 里就是这么用的）
#        ./bench/oi/run.sh --report     （另外把结果写成 markdown：bench/oi/REPORT.md）
set -u
cd "$(dirname "$0")/../.."

OUT=build/oi
mkdir -p "$OUT"
RUNS=${RUNS:-3}
N=${N:-4000000}
V=${V:-1000000}
EXTC=${EXTC:-./build/extc}
CC=${CC:-gcc}
CXX=${CXX:-g++}
RUSTC=${RUSTC:-rustc}
OPT=${OPT:-2}                 # 优化级别：C/C++ 的 -O$OPT、Rust 的 -C opt-level=$OPT
REPORT=0
[ "${1:-}" = "--report" ] && REPORT=1

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 构建计时（毫秒）：失败就把错误打出来并记 ERR
build_ms() {
    local t0 t1 log="$TMP/build.log"
    t0=$(date +%s%N)
    if ! "$@" >"$log" 2>&1; then
        echo "ERR"; sed 's/^/      /' "$log" | head -8 >&2; return 1
    fi
    t1=$(date +%s%N); echo $(( (t1 - t0) / 1000000 ))
}

# 跑 RUNS 次：输出 "最快毫秒 最大RSS_MB"
bench_bin() {
    local bin=$1 best=99999999 rss=0 i out t
    for ((i = 0; i < RUNS; i++)); do
        out=$(/usr/bin/time -f "%e %M" "$bin" 2>&1 >/dev/null) || { echo "ERR ERR"; return; }
        t=$(echo "$out" | awk '{printf "%d", $1*1000}')
        local m=$(echo "$out" | awk '{print $2}')
        (( t < best )) && best=$t
        (( m > rss )) && rss=$m
    done
    echo "$best $(( rss / 1024 ))"
}

# 源码行数：总行数 / 非注释非空行
srclines() {
    local f=$1 tot code
    tot=$(wc -l < "$f")
    code=$(grep -vE '^[[:space:]]*(//|/\*|\*|$)' "$f" | wc -l)
    echo "$tot $code"
}

printf 'OI 数量级四语言横评 —— 三维偏序（CDQ + 树状数组）\n'
printf 'N=%s  V=%s  RUNS=%s  优化 -O%s   构建工具：%s / %s / %s\n\n' \
       "$N" "$V" "$RUNS" "$OPT" "$(gcc -dumpversion 2>/dev/null)" "$($CXX -dumpversion 2>/dev/null)" "$($RUSTC --version 2>/dev/null | cut -d' ' -f2)"

# ---- 生成 C 的路径 / 生成时间（extC 特有）----
# ---- 规模参数化：extC / Rust 的 N,V 是**源码里的编译期常数**（不是命令行旗子）----
#      ⇒ 按当前 N/V 生成一份副本再编（C/C++ 直接用 -D）✓
#      ⚠️ 这一步很关键：漏了它就会变成"extC 跑 4e6 而 C 跑 5e5"⇒ 整张表没意义 ✗（真踩过）
EXT_SRC=bench/oi/p3810.extc
RS_SRC=bench/oi/p3810.rs
EXT_BUILD=$EXT_SRC
RS_BUILD=$RS_SRC
if [ "$N" != 4000000 ] || [ "$V" != 1000000 ]; then
    EXT_BUILD="$OUT/p3810.N$N.extc"
    RS_BUILD="$OUT/p3810.N$N.rs"
    sed -e "s/let N: i64 = [0-9]*/let N: i64 = $N/" \
        -e "s/let V: i64 = [0-9]*/let V: i64 = $V/" \
        -e "s/\[4000000\]/[$N]/g" -e "s/\[1000001\]/[$((V + 1))]/g" \
        "$EXT_SRC" > "$EXT_BUILD"
    sed -e "s/const N: usize = [0-9_]*;/const N: usize = $N;/" \
        -e "s/const V: usize = [0-9_]*;/const V: usize = $V;/" \
        "$RS_SRC" > "$RS_BUILD"
fi

genC_ms=$(build_ms $EXTC "$EXT_BUILD" -o "$OUT/p3810.extc.c") || exit 1
genC_lines=$(wc -l < "$OUT/p3810.extc.c")
genC_kb=$(( $(stat -c%s "$OUT/p3810.extc.c") / 1024 ))

# ---- 构建全部变体 ----
declare -a NAME=() B_MS=() BIN=() SRC=() SRCCODE=() GEN=()
add() {   # add 名字 构建毫秒 二进制 源码文件 [生成C行数] [源码非空行]
    NAME+=("$1"); B_MS+=("$2"); BIN+=("$3"); SRC+=("$4"); SRCCODE+=("${6:-0}"); GEN+=("${5:-0}")
}

add "extC（手写归并）" "$(( genC_ms + $(build_ms $CC -O$OPT -w -o "$OUT/extc_merge" "$OUT/p3810.extc.c") ))" \
    "$OUT/extc_merge" "$EXT_SRC" "$genC_lines" "$(srclines "$EXT_SRC" | cut -d' ' -f2)"
add "C（手写归并）"     "$(build_ms $CC -O$OPT -DN=$N -DV=$V -o "$OUT/c_merge" bench/oi/p3810.c)" \
    "$OUT/c_merge" bench/oi/p3810.c 0 "$(srclines bench/oi/p3810.c | cut -d' ' -f2)"
add "C++（手写归并）"   "$(build_ms $CXX -O$OPT -DN=$N -DV=$V -o "$OUT/cpp_merge" bench/oi/p3810.cpp)" \
    "$OUT/cpp_merge" bench/oi/p3810.cpp 0 "$(srclines bench/oi/p3810.cpp | cut -d' ' -f2)"
add "Rust（手写归并）"  "$(build_ms $RUSTC -C opt-level=$OPT --crate-name p3810 -o "$OUT/rs_merge" "$RS_BUILD")" \
    "$OUT/rs_merge" "$RS_SRC" 0 "$(srclines "$RS_SRC" | cut -d' ' -f2)"

add "C（qsort）"        "$(build_ms $CC -O$OPT -DN=$N -DV=$V -DUSE_STD_SORT -o "$OUT/c_std" bench/oi/p3810.c)" \
    "$OUT/c_std" bench/oi/p3810.c 0 "$(srclines bench/oi/p3810.c | cut -d' ' -f2)"
add "C++（std::sort）"  "$(build_ms $CXX -O$OPT -DN=$N -DV=$V -DUSE_STD_SORT -o "$OUT/cpp_std" bench/oi/p3810.cpp)" \
    "$OUT/cpp_std" bench/oi/p3810.cpp 0 "$(srclines bench/oi/p3810.cpp | cut -d' ' -f2)"
add "Rust（sort_unstable）" "$(build_ms $RUSTC -C opt-level=$OPT --crate-name p3810 --cfg use_std_sort -o "$OUT/rs_std" "$RS_BUILD")" \
    "$OUT/rs_std" "$RS_SRC" 0 "$(srclines "$RS_SRC" | cut -d' ' -f2)"

# ⭐ 「安全设计的代价」对照：C + 手工边界检查（形状跟 extC 生成的一样）
#    —— 有它才能把"extC 慢的那点"拆成【检查的代价】和【实现/编译器的代价】✓
add "C（手写归并+边界检查）" "$(build_ms $CC -O$OPT -DCHECKED -DN=$N -DV=$V -o "$OUT/c_chk" bench/oi/p3810.c)" \
    "$OUT/c_chk" bench/oi/p3810.c 0 "$(srclines bench/oi/p3810.c | cut -d' ' -f2)"

# ---- 对拍 + 计时 ----
ref=""
worst=0
printf '%-22s %9s %9s %10s %10s %8s %7s  %s\n' \
       "语言 / 写法" "构建ms" "运行ms" "RSS(MB)" "二进制KB" "源码行" "生成C行" "对拍"
printf '%s\n' "------------------------------------------------------------------------------------------------------"
: > "$OUT/results.tsv"
for i in "${!NAME[@]}"; do
    # 构建失败就别往下跑 —— 否则会拿**上一次的旧二进制**量出一格假数据 ✗（真踩过）
    if [ "${B_MS[$i]}" = "ERR" ]; then
        printf '%-22s %9s %9s %10s %10s %8s %7s  %s\n' \
               "${NAME[$i]}" "ERR" "-" "-" "-" "${SRCCODE[$i]}" "${GEN[$i]}" "**构建失败 ✗**"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
               "${NAME[$i]}" "ERR" "-" "-" "-" "${SRCCODE[$i]}" "${GEN[$i]}" "**构建失败 ✗**" >> "$OUT/results.tsv"
        worst=1
        continue
    fi
    got=$("${BIN[$i]}" 2>&1 | tr '\n' '|')
    if [ -z "$ref" ]; then ref="$got"; verdict="基准"
    elif [ "$got" = "$ref" ]; then verdict="一致 ✓"
    else verdict="**不一致 ✗**"; worst=1; fi

    read -r rms rss <<<"$(bench_bin "${BIN[$i]}")"
    sz=$(( $(stat -c%s "${BIN[$i]}") / 1024 ))
    printf '%-22s %9s %9s %10s %10s %8s %7s  %s\n' \
           "${NAME[$i]}" "${B_MS[$i]}" "$rms" "$rss" "$sz" "${SRCCODE[$i]}" "${GEN[$i]}" "$verdict"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
           "${NAME[$i]}" "${B_MS[$i]}" "$rms" "$rss" "$sz" "${SRCCODE[$i]}" "${GEN[$i]}" "$verdict" >> "$OUT/results.tsv"
done

echo
echo "（构建 = extC 前端 + gcc 两段之和；extC 前端单独一段是 ${genC_ms} ms ✓）"
echo "（'生成C行' 只有 extC 有 —— 它就是"前端吐出来的那份 C 有多大"）"
echo "（主组 = 四个语言**手写同一套归并排序**；对照组 = 各自的 std 排序）"
echo "  对拍基准（第一行程序的全部输出）：$(echo "$ref" | tr '|' ' ')"

if [ "$REPORT" = 1 ]; then
    {
        echo "<!-- 由 bench/oi/run.sh --report 自动生成，别手改 -->"
        echo "# OI 数量级四语言横评 —— 三维偏序（CDQ 分治 + 树状数组）"
        echo
        echo "- 规模：N=$N、值域 V=$V（C/C++/Rust 的常见写法 ≈ 3 秒；主人要的「顶满 2~3s」）"
        echo "- 数据：**同一个 pcg32 种子**在程序内生成（extC 还没有 IO）⇒ 四语言输出必须逐字节一致"
        echo "- 优化：C/C++ \`-O$OPT\`、Rust \`-C opt-level=$OPT\`；extC = 前端 + 同一套旗子编它吐的 C"
        echo "- 时间 = best of $RUNS；RSS = \`/usr/bin/time -v\` 的 Maximum resident set size"
        echo
        printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' \
               "语言 / 写法" "构建 ms" "运行 ms" "RSS MB" "二进制 KB" "源码行" "生成 C 行" "对拍"
        printf '|%s|%s|%s|%s|%s|%s|%s|%s|\n' --- --- --- --- --- --- --- ---
        while IFS=$'\t' read -r a b c d e f g h; do
            printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' "$a" "$b" "$c" "$d" "$e" "$f" "$g" "$h"
        done < "$OUT/results.tsv"
        echo
        echo "> extC 的构建两段：前端（extC→C）**${genC_ms} ms**，编它吐的那份 C（**${genC_lines} 行 / ${genC_kb} KB**）是剩下那段 ✓"
        echo
        echo "## 怎么读这张表（很重要，不然会误读）"
        echo
        echo "1. **主组四行是苹果对苹果**：四个语言**手写同一套归并排序 + 同一套 CDQ + 同一套树状数组**，"
        echo "   连比较顺序都一模一样 ⇒ 差的那点就是**语言/编译器**的差 ✓"
        echo "2. **对照组三行不是**：C \`qsort\` / C++ \`std::sort\` / Rust \`sort_unstable_by\` 是三种不同的排序，"
        echo "   只该横着看\"这家标准库的排序值多少\"，不该当成语言快慢 ✗"
        echo "   （extC **没有 std 排序** ⇒ 它只在主组里，这是**语言能力差异**，不是它慢）"
        echo "3. **RSS 那一列才是"语言的常驻开销"**：四家都是同一套数组，所以数字应当非常接近；"
        echo "   差得多就说明有隐藏分配（Rust 的 \`Vec\` 就是显式的堆分配，extC 是全局数组）✓"
        echo "4. **二进制大小不能只跟 C 比**：Rust 默认静态链接 std ⇒ 十几 MB；"
        echo "   \`-C prefer-dynamic\` 或 \`strip\` 之后才可比 ✓"
        echo "5. **源码行数反映"写法长度"**：extC 没有 \`+=\` / \`++\` / \`for\` ⇒ 同样算法要多写十几行 "
        echo "   （这是**刻意的显式**，见 DECISIONS）✓"
        echo "6. **extC 的构建要拆两段看**：前端（extC→C，几毫秒）+ 编它吐的 C（大头）——"
        echo "   用户感受到的是两段之和 ✓"
        echo
        echo "### 正确性"
        echo
        echo "四个语言用的是**同一个 pcg32 种子**（20260922 / 流 54），程序内生成数据，"
        echo "最后打印 \`gen 校验和 / m / max / 答案校验和\` 四行 —— 任何一步（PRNG 复刻、排序、"
        echo "去重、CDQ、BIT）错了都会让这四行对不上 ✓"
        echo "本次基准输出：\`$(echo "$ref" | tr '|' ' ')\`"
    } > bench/oi/REPORT.md
    echo "→ 报告已写入 bench/oi/REPORT.md ✓"
fi

exit $worst
