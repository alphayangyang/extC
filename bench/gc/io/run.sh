#!/usr/bin/env bash
# bench/gc/io/run.sh —— 读输入的两个实现：**extC 的 std::io vs 手写 C**
#
# 形状必须一样才可比：两边都是"自己管 64KB 块 + 下标式解析/扫描"，
# 旗子也同一套（`-O2 -fwrapv`，见 src/main.c）✓
# 输出必须逐字节一致，不然量出来的不是同一件事 ✗
set -u
cd "$(dirname "$0")"
ROOT=../..

# 输入**不再入库**（两个文件 36MB，占全新克隆的九成）⇒ 缺了就现场生成 ✓
# in_lines.txt 是逐字节复现；in_ints.txt 同规模同分布（生成器里写清了差异）
if [ ! -f in_ints.txt ] || [ ! -f in_lines.txt ]; then
    cc -O2 -o /tmp/extc_io_gen gen.c && /tmp/extc_io_gen . || { echo "生成输入失败"; exit 1; }
fi
B=$ROOT/build/gc-io
mkdir -p "$B"

build() {  # build <shape>
    local s=$1
    $ROOT/build/extc read_$s.extc -o "$B/${s}_extc.c" >/dev/null || { echo "extC 编不过 $s"; return 1; }
    cc -O2 -fwrapv -o "$B/${s}_extc" "$B/${s}_extc.c" || return 1
    cc -O2 -fwrapv -o "$B/${s}_c" read_$s.c || return 1
}

for s in ints lines tokens; do
    case $s in ints) inp=in_ints.txt;; lines) inp=in_lines.txt;; tokens) inp=in_ints.txt;; esac
    build $s || continue
    a=$("$B/${s}_extc" < "$inp"); b=$("$B/${s}_c" < "$inp")
    echo "== $s（$inp）=="
    if [ "$a" = "$b" ]; then echo "  对拍一致 ✓  $a"
    else echo "  ✗ 输出不一致：extC [$a] vs C [$b]"; continue; fi
    for v in extc c; do
        printf '  %-12s' "$([ $v = extc ] && echo 'extC std::io' || echo 'C 手写')"
        python3 time.py --bin "$B/${s}_$v" "$inp" 9 | sed 's/^[^ ]*  //'
    done
    echo
done
