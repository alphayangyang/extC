#!/usr/bin/env bash
# bench/vararray —— varArray / stl::vector / 旧编译器（池底）/ C++ / Rust 五臂同形对照
#
# 形状故意偏向 varArray：它是 arena 底（没有池记录、没有板块 malloc、push 没有守卫），
# 所以在短命、每轮新建、随地方整块回滚的形状上应当占优。
#
#   S1 每轮新建 + 填 4 个 + 丢弃（循环体是一个地方，每轮整块回滚）
#   S2 只构造/销毁，不填（把建一个容器的固定开销单独拎出来）
#   S3 每轮长到 4096 再丢（每轮几代扩容）
#   S4 一个容器长到 1e6（反例：这里该看 RSS）
#
# 臂：old = 池版本编译器（worktree f910d14d）；va / vec = 当前树的两种容器；
#     cpp = g++ -O2 -std=c++17 的 std::vector；rs = rustc -O 的 Vec。
# 判据：五臂输出逐字相同；交错多轮取最小墙钟；峰值 RSS 取 ru_maxrss。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=cc
CXX=g++
RUSTC=rustc
REPS=3
PIN=6
OLD=/tmp/ct/wt-old
if [ ! -x "$OLD/build/extc" ]; then
    echo "缺旧编译器：$OLD/build/extc" >&2
    exit 2
fi
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
fail=0

build() {   # build <形状文件> <臂>
    local f=$1 arm=$2
    case $f in
        *.extc)
            case $arm in
                old) EXTC_STD="$OLD/stdlib" "$OLD/build/extc" -w --no-line-map -o "$B/$arm.c" "bench/vararray/$f" >/dev/null 2>&1 || return 1 ;;
                va|vec) "$EXTC" -w --no-line-map -o "$B/$arm.c" "bench/vararray/$f" >/dev/null 2>&1 || return 1 ;;
            esac
            $CC -O2 -fwrapv -std=c11 -o "$B/$arm" "$B/$arm.c" >/dev/null 2>&1 || return 1 ;;
        *.cpp) $CXX -O2 -std=c++17 -o "$B/$arm" "bench/vararray/$f" >/dev/null 2>&1 || return 1 ;;
        *.rs)  $RUSTC -O -o "$B/$arm" "bench/vararray/$f" >/dev/null 2>&1 || return 1 ;;
    esac
}

once() {   # once <臂> -> "秒 峰值RSS(KB) 输出"
    local arm=$1 best="" brss="" tout="" k s e m
    for k in $(seq 1 "$REPS"); do
        s=$( { /usr/bin/time -f "%e %M" taskset -c "$PIN" "$B/$arm" >"$B/$arm.out" 2>"$B/$arm.t"; } 2>/dev/null; tail -1 "$B/$arm.t" )
        e=$(echo "$s" | cut -d" " -f1)
        m=$(echo "$s" | cut -d" " -f2)
        if [ -z "$best" ] || awk "BEGIN{exit !($e < $best)}"; then best=$e; brss=$m; fi
        tout=$(cat "$B/$arm.out")
    done
    printf "%s %s %s" "$best" "$brss" "$tout"
}

printf "%-6s %-28s %-10s %-12s %s\n" "形状" "臂" "时间(s)" "峰值RSS(KB)" "输出"
for shape in s1 s2 s3 s4; do
    num=$(echo "$shape" | cut -c2-)
    case $shape in
        s1) desc="每轮新建+填4+丢弃 x2e7" ;;
        s2) desc="只构造/销毁 x2e7" ;;
        s3) desc="每轮长到4096再丢 x2e4" ;;
        s4) desc="一个容器长到1e7" ;;
    esac
    ref=""
    for arm in old va vec cpp rs; do
        case $arm in
            old) src=$shape"_va.extc"; name="旧编译器(池底)" ;;
            va)  src=$shape"_va.extc"; name="varArray" ;;
            vec) src=$shape"_vec.extc"; name="stl::vector" ;;
            cpp) src="s"$num"_va.cpp"; name="C++ std::vector" ;;
            rs)  src=$shape"_va.rs"; name="Rust Vec" ;;
        esac
        if ! build "$src" "$arm"; then echo "  FAIL $shape/$name -> 编不过（$src）"; fail=1; continue; fi
        r=$(once "$arm")
        t=$(echo "$r" | cut -d" " -f1); m=$(echo "$r" | cut -d" " -f2); o=$(echo "$r" | cut -d" " -f3-)
        printf "%-6s %-28s %-10s %-12s %s\n" "$shape" "$desc / $name" "$t" "$m" "$o"
        if [ -z "$ref" ]; then ref=$o; elif [ "$o" != "$ref" ]; then echo "  FAIL $shape/$name -> 输出不一致（$o vs $ref）"; fail=1; fi
    done
done
echo
if [ "$fail" = 0 ]; then echo "五臂输出逐字一致"; else echo "有失败项"; fi
exit $fail
