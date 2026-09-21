#!/usr/bin/env bash
# extC 全套自检：**一条命令**跑完 测试 + 攻击库 + 四语言横评
#   用法： ./check.sh          （全部）
#          ./check.sh quick    （只跑测试 + 攻击库，跳过基准）
set -u
cd "$(dirname "$0")"
pass=0; fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }

echo "== 构建 =="
if make -s >/tmp/extc-build.log 2>&1; then ok "make"; else bad "make"; cat /tmp/extc-build.log; exit 1; fi

echo "== 测试（例子 / 反例 / trap）=="
if out=$(./tests/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"; else bad "tests/run.sh"; echo "$out" | tail -5; fi

echo "== arena（按块细化：150MB 上限下不许涨）=="
if out=$(./tests/arena/run.sh 2>&1); then ok "$(echo "$out" | wc -l) 个用例"; else bad "tests/arena/run.sh"; echo "$out"; fi

echo "== 攻击库（通过的必须是 BASELINE 里那几条 ⇒ 没放松）=="
now=$(mktemp)
for f in tests/attacks/*.extc; do
    n=$(basename "$f" .extc)
    timeout 20 ./build/extc "$f" -o /dev/null >/dev/null 2>&1 && echo "$n"
done | sort > "$now"
if diff -q tests/attacks/BASELINE "$now" >/dev/null; then
    ok "通过集合与基线一致（$(wc -l < tests/attacks/BASELINE) 条已知安全 + 其余全部被挡）"
else
    bad "攻击库的通过集合变了！"; diff tests/attacks/BASELINE "$now" | sed 's/^/        /'
fi
rm -f "$now"

if [ "${1:-}" != "quick" ]; then
    echo "== 基准：extC vs C =="
    ./bench/run.sh 2>&1 | tail -n +1 | sed 's/^/  /' | tail -12
    echo "== 基准：重负载（bt / radix / mandelbrot）=="
    ./bench/heavy/run.sh 2>&1 | sed 's/^/  /'
    echo "== 基准：随机负载压力（同种子对拍 C）=="
    ./bench/stress/run.sh 2>&1 | sed 's/^/  /'
    echo "== 基准：**编译时长**（合成大程序；顺带抓「生成的 C 编不过」那类问题）=="
    if cout=$(NS=500 RUNS=1 ./bench/compile/run.sh 2>&1); then
        echo "$cout" | sed -n '/^N /,$p' | sed 's/^/  /'
        if echo "$cout" | grep -q "ERR"; then bad "编译时长压测（有 ERR）"
        else ok "编译时长压测（N=500 全通）"; fi
    else
        bad "编译时长压测"
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
