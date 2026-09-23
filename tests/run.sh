#!/usr/bin/env bash
# extC week-0 回归测试：例子能跑通，坏代码能被编译期挡掉。
set -u
cd "$(dirname "$0")/.."

EXTC=./build/extc
pass=0
fail=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail + 1)); }

echo "== 构建 =="
if make -s 2>/tmp/extc-build.log; then ok "make"; else bad "make"; cat /tmp/extc-build.log; exit 1; fi

# 三个用例套件（正例 / 反例 / trap）交给 Python 并行跑。
# 每个用例都要起 extc + gcc，串行时几十核的机器只用 1 个 ✗
# 并发有界、结果**按提交顺序**回放 ⇒ 输出与串行时逐行一致（实测 diff = 0）✓
# 并发数：PAR_JOBS 覆盖；默认见 tools/parrun.py（不是 cpu_count —— 编译器和 gcc 各自还会派子进程）
if out=$(python3 tools/parrun.py ${PAR_JOBS:+--jobs "$PAR_JOBS"} 2>&1); then
    echo "$out"
    pass=$((pass + $(echo "$out" | tail -1 | grep -oE '通过 [0-9]+' | grep -oE '[0-9]+')))
else
    echo "$out"
    fail=$((fail + $(echo "$out" | tail -1 | grep -oE '失败 [0-9]+' | grep -oE '[0-9]+')))
fi

echo "== arena：按块细化（150MB 上限下不许涨）=="
if [ -x tests/arena/run.sh ]; then
    if out=$(tests/arena/run.sh 2>&1); then
        printf '%s\n' "$out" | grep -E "ok |FAIL"
        pass=$((pass + 1))
    else
        echo "$out"
        bad "arena 用例"
    fi
fi

echo "== 警告：该响的响、正例语料上**零误报**、\`-w\` 能关 =="
if [ -x tests/warnings/run.sh ]; then
    if out=$(tests/warnings/run.sh 2>&1); then
        printf '%s\n' "$out" | grep -E "ok |FAIL"
        pass=$((pass + 1))
    else
        echo "$out"
        bad "警告用例"
    fi
fi

echo "== ASan：内存安全形状必须**真的**跑得干净（不是"编过了"就算）=="
if [ -x tests/asan/run.sh ]; then
    if out=$(tests/asan/run.sh 2>&1); then
        echo "$out" | grep -E "ok |FAIL|skip" | while read -r line; do echo "$line"; done
        pass=$((pass + 1))
    else
        echo "$out"
        bad "ASan 用例"
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
