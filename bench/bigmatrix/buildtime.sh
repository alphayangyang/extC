#!/usr/bin/env bash
# bench/bigmatrix/buildtime.sh —— **只构建，不运行**：每格重复 REPS 次，每一次都记下来
#
# 为什么单独有这个：run.sh 里每格只构建一次，一次 100ms 级的测量抖得很 ✗
# 这里给"最快 / 中位 + 每一次的原始值"，够看稳定性 ✓
#
# ⚠️ **Go 的构建缓存必须清掉**：`go build` 第二次起直接命中缓存 ⇒ 5 次里只有第 1 次
#    是真的在编译，跟 C/Rust/Java（每次都是真编）不可比 ✗
#    ⇒ 每次换一个 GOCACHE（std 也要重编，但那正是"冷构建"的真值 ✓）
#
# 用法：bash bench/bigmatrix/buildtime.sh            （REPS=5）
#       REPS=3 SHAPES="cdq rebuild" bash bench/bigmatrix/buildtime.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
EXTC=$ROOT/build/extc
B=build
REPS=${REPS:-5}
SHAPES=${SHAPES:-"radix cdq bt mandel rebuild"}
LANGS=${LANGS:-"fe extc c cpp rs go java"}   # 只想补某一家的数据时用（配 APPEND=1）
mkdir -p "$B"
# 默认清空重来；APPEND=1 时追加（补单家的数据用 ✓）
[ -n "${APPEND:-}" ] || : > "$B/btimes.tsv"

has() { case " $LANGS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

timeit() {  # timeit <形状> <语言> <命令…>
    local s=$1 l=$2; shift 2
    local t0 t1 ms
    t0=$(date +%s%N)
    if "$@" >/dev/null 2>&1; then
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
    else
        ms=ERR
    fi
    printf '%s\t%s\t%s\n' "$s" "$l" "$ms" >> "$B/btimes.tsv"
}

for rep in $(seq 1 "$REPS"); do
    echo "-- 第 $rep/$REPS 轮（语言：$LANGS）"
    # ⚠️ GOCACHE **必须是绝对路径**（相对路径 Go 直接拒绝：实测 25 次全记成 ERR ✗）
    # ⚠️ 而且要**按形状各给一个**：一个 rep 里共用同一只缓存的话，第一个形状付
    #    std 重编的代价、其余白捡热缓存 ⇒ 同一列里 1702ms 和 72ms 混在一起 ✗
    for s in $SHAPES; do
        cls=$(printf '%s' "$s" | sed 's/^./\U&/')
        # extC 拆两段：前端（.extc → C）+ 编它吐的那份 C
        has fe   && timeit "$s" fe   "$EXTC" "src/$s.extc" -o "$B/${s}_extc.c"
        has extc && timeit "$s" extc cc -O3 -march=native -fwrapv -o "$B/${s}_extc" "$B/${s}_extc.c"
        has c    && timeit "$s" c    cc -O3 -march=native -fwrapv -o "$B/${s}_c" "src/$s.c"
        has cpp  && timeit "$s" cpp  g++ -O3 -march=native -std=c++20 -o "$B/${s}_cpp" "src/$s.cpp"
        has rs   && timeit "$s" rs   rustc -C opt-level=3 -C target-cpu=native -C codegen-units=1 \
                              -o "$B/${s}_rs" "src/$s.rs"
        # Go：新 GOCACHE（见文件头那条 ⚠️）；`env` 只是个程序，照样能塞进 timeit ✓
        has go   && timeit "$s" go   env GOCACHE="$(pwd)/$B/gocache_${s}_r$rep" \
                                     go build -o "$B/${s}_go" "src/$s.go"
        has java && timeit "$s" java javac -d "$B/java" "src/$cls.java"
    done
done

rm -rf "$B"/gocache_r* "$B"/gocache_*_r*
echo "→ build/btimes.tsv（$(wc -l < "$B/btimes.tsv") 次构建，$(ls src/*.extc src/*.c src/*.cpp src/*.rs src/*.go src/*.java | wc -l) 个源文件）"
