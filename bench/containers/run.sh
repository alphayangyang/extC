#!/usr/bin/env bash
# bench/containers —— 容器存储基质的 A/B：池板块（旧）vs buf<T> 预留（新）vs C++ std::vector
#
# 各臂编译**同一份 .extc 源**：
#   old = 池版本编译器（git worktree 检出 f910d14d 并自建），EXTC_STD 指向该树的 stdlib
#   new = 当前树的 ./build/extc
#   cpp = g++ -O2 -std=c++17
#   rs  = rustc -O（std::collections 的同形对照）
# 判据：① 三臂的输出逐字相同（等价工作）；② 交错多轮取最小值（毫秒）；③ 峰值 RSS 取 ru_maxrss。
# 用法：bash bench/containers/run.sh [--reps N] [--pin N] [--old DIR] [--shapes v1,v2,...]
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
RUSTC=${RUSTC:-rustc}
REPS=${REPS:-5}
PIN=${PIN:-6}
OLD=${OLD:-/tmp/ct/wt-old}
SHAPES=${SHAPES:-v1,v2,v3,v4,s1,s2,s3,h1,h2,h3,h4,h5,h6,hs1,hs2,lm1,lm2,ls1,m1,m2,st1}
OUTDIR=${OUTDIR:-bench/containers}
TSV=${TSV:-RESULTS.tsv}
while [ $# -gt 0 ]; do
    case $1 in
        --reps) REPS=$2; shift 2 ;;
        --pin)  PIN=$2; shift 2 ;;
        --old)  OLD=$2; shift 2 ;;
        --shapes) SHAPES=$2; shift 2 ;;
        --tsv)  TSV=$2; shift 2 ;;
        -h|--help) sed -n '2,10p' $0; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
if [ ! -x "$OLD/build/extc" ]; then
    echo "缺旧编译器：$OLD/build/extc（先 git worktree add -f $OLD f910d14d && make -C $OLD build/extc -j8）" >&2
    exit 2
fi
B=${B:-$(mktemp -d)}; trap 'rm -rf "$B"' EXIT

build_arm() {   # build_arm <extc源|.cpp> <old|new|cpp>
    local n=$1 arm=$2
    case $n in
        *.extc)
            case $arm in
                old) EXTC_STD="$OLD/stdlib" "$OLD/build/extc" -w --no-line-map -o "$B/$arm.c" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
                new) "$EXTC" -w --no-line-map -o "$B/$arm.c" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
            esac
            $CC -O2 -fwrapv -std=c11 -o "$B/$arm" "$B/$arm.c" >/dev/null 2>&1 || return 1 ;;
        *.cpp)
            $CXX -O2 -std=c++17 -o "$B/$arm" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
        *.rs)
            $RUSTC -O -o "$B/$arm" "bench/containers/$n" >/dev/null 2>&1 || return 1 ;;
    esac
}

wall() { local a=$1 out; out=$( { TIMEFORMAT='%3R'; time taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>/dev/null; } 2>&1 ); printf '%s\n' "$out"; }
once() { local a=$1 st; st=$( { /usr/bin/time -f '%M' taskset -c "$PIN" "$B/$a" >"$B/$a.out" 2>"$B/$a.err"; } 2>&1; tail -1 "$B/$a.err" ); printf '%s %s\n' "$st" "$(cat "$B/$a.out")"; }

printf 'shape\tarm\tsec\trss_kb\tverdict\toutput\n' > "$OUTDIR/$TSV"
fail=0
for shape in $(echo "$SHAPES" | tr ',' ' '); do
    case $shape in
        v1) desc='8e6 个 i64：从容量 4 增长并求和' ; src=v1_grow.extc; csrc=v1_vec.cpp ; rsrc=v1_vec.rs ;;
        v2) desc='2e6 装满后连做 5 次克隆' ; src=v2_clone.extc; csrc=v2_vec.cpp ; rsrc=v2_vec.rs ;;
        v3) desc='8e6 装满 → clear → 再装满' ; src=v3_reuse.extc; csrc=v3_vec.cpp ; rsrc=v3_vec.rs ;;
        v4) desc='20 万个小容器（容量 8）生死循环' ; src=v4_churn.extc; csrc=v4_vec.cpp ; rsrc=v4_vec.rs ;;
        s1) desc='1000 万次短串（15 字节）构造+读回，不该分配' ; src=s1_sso.extc; csrc=s1_sso.cpp ; rsrc=s1_sso.rs ;;
        s2) desc='16 条 4 MiB 长串，64 字节块 append 65536 次' ; src=s2_growstr.extc; csrc=s2_growstr.cpp ; rsrc=s2_growstr.rs ;;
        s3) desc='8 MiB 串 clone 20 次（深拷贝，搬 160 MiB）' ; src=s3_clone_str.extc; csrc=s3_clone_str.cpp ; rsrc=s3_clone_str.rs ;;
        h1) desc='4e6 条 i64 键：容量一次给够，插入后全量命中查询' ; src=h1_cap.extc;    csrc=h1_cap.cpp ; rsrc=h1_cap.rs ;;
        h2) desc='同样的 4e6 条键，但从容量 8 长起来（增长路径）' ; src=h2_grow.extc;   csrc=h2_grow.cpp ; rsrc=h2_grow.rs ;;
        h3) desc='8e6 次插入/查询/删除混在 1e6 个键上（墓碑与 swap-remove）' ; src=h3_churn.extc; csrc=h3_churn.cpp ; rsrc=h3_churn.rs ;;
        h4) desc='2e6 条之后两遍全表遍历（值列一遍、桶列一遍）' ; src=h4_iter.extc;   csrc=h4_iter.cpp ; rsrc=h4_iter.rs ;;
        h5) desc='4e6 条之后 8e6 次不命中查询（未命中路径）' ; src=h5_miss.extc;   csrc=h5_miss.cpp ; rsrc=h5_miss.rs ;;
        h6) desc='只做 4e6 次插入（把 h1 拆成插入侧与查询侧）' ; src=h6_putonly.extc; csrc= ; rsrc=h6_putonly.rs ;;
        hs1) desc='4e6 条 i64 键进 hashSetI64，容量一次给够，插入后全量查询' ; src=hs1_set.extc; csrc=hs1_set.cpp ; rsrc=hs1_set.rs ;;
        hs2) desc='1e6 键上 8e6 次插入/删除/查询（集合的墓碑与 swap-remove）' ; src=hs2_churn.extc; csrc=hs2_churn.cpp ; rsrc=hs2_churn.rs ;;
        lm1) desc='1024 条 i64 键的 linMap，20 万次线性查找' ; src=lm1_get.extc; csrc=lm1_get.cpp ; rsrc=lm1_get.rs ;;
        lm2) desc='1024 键上 20 万次写入/删除/读取（swap-remove）' ; src=lm2_churn.extc; csrc=lm2_churn.cpp ; rsrc=lm2_churn.rs ;;
        ls1) desc='1024 个成员的 linSet，20 万次成员查询' ; src=ls1_has.extc; csrc=ls1_has.cpp ; rsrc=ls1_has.rs ;;
        m1)  desc='有序 map 插入 1e6 条 i64 键，再全量查询' ; src=m1_putget.extc; csrc=m1_putget.cpp ; rsrc=m1_putget.rs ;;
        m2)  desc='有序 map 在 30 万键上 2e6 次写入/删除/读取' ; src=m2_churn.extc; csrc=m2_churn.cpp ; rsrc=m2_churn.rs ;;
        st1) desc='有序 set 插入 1e6 条、全量查询、再有序走一遍' ; src=st1_ordered.extc; csrc=st1_ordered.cpp ; rsrc=st1_ordered.rs ;;
        *) echo "unknown shape: $shape" >&2; exit 2 ;;
    esac
    echo "== $shape：$desc =="
    built=""
    for arm in old new cpp rs; do
        if [ "$arm" = cpp ] && [ -z "$csrc" ]; then continue; fi
        if [ "$arm" = rs ] && [ -z "$rsrc" ]; then continue; fi
        f=$src; [ $arm = cpp ] && f=$csrc; [ $arm = rs ] && f=$rsrc
        if build_arm "$f" "$arm"; then built="$built $arm"; else echo "  FAIL $arm 编不过（$f）"; fail=1; fi
    done
    for a in $built; do
        eval "best_$a="
    done
    for k in $(seq "$REPS"); do
        for a in $built; do
            ms=$(wall "$a")
            eval "cur=\$best_$a"
            if [ -z "$cur" ] || awk "BEGIN{exit !($ms < $cur)}"; then eval "best_$a=$ms"; fi
        done
    done
    first=""; ref=""
    for a in $built; do
        info=$(once "$a"); rss=${info%% *}; out=${info#* }
        eval "best=\$best_$a"
        printf '%s\t%s\t%s\t%s\tmin-of-%s\t%s\n' "$shape" "$a" "$best" "$rss" "$REPS" "$out" >> "$OUTDIR/$TSV"
        echo "  $a  min=${best}s  rss=${rss}KB  $out"
        if [ -z "$first" ]; then first=$out; ref=$a; elif [ "$out" != "$first" ]; then echo "  FAIL 输出不一致：$ref=[$first] vs $a=[$out]"; fail=1; fi
    done
done
echo
echo "结果写在 $OUTDIR/$TSV（交错 min-of-$REPS）"
[ "$fail" = 0 ] && echo '各臂输出逐字一致' || echo '有失败项'
exit $fail