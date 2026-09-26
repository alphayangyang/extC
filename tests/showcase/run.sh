#!/usr/bin/env bash
# 展示代码套件：`examples/showcase-*.extc` —— 文档 docs/EXTC-SHOWCASE.md 里的六段。
#
# 为什么单独一套：`examples/` 里的文件此前**没有任何套件在跑**（只有已弃置的 golden 工具编译过），
# 于是"展示代码"最容易悄悄腐烂。这里让它们每次 check.sh 都**编译并运行**，输出逐字比对。
#
# 约定：多行输出整块比对（run_exact）；随进程变化的量（如 pid）只比对可确定的那一行（run_grep）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
fail=0

run_exact() {   # run_exact <名字> <文件> <期望的多行输出>
    local name=$1 f=$2 want=$3 out
    if ! out=$(timeout 60 "$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if [ "$out" = "$want" ]; then
        echo "  ok   $name  ->  $(echo "$out" | head -1)"
    else
        echo "  FAIL $name  ->  输出对不上"
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$out") | head -8 | sed 's/^/        /'
        fail=1
    fi
}

run_grep() {    # run_grep <名字> <文件> <必须出现的一行>
    local name=$1 f=$2 want=$3 out
    if ! out=$(timeout 60 "$EXTC" --run "$f" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; fail=1; return
    fi
    if echo "$out" | grep -qF -- "$want"; then
        echo "  ok   $name  ->  $(echo "$out" | tail -1)"
    else
        echo "  FAIL $name  ->  输出里没有「$want」"; fail=1
    fi
}

echo "== 展示代码：Arena 返回值（零标注 · home 机制）=="
run_exact showcase-arena-return examples/showcase-arena-return.extc "n=5 sum(y)=30"

echo "== 展示代码：home 机制（被调者的分配落在调用者的分区）=="
run_exact showcase-home examples/showcase-home.extc "len=6 sum=55"

echo "== 展示代码：池的身份（同一格，不同代）=="
run_exact showcase-pool-identity examples/showcase-pool-identity.extc \
"h1 格=0 代=0
h2 格=1 代=0
h3 格=0 代=1
陈旧句柄 → none（不会指向 h3 的 333）
h3 → 333"

echo "== 展示代码：视图（拷贝是默认，零拷贝要签字）=="
run_exact showcase-views examples/showcase-views.extc "copy.len=11 view=5 sum=532"

echo "== 展示代码：impl 与运算符重载（含内建标量）=="
run_exact showcase-impl examples/showcase-impl.extc "c=(11,22) norm2=605
i64::doubled=42"

echo "== 展示代码：与 C 同层（extern! + effects）=="
run_grep showcase-extern examples/showcase-extern.extc "pid>0=true"

if [ "$fail" = 0 ]; then echo "通过 6，失败 0"; else echo "通过 $((6-1))，失败 1"; exit 1; fi
