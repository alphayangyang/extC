#!/usr/bin/env bash
# bench/buf —— buf<T>（extC 的原地增长缓冲区）vs std::vector（C++17）
#
# 五条形状（详见 SPEC.md）：S1 从 4 push 到 8e6 · S2 100 万个短命缓冲区 ·
# S3 装满后 shrink 再长回去 · S4 24 字节结构体元素 · S5 16e6 个 i64 顺序扫+随机索引扫。
# 判据：① 同一形状各臂的 n/sum 必须逐字相同（校验和）；② 计时用交错多轮的**最小值**，
#      单位毫秒（bash time 的 %3R，10 µs 分辨率）；③ 峰值 RSS 取 /usr/bin/time 的 ru_maxrss。
# 用法： bash bench/buf/run.sh [--reps N] [--pin N]
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
REPS=${REPS:-5}
PIN=${PIN:-6}
OUTDIR=${OUTDIR:-bench/buf}
while [ $# -gt 0 ]; do
    case $1 in
        --reps) REPS=$2; shift 2 ;;
        --pin)  PIN=$2; shift 2 ;;
        -h|--help) sed -n '2,9p' $0; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
B=${B:-$(mktemp -d)}; trap 'rm -rf "$B"' EXIT

build_arm() {   # build_arm <文件名>  -> 0 成功
    local n=$1 arm
    case $n in
        *.extc)
            arm=${n%.extc}
            "$EXTC" -w --no-line-map -o "$B/$arm.c" "bench/buf/$n" >/dev/null 2>&1 \
              && $CC -O2 -fwrapv -std=c11 -o "$B/$arm" "$B/$arm.c" >/dev/null 2>&1 || return 1 ;;
        *.cpp)
            arm=${n%.cpp}
            $CXX -O2 -std=c++17 -o "$B/$arm" "bench/buf/$n" >/dev/null 2>&1 || return 1 ;;
    esac
}

wall() {   # wall <arm> -> 毫秒（3 位小数，单位 s）
    local a=$1 out
    out=$( { TIMEFORMAT='%3R'; time taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>/dev/null; } 2>&1 )
    printf '%s\n' "$out"
}

once() {   # once <arm> -> "rss_kb output"（一次即可：ru_maxrss 与输出都是确定的）
    local a=$1 st
    st=$( { /usr/bin/time -f '%M' taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>"$B/$a.err"; } 2>&1; tail -1 "$B/$a.err" )
    printf '%s %s\n' "$st" "$(cat "$B/$a.out")"
}

norm() { sed 's/ moves=[0-9]*//; s/ cap=[0-9]*//; s/ leak=0//'; }

printf 'shape\tarm\tsec\trss_kb\tverdict\toutput\n' > "$OUTDIR/RESULTS.tsv"
fail=0
for spec in "s1 s1_buf.extc,s1_vec.cpp,s1_vecres.cpp 从 4 push 到 8e6 个 i64" \
            "s2 s2_buf.extc,s2_vec.cpp 100 万个短命缓冲区（各 64 个 i64）" \
            "s3 s3_buf.extc,s3_vec.cpp 装满 8e6 → 还页到 4096 → 原地长回去" \
            "s4 s4_buf.extc,s4_vec.cpp 24 字节结构体元素 2e6 个" \
            "s5 s5_buf.extc,s5_vec.cpp 16e6 个 i64：顺序扫 + 随机索引扫"; do
    set -- $spec
    shape=$1; arms=$2; desc=${spec#* * * }
    echo "== $shape：${spec#"$shape $arms "} =="
    built=""
    for f in $(echo "$arms" | tr ',' ' '); do
        if build_arm "$f"; then built="$built ${f%.*}"; else echo "  FAIL $f 编不过"; fail=1; fi
    done
    for a in $built; do
        best=""
        for k in $(seq "$REPS"); do
            ms=$(wall "$a")
            if [ -z "$best" ] || awk "BEGIN{exit !($ms < $best)}"; then best=$ms; fi
        done
        info=$(once "$a")
        rss=${info%% *}; out=${info#* }
        printf '%s\t%s\t%s\t%s\tmin-of-%s\t%s\n' "$shape" "$a" "$best" "$rss" "$REPS" "$out" >> "$OUTDIR/RESULTS.tsv"
        echo "  $a  min=${best}s  rss=${rss}KB  $out"
        eval "key_$a=\$(echo \"$out\" | norm)"
    done
    first=""
    for a in $built; do
        eval "k=\$key_$a"
        if [ -z "$first" ]; then first=$k; ref=$a
        elif [ "$k" != "$first" ]; then echo "  FAIL 校验和不一致：$ref=[$first] vs $a=[$k]"; fail=1; fi
        if [ -z "$first" ]; then :; fi
    done
done
echo
echo "结果写在 $OUTDIR/RESULTS.tsv（min-of-$REPS 交错多轮）"
[ "$fail" = 0 ] && echo "校验和全部一致" || echo "有失败项"
exit $fail