#!/usr/bin/env bash
# extC week-0 回归测试：例子能跑通，坏代码能被编译期挡掉。
set -u
cd "$(dirname "$0")/.."

EXTC=${EXTC:-./build/extc}   # `EXTC=build-dbg/extc` 可跑断言版（见 src/dbg.h）
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

# 注解套件放在这里也跑一遍（`check.sh` 另有自己那一节）：`@unchecked` 的判据之一是**生成物**
# 断言 —— 检查有没有按函数粒度消失 —— 它和 ASan 一样属于"编过了不算数"的那一类。
echo "== 注解：@inline / @unchecked 要真的生效，写错的必须编译期报错 =="
if [ -x tests/annot/run.sh ]; then
    if out=$(tests/annot/run.sh 2>&1); then
        # 这一节自己也上色 ⇒ 先剥掉 ANSI 再筛行（不剥的话 `ok ` 一条都匹配不到，踩过）
        printf '%s\n' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E "ok |FAIL"
        pass=$((pass + 1))
    else
        echo "$out"
        bad "注解用例"
    fi
fi

echo "== 回收运行期：发射出的文本 + 驱动断言（owns 守卫 / 描述符随槽）=="
# 被测的是 `src/back/reclaim.c` **发射出来的**那份文本：run.sh 先用仓库的发射口导成
# extc_reclaim_runtime.inc，再和断言程序一起编译（另有 ASan/UBSan 一遍）⇒ 断言的是生成文件里
# 真正跑的那份代码。这是门禁：编译不过或断言不过就是红。
if [ -x tests/reclaim/run.sh ]; then
    if out=$(tests/reclaim/run.sh 2>&1); then
        printf '%s\n' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E "ok |FAIL"
        pass=$((pass + 1))
    else
        echo "$out"
        bad "回收运行期用例"
    fi
fi

echo "== 回收行为基线：只记录现状（exit 0），红条目在表里标出 =="
# 与 tests/arena-soundness 同口径：这是证据，不是回归判据 ⇒ 永远 exit 0，不计入失败。
# 只在开关下成立的判据在下面的 reclaim-shapes/run-switch.sh 里，那一条才是门禁。
# ⚠️ 这里 `env -u EXTC_DBG`：基线记录的是**普通构建**下的形状；assert 版下 cyclic_shape 会撞到
# 一个与回收无关的已知逃逸分析兜底（check_escape.c:730），那张表不该被它污染（断言版另有那一节）。
if [ -x tests/reclaim-shapes/run.sh ]; then
    if out=$(env -u EXTC_DBG tests/reclaim-shapes/run.sh 2>&1); then
        printf '%s\n' "$out" | sed 's/\x1b\[[0-9;]*m//g' \
            | grep -E '^(cond-save|reject_|cyclic-shape|promote-place|scale-growth)' | sed 's/^/  /'
        pass=$((pass + 1))
    else
        echo "$out"
        bad "回收行为基线"
    fi
fi

echo "== 回收开关（EXTC_RECLAIM=1）：界与轮数解耦 + 曾经的 SEGV 用例不崩 =="
# 这一节是**门禁**：`scale_growth` N vs 4N 的峰值比 < 1.5；关掉开关这条必须红
# （`EXTC_RECLAIM=0 tests/reclaim-shapes/run-switch.sh` ⇒ 2.99× ⇒ exit 1，判据有牙）。
if [ -x tests/reclaim-shapes/run-switch.sh ]; then
    if out=$(tests/reclaim-shapes/run-switch.sh 2>&1); then
        printf '%s\n' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E "ok |FAIL"
        pass=$((pass + 1))
    else
        echo "$out"
        bad "回收开关断言"
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
