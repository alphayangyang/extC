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
# ⚠️ 已知误拒：`examples/field-strong-update.extc`（见 `KNOWN-ISSUES.md`）——
# 它**只**许以"一条已记档的失败"出现；**多出任何一条别的失败就算红** ✗
# （理由：把已知项算进基线，但绝不掩盖新问题 ✓）
if out=$(./tests/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
else
    # ⚠️ 输出带 ANSI 颜色码 ⇒ 先剥掉再匹配（不剥的话 `^  FAIL` 一条都匹配不到 ✗ 踩过）
    plain=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
    # ⚠️ 白名单**现在是空的**（`field-strong-update` 的误拒 2026-09-23 已修 ✓）——
    # 机制留着，将来再有"已记档的失败"时按同样办法加一条，**不许**放宽成"忽略所有失败" ✗
    extra=$(printf '%s' "$plain" | grep '^  FAIL' || true)
    if [ -z "$extra" ]; then
        ok "$(echo "$out" | tail -1)（只差 field-strong-update —— 见 KNOWN-ISSUES.md ✓）"
    else bad "tests/run.sh"; echo "$out" | grep FAIL | head -5; fi
fi

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

echo "== IO 第一块（定案 73：std::sys 原语 + std::io 库 —— 能从 stdin 读了）=="
if out=$(./tests/io/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（stdin 读取 + 分层）"
else bad "tests/io/run.sh"; echo "$out"; fi

echo "== extern! + 信任声明（定案 72：签字才放行 · 默认最保守）=="
if out=$(./tests/extern/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 3 反例）"
else bad "tests/extern/run.sh"; echo "$out"; fi

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

echo "== 全限定名（PLAN #53：**全名是权利** · \`as\` 别名是方便）=="
if out=$(./tests/qname/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（6 正例 + 1 反例 + 1 结构判据）"
else bad "tests/qname/run.sh"; echo "$out" | tail -8; fi

echo "== std::fs 命名规范（定案 77：读型/写型分开 ⇒ 误用**编不过**）=="
if out=$(./tests/fs-shape/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 2 反例 + 常量不外露 + 规范入档）"
else bad "tests/fs-shape/run.sh"; echo "$out" | tail -8; fi

echo "== 头文件 include guard（内容不许落在 #endif 之后）=="
# 判据：每个 src/*.h 的**最后一个非空行**必须是 #endif。
# 为什么要有这一节：曾经有一段声明（连同它的文档注释）被追加到 types.h 的 #endif **之后**
# ⇒ 每被包含一次就重复声明一次。那次是靠 grep 撞出来的，这个脚本让它再也跑不掉 ✓
if out=$(python3 tools/check_guards.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "tools/check_guards.py"; echo "$out" | head -8
fi

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
