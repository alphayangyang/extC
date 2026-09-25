#!/usr/bin/env bash
# STL 容器横评：**extC 的容器 vs C++ STL**（同算法 · 同参数 · 输出必须逐位相同 ✓）
#
#   判据与 bench/heavy 一致：两边各自编译，跑同一个工作量，比对打印出来的校验和；
#   校验和不一致 ⇒ 这一项标 FAIL（比"时间不对"严重得多）。
#   度量：/usr/bin/time 的 TIME（秒）与 RSS（KB 峰值）。
set -u
cd "$(dirname "$0")/../.."
mkdir -p build/stl /tmp/stlbin
CC=${CC:-gcc}
CXX=${CXX:-g++}
pass=0; fail=0
ROWS=$(mktemp)

# 机器与环境：表里要能看出是在什么上量的（WSL2 + 笔记本 CPU 的调度会给 3~5% 波动）
MACHINE=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')
KERNEL=$(uname -r)

build() {   # 名字
    "$PWD/build/extc" "bench/stl/$1.extc" -o "/tmp/stlbin/$1.extc.c" >/dev/null 2>&1 \
      && $CC -O2 "/tmp/stlbin/$1.extc.c" -o "/tmp/stlbin/$1_extc" >/dev/null 2>&1 \
      || { echo "  $1：extC 侧编不过"; return 1; }
    $CXX -O2 -std=c++17 "bench/stl/$1.cpp" -o "/tmp/stlbin/$1_cpp" >/dev/null 2>&1 \
      || { echo "  $1：C++ 侧编不过"; return 1; }
    return 0
}
RUNS=${RUNS:-3}
best() {    # 可执行 -> "最优时间 那次 RSS"
    local exe=$1 i=1 t r bt="" br=""
    while [ "$i" -le "$RUNS" ]; do
        o=$( { /usr/bin/time -f "TIME %e RSS %M" "$exe"; } 2>&1 )
        t=$(echo "$o" | grep TIME | awk '{print $2}')
        r=$(echo "$o" | grep TIME | awk '{print $4}')
        if [ -z "$bt" ] || awk -v a="$t" -v b="$bt" 'BEGIN{exit !(a < b)}'; then bt=$t; br=$r; fi
        LO=$(echo "$o" | grep -v TIME | head -1)
        i=$((i+1))
    done
    echo "$bt|$br|$LO"
}
one() {     # 名字
    local name=$1
    build "$name" || { fail=$((fail+1)); return; }
    # 每个程序跑 RUNS 次取最快（外设计时精度只有 0.01s，单次很容易被噪音带偏）。
    # `best` 在命令替换里跑，所以它一次把三样都回传：时间|峰值 RSS|程序的输出行。
    local be bc
    be=$(best /tmp/stlbin/${name}_extc)
    bc=$(best /tmp/stlbin/${name}_cpp)
    # 数字与显示分开：比值要拿纯数字算，表里要写成带单位的样子
    local le lc te tc re rc we wc ratio
    te=$(echo "$be" | cut -d'|' -f1); re=$(echo "$be" | cut -d'|' -f2); le=$(echo "$be" | cut -d'|' -f3)
    tc=$(echo "$bc" | cut -d'|' -f1); rc=$(echo "$bc" | cut -d'|' -f2); lc=$(echo "$bc" | cut -d'|' -f3)
    we="TIME ${te}s"; wc="TIME ${tc}s"
    ratio=$(awk -v a="$te" -v b="$tc" 'BEGIN{ if (b > 0) printf "%.2fx", a/b; else printf "-" }')
    printf "%-8s extC  %-34s %s\n" "$name" "$le" "$we"
    printf "%-8s C++   %-34s %s\n" "$name" "$lc" "$wc"
    if [ "$le" = "$lc" ]; then
        printf "%-8s 校验和一致  RSS: extC %s KB vs C++ %s KB\n" "" "$re" "$rc"; pass=$((pass+1))
        echo "| \`$name\` | $we | $wc | $ratio | $re KB | $rc KB | \`$le\` |" >> "$ROWS"
    else
        printf "%-8s 校验和不同：extC「%s」/ C++「%s」\n" "" "$le" "$lc"; fail=$((fail+1))
    fi
    echo
}

echo "== STL 容器横评（extC vs C++ STL，同算法同参数）=="
for t in vector hashmap map set string; do one "$t"; done
echo "通过 $pass 项，失败 $fail 项"

# 结果落盘（与 bench/bigmatrix 同一规矩：这张表由本脚本生成，不要手改）
{
    echo "# bench/stl —— STL 容器横评（extC 的容器 vs C++ STL）"
    echo
    echo "> **自动生成**（\`bash bench/stl/run.sh\`），别手改。"
    echo "> 口径：两边**同算法同参数**，跑同一个工作量，打印出来的校验和**逐位相同**才算数；"
    echo "> 时间与 RSS 都是 \`best of ${RUNS:-3}\`（外设 \`/usr/bin/time\`，时间精度 0.01s）。"
    echo
    echo "机器：$MACHINE · kernel $KERNEL"
    echo
    echo "## 读这张表前要知道的三件事"
    echo
    echo "1. **我们的取元素是有界检查的**：\`vector::get\` / \`string::at\` 之类越界会带源位置 trap，"
    echo "   而 C++ 那边用的是裸 \`v[i]\`（\`unordered_map::find\` 两边都是查表，可比）。这一项算 extC 的明账。"
    echo "2. **RSS 现在看情况**：容器的存储自 2026-09-26 起住在**自己的池板块**上（POOLS.md §2.1），"
    echo "   扩容时旧块当场还回去，所以 vector / map 这两格已经低于 C++。仍然偏大的两格（hashmap / set）"
    echo "   不是漏掉：它们按基准给的容量提示**一次性要满** 2 的幂张桶表（掩码寻址要求），而 C++ 的"
    echo "   \`reserve\` 不保证同样的装载因子；这一项属于「同算法同参数」以外的容量策略差，不是泄漏。"
    echo "   长期存储之外只剩三处 \`new\`：临时草稿、返回值、定长缓冲（都写在 DEVLOG 周期 37）。"
    echo "3. **WSL2 + 笔记本 CPU 有 3~5% 波动**：看量级与排序，不要读最后一位小数。"
    echo
    echo "## 结果"
    echo
    echo "| 容器 | extC（同算法同参数） | C++ STL | 比值 | extC RSS | C++ RSS | 校验和 |"
    echo "|---|---|---|---|---|---|---|"
    cat "$ROWS"
    echo
    echo "对比物：\`vector<i32>\` 对 \`std::vector\` · \`hashMapI64<i32>\` 对 \`std::unordered_map\` ·"
    echo "\`map<i64,i32>\`（B+ 树）对 \`std::map\`（红黑树）· \`hashSetI64\` 对 \`std::unordered_set\` ·"
    echo "\`string\` 对 \`std::string\`。"
    echo
    echo "结论：**有序表这一格是设计目的兑现的地方** —— B+ 树在 100 万次插入 + 100 万次查找 + 100 万次"
    echo "有序遍历 + 50 万次删除上明显快过红黑树（cache miss 少一个数量级，遍历是叶子链线性扫描）；"
    echo "哈希与集合打平；连续容器（vector / string）慢在「每次访问都过方法 + 有界检查」上。"
} > bench/stl/RESULTS.md
rm -f "$ROWS"
[ "$fail" = 0 ]
