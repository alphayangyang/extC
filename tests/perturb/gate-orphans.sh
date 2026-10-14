#!/usr/bin/env bash
# tests/perturb/gate-orphans.sh —— 扰动普查的**进程卫生**判据：一次有界子集普查（**必含超时用例**）
# 跑完，`/tmp/extc-perturb-runs/` 下必须**一个进程都不剩**。
#
# 为什么要有这条：`extc --run` 会 fork 出被编译的程序（`tests/coro/echo_server.extc` 这类还会再
# fork 后代）。只杀编译器，那些程序就被 double-fork 甩给 init 当孤儿 —— 实测一次有界普查
# （2 个源 × 1 档 × `--timeout 2`）留下 **9 个** `/tmp/extc-perturb-runs/...`，`echo_server_fork`
# 的还在不停再生；跨多轮普查会累积到上百个。`tests/perturb/census.py` 现在每个用例跑在自己的
# **会话/进程组**里（`start_new_session=True`）+ 登记（复用 `tools/parrun.py` 的三件原语），超时
# `killpg` 整个组。这条判据把那件事钉住：改回"只杀编译器" ⇒ 当场红。
#
# 判据（四格）：
#   ① 跑之前本判据的**私有** scratch 根下必须是 0 个进程（否则拒绝开跑，见下面的 RUN_ROOT），
#      本判据认不出哪些是自己的后代，宁可红也不误杀别人）；
#   ② 普查 rc==0；
#   ③ **反空跑**：这一轮必须真的发生 ≥1 次超时（否则没走到"杀进程组"那条路，判据是空转）；
#   ④ 跑完 `pgrep -f '^<私有 scratch 根>/'` 必须为 0。
#
# 档位：进 `nobench`（不进 `smoke`）—— 2 个源 × 1 档 × timeout 2s ≈ 4~6s，属"每次都要跑"的
# 卫生判据（它防的是**跨轮累积**，只有每轮都跑才有意义）。
#
# 用法：tests/perturb/gate-orphans.sh
#   EXTC=…      换编译器（默认 build/extc）
#   CENSUS=…    换普查脚本（牙口用：副本里把"杀进程组"打回"只杀编译器"）
#   FILTER=…    语料过滤（默认 coro/echo_server，两个都会挂）
#   RUN_ROOT=…  本次普查的私有 scratch 根（默认 /tmp/extc-perturb-orphans-gate）
#   TIMEOUT_S=… 单例超时秒数（默认 2）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
EXTC=${EXTC:-$ROOT/build/extc}
CENSUS=${CENSUS:-$ROOT/tests/perturb/census.py}
FILTER=${FILTER:-coro/echo_server}
TIMEOUT_S=${TIMEOUT_S:-2}
JOBS=${JOBS:-2}
# 这次普查用**私有** scratch 根（`census.py` 的 `EXTC_PERTURB_RUN_ROOT`）：判据的前缀就是它。
# 好处：与别的普查（bench 档 `gate-perturb-corpus` 过全语料）互不干扰 —— 否则并发时"跑之前必须
# 0 进程"会被邻居的正常运行踩红（假红会让门禁退化成噪声）。
RUN_ROOT=${RUN_ROOT:-/tmp/extc-perturb-orphans-gate}
export EXTC_PERTURB_RUN_ROOT="$RUN_ROOT"
PREFIX="^${RUN_ROOT}/"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== 扰动普查的进程卫生（有界子集，含超时用例；跑完 $RUN_ROOT/ 下必须 0 进程）=="
echo "   编译器=$EXTC  普查=$CENSUS  过滤=$FILTER  timeout=${TIMEOUT_S}s  jobs=$JOBS"

fail=0
before=$(pgrep -cf "$PREFIX" 2>/dev/null || true)
[ -n "$before" ] || before=0
if [ "$before" != 0 ]; then
    bad "跑之前 $RUN_ROOT/ 下就有 $before 个进程 —— 本判据认不出哪些是自己的后代，先清干净再跑"
    pgrep -af "$PREFIX" | head -5 | sed 's/^/      /'
    exit 1
fi
ok "跑之前 0 个残留（判据可以干净地归因）"

timeout $((TIMEOUT_S * 40 + 120)) python3 "$CENSUS" --extc "$EXTC" \
        --filter "$FILTER" --arms plain --timeout "$TIMEOUT_S" --jobs "$JOBS" \
        --out "$TMP/out" >"$TMP/census.log" 2>&1
crc=$?

timeouts=$(sed -n 's/.*超时被杀（整个进程组）: \([0-9][0-9]*\) 次.*/\1/p' "$TMP/census.log" | tail -1)
[ -n "$timeouts" ] || timeouts=0

if [ "$crc" != 0 ]; then
    bad "普查本身非零退出（rc=$crc）"; tail -6 "$TMP/census.log" | sed 's/^/      /'; fail=1
fi
if [ "$timeouts" -lt 1 ]; then
    bad "反空跑：这一轮 0 次超时 ⇒ 没走到「杀进程组」那条路，判据此刻没有牙（过滤词/超时值是不是选错了）"
    tail -6 "$TMP/census.log" | sed 's/^/      /'
    fail=1
fi

after=$(pgrep -cf "$PREFIX" 2>/dev/null || true)
[ -n "$after" ] || after=0
if [ "$after" = 0 ]; then
    ok "跑完 0 个残留：超时把那整个进程组（编译器 + 被编译程序及后代）收干净了（本轮超时 $timeouts 次）"
else
    bad "$after 个进程活了下来（超时只杀了编译器？）—— 它们是孤儿，跨轮会累积"
    # 只收**我们自己**刚起的：① 跑之前已确认是 0；② 前缀就是普查的私有 scratch 根。
    # 用 pid 列表（不用 pkill/模式），避免匹配到本命令自己的命令行。
    pids=$(pgrep -f "$PREFIX" 2>/dev/null || true)
    shown=0
    for p in $pids; do
        if [ "$shown" -lt 8 ]; then
            printf '      %s %s\n' "$p" "$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)"
            shown=$((shown + 1))
        fi
    done
    [ "$after" -le 8 ] || echo "      …（共 $after 个）"
    for p in $pids; do kill -9 "$p" 2>/dev/null || true; done
    echo "      （已按 pid 收掉这 $after 个：跑前为 0 ⇒ 只可能是本次普查留下的）"
    fail=1
fi

exit "$fail"
