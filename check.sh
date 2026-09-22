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

echo "== 警告（该响的响 · 正例语料零误报 · \`-w\` 能关）=="
if out=$(./tests/warnings/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（含正例语料零误报 ✓）"
else bad "tests/warnings/run.sh"; echo "$out"; fi

echo "== 泛型自由函数（PLAN #47：推导 / 显式实参 / 推迟的 T: ==）=="
if out=$(./tests/generics/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 3 反例）"
else bad "tests/generics/run.sh"; echo "$out"; fi

echo "== 模块（定案 70：语义导入 · 一个文件一个模块 · @private · 禁环）=="
if out=$(./tests/modules/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（2 正例 + 6 反例）"
else bad "tests/modules/run.sh"; echo "$out"; fi

echo "== ASan（内存安全的形状必须真的跑得干净）=="
if out=$(./tests/asan/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 个形状 ASan 干净"
else bad "tests/asan/run.sh"; echo "$out"; fi

echo "== arena 层号（定案 68：检查器是唯一权威，codegen 只翻译 —— 不许漂）=="
# 哨兵 `EXTC_DBG_ARENA=1` 在**生成时**比对"检查器算的层号"与"codegen 当前块"✓
# 它不改变输出（golden 照旧逐字节相同 ✓），只是把"两个权威漂了"变成看得见的一行 ✗
drift=0; dsn=0
for f in examples/*.extc bench/*/*.extc bench/oi/*.extc bench/oi/persist/*.extc; do
    [ -f "$f" ] || continue
    dsn=$((dsn+1))
    if EXTC_DBG_ARENA=1 ./build/extc "$f" -o /dev/null 2>&1 | grep -q "arena!"; then
        drift=$((drift+1)); echo "        ✗ $f"
    fi
done
if [ "$drift" -eq 0 ]; then ok "$dsn 个语料：层号零漂移 ✓"
else bad "arena 层号漂移 $drift 处 ✗"; fi

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

    echo "== 基准：**OI 数量级四语言横评**（三维偏序 CDQ+BIT；缩小规模当回归）=="
    if oout=$(N=500000 RUNS=1 ./bench/oi/run.sh 2>&1); then
        echo "$oout" | sed -n '/^语言/,/^$/p' | sed 's/^/  /'
        ok "OI 横评（四语言输出一致 ✓）"
    else
        bad "OI 横评（有构建失败 / 语言之间对拍不一致 ✗）"
        echo "$oout" | sed 's/^/  /' | tail -20
    fi

    echo "== 基准：**主席树四语言横评**（P3834；小规模 + 暴力对拍，缩小规模当回归）=="
    if pout=$(N=200000 Q=200000 V=200000 RUNS=1 ./bench/oi/persist/run.sh 2>&1); then
        echo "$pout" | sed -n '/^  小规模对拍/p;/^语言/,/^$/p' | sed 's/^/  /'
        ok "主席树横评（四语言一致 + 暴力对拍 ✓）"
    else
        bad "主席树横评（构建失败 / 对拍不一致 ✗）"
        echo "$pout" | sed 's/^/  /' | tail -20
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
