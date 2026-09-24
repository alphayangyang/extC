#!/usr/bin/env bash
# tests/region/run.sh —— **区域注册表（REGIONS.md 期 1）的常设验收**
#
# 期 1 只有运行时与管理面（没有 `new (r) T[n]`，那是期 2），所以判据全部落在
# "注册表自己的账对不对"上：
#   ① 建 / 释放 / 世代：活着 live=1 gen=1；drop 后 live=0 且**旧 handle 的世代对不上** ✓
#   ② **块退出把子树带走**：块内建的 region 出块就没了（不需要手动 drop ✓）· 父 drop ⇒ 子一起走 ✓
#   ③ churn **内存平**：20 万次建/放之后 live=0 而槽位表容量停在 64（高水位 = 1）✓
#   ④ 生成物在 `-Wall -Wextra -Werror` 下编得过，且 ASan（含 LeakSanitizer）干净 ——
#      注册表是我们自己 malloc 的，漏一个字节这里就会响 ✓
set -u
cd "$(dirname "$0")/../.."

EXTC=./build/extc
CC=${CC:-gcc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

check() {   # check <名字> <文件> <期望的一整行>
    local name=$1 f=$2 want=$3
    if ! out=$("$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if [ "$out" = "$want" ]; then
        echo "  ok   $name  ->  $out"
    else
        echo "  FAIL $name  ->  期望「$want」，得到「$out」"; fail=1
    fi
}

echo "== ① 建 / 世代 / 释放 =="
check basic tests/region/basic.extc "live=0 up=1 gen=1 down=0 stale=0"

echo "== ② 块退出带走子树 · 父释放带走子 =="
check blockexit tests/region/blockexit.extc "before=0 in=1 rid=0 after=0 two=2 gone=0"

echo "== ③ churn：内存平（容量 = 高水位，与创建次数无关）=="
# acc = 1+2+…+200000（世代每次复用都自增 ✓ 这正是"旧 handle 查得出来"的机制 ✓）
check churn tests/region/churn.extc "live=0 cap=64 gen=0 acc=20000100000"

echo "== ④ 吞吐基线（期 1 没有类型化分配/遍历 ⇒ 先量注册表本身：建/放各 20 万次）=="
if "$EXTC" tests/region/churn.extc -o "$TMP/churn.c" 2>/dev/null \
   && $CC -std=c11 -O2 -o "$TMP/churn_fast" "$TMP/churn.c" 2>/dev/null; then
    # 三次取最好：这套机器上抖动很大，单次不可比
    best=99999
    for i in 1 2 3; do
        t0=$(date +%s%N)
        "$TMP/churn_fast" > /dev/null
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
        [ "$ms" -lt "$best" ] && best=$ms
    done
    echo "  ok   40 万次建/放 = ${best} ms（最快一次）⇒ $(awk -v m="$best" 'BEGIN{printf "%.1f", 400000.0/(m>0?m:1)/1000}') Mops/s"
    echo "       ⚠️ 这是**期 1 的对照数字**：期 2/3 加了类型化分配、buf 原地扩展、列式遍历之后，"
    echo "          注册表这一层不许明显变慢（遍历吞吐那一条要等有遍历 API 才谈得上 ✓）"
else
    echo "  FAIL 吞吐基线：生成或编译失败"; fail=1
fi

echo "== ⑤ 生成物：-Wall -Wextra -Werror + ASan =="
if "$EXTC" tests/region/churn.extc -o "$TMP/churn.c" 2>"$TMP/cerr"; then
    if $CC -std=c11 -Wall -Wextra -Werror -o "$TMP/churn" "$TMP/churn.c" 2>"$TMP/gerr"; then
        echo "  ok   生成的 C 在 -Wall -Wextra -Werror 下编得过 ✓"
    else
        echo "  FAIL 生成的 C 编不过："; head -4 "$TMP/gerr" | sed 's/^/        /'; fail=1
    fi
    if $CC -std=c11 -g -fsanitize=address -o "$TMP/churn_asan" "$TMP/churn.c" 2>/dev/null; then
        out=$("$TMP/churn_asan" 2>&1)
        if echo "$out" | grep -q "Sanitizer"; then
            echo "  FAIL ASan 报了：$(echo "$out" | grep -m1 -o 'ERROR: AddressSanitizer.*')"; fail=1
        else
            echo "  ok   ASan（含泄漏检查）干净 ✓ —— 注册表自己 malloc 的那块也被 free 了"
        fi
    else
        echo "  FAIL ASan 版本编不过"; fail=1
    fi
else
    echo "  FAIL 生成失败：$(head -2 "$TMP/cerr" | tr '\n' '|')"; fail=1
fi

exit $fail
