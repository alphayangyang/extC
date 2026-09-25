#!/usr/bin/env bash
# bench/vararray —— **varArray vs stl::vector**（同一个工作量，判据：校验和必须一致）
#
# 形状**故意偏向 varArray**：它是 arena 底（没有池记录、没有板块 malloc、`push` 没有守卫），
# 所以在"短命、每轮新建、随地方整块回滚"的形状上应当占优。想知道它还有没有必要留，
# 就得先看它在**自己最有利**的形状上能不能赢；赢不了就没有保留的理由。
#
#   S1 每轮新建 + 填 4 个 + 丢弃（循环体是一个地方 ⇒ 每轮整块回滚）
#   S2 只构造/销毁，不填（把"建一个容器"的固定开销单独拎出来）
#   S3 每轮长到 4096 再丢（每轮几代扩容）
#   S4 **一个**容器长到 1e6（反例：这里该看 RSS，旧 buffer 还不了的是 varArray）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

run() {   # run <名字> <文件>
    local n=$1 f=$2
    if ! "$EXTC" "$f" -o "$TMP/$n.c" >/dev/null 2>&1 || ! "$CC" -O2 -std=c11 -o "$TMP/$n" "$TMP/$n.c" >/dev/null 2>&1; then
        echo "  FAIL $n  ->  编不过"; fail=1; return
    fi
    local t="" r=""
    for k in 1 2 3; do
        local s; s=$( { /usr/bin/time -f "%e %M" "$TMP/$n" >"$TMP/$n.out" 2>"$TMP/$n.t"; } 2>/dev/null; cat "$TMP/$n.t" )
        local e=${s%% *} m=${s##* }
        if [ -z "$t" ] || awk "BEGIN{exit !($e < $t)}"; then t=$e; r=$m; fi
    done
    echo "$t $r $(cat "$TMP/$n.out")"
}

printf '%-6s %-28s %-10s %-10s %s\n' "形状" "实现" "时间(s)" "峰值RSS(KB)" "校验和"
for shape in s1 s2 s3 s4; do
    case $shape in
        s1) desc="每轮新建+填4+丢弃 ×2e6" ;;
        s2) desc="只构造/销毁 ×2e6" ;;
        s3) desc="每轮长到4096再丢 ×2000" ;;
        s4) desc="一个容器长到1e6" ;;
    esac
    va=$(run ${shape}_va  bench/vararray/${shape}_va.extc)
    ve=$(run ${shape}_vec bench/vararray/${shape}_vec.extc)
    cva=$(echo "$va" | cut -d' ' -f3-); cve=$(echo "$ve" | cut -d' ' -f3-)
    tva=$(echo "$va" | cut -d' ' -f1); rva=$(echo "$va" | cut -d' ' -f2)
    tve=$(echo "$ve" | cut -d' ' -f1); rve=$(echo "$ve" | cut -d' ' -f2)
    printf '%-6s %-28s %-10s %-10s %s\n' "$shape" "$desc / varArray" "$tva" "$rva" "$cva"
    printf '%-6s %-28s %-10s %-10s %s\n' ""     "$desc / stl::vector" "$tve" "$rve" "$cve"
    if [ "$cva" != "$cve" ]; then echo "  FAIL $shape  ->  校验和不一致"; fail=1; fi
done
echo
[ "$fail" = 0 ] && echo "校验和全部一致" || echo "有失败项"
exit $fail
