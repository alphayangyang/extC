#!/usr/bin/env bash
# tests/gomoku -- examples/gomoku 这个多模块项目按它自称的方式跑。
#
# 判据：用法列全四种模式；未知参数退出码 2；自对弈两次逐字节相同且真的下出结果；
# 人机模式吃得下记谱、非法输入给了提示、AI 真的应手；bench 报出的 nps 是数字且
# 真的搜了节点；全屏前端在真 PTY 里落子/悔棋/退出、退出后终端复原（没有 pty 就跳过）。
set -u
cd "$(dirname "$0")/../.." || exit 1
fail=0
ok()  { printf '  ok %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }
EXTC=${EXTC:-./build/extc}
APP=examples/gomoku

if [ ! -x "$EXTC" ]; then
    echo "  --   跳过：$EXTC 不存在"
    exit 0
fi

help=$("$EXTC" run "$APP" -- --help 2>&1)
if printf '%s\n' "$help" | grep -q -- '--selfplay' &&
   printf '%s\n' "$help" | grep -q -- '--bench' &&
   printf '%s\n' "$help" | grep -q -- '--tui'; then
    ok "--help 列出 selfplay/bench/tui"
else
    bad "--help 没列全模式"
fi

"$EXTC" run "$APP" -- --bogus >/dev/null 2>&1
rc=$?
[ "$rc" = 2 ] && ok "未知参数退出码 2" || bad "未知参数退出码 $rc，期望 2"

sp1=$("$EXTC" run "$APP" -- --selfplay 2>&1)
sp2=$("$EXTC" run "$APP" -- --selfplay 2>&1)
[ "$sp1" = "$sp2" ] && ok "自对弈两次逐字节相同" || bad "自对弈不确定"
printf '%s\n' "$sp1" | grep -q '^result: ' && ok "自对弈下出了结果" || bad "自对弈没有 result 行"
moves=$(printf '%s\n' "$sp1" | grep -o 'moves=[0-9]*' | head -1 | cut -d= -f2)
if [ -n "$moves" ] && [ "$moves" -ge 5 ]; then ok "自对弈手数 $moves"; else bad "自对弈手数异常：$moves"; fi

human=$(printf 'H8\nZZ\nq\n' | "$EXTC" run "$APP" 2>&1)
rc=$?
[ "$rc" = 0 ] && ok "人机模式 q 退出码 0" || bad "人机模式退出码 $rc"
printf '%s\n' "$human" | grep -q 'cannot parse that move' && ok "非法记谱有提示" || bad "非法记谱没有提示"
printf '%s\n' "$human" | grep -q 'ai thinking' && ok "AI 回合有提示" || bad "AI 回合没有提示"
printf '%s\n' "$human" | grep -q 'white: ' && ok "AI 真的应手了" || bad "AI 没有应手"
printf '%s\n' "$human" | grep -q '^ 8 .* x\|^ 8 .* X' && ok "人手的子落在棋盘上" || bad "人手的子没落在棋盘上"

bench=$("$EXTC" run "$APP" -- --bench --depth 4 --runs 2 2>&1)
printf '%s\n' "$bench" | grep -q '^bench depth=4 runs=2 ' && ok "bench 报了深度与轮数" || bad "bench 输出不对"
nodes=$(printf '%s\n' "$bench" | grep -o 'nodes=[0-9]*' | head -1 | cut -d= -f2)
if [ -n "$nodes" ] && [ "$nodes" -gt 100 ]; then ok "bench 搜了 $nodes 个节点"; else bad "bench 节点数异常：$nodes"; fi
nps=$(printf '%s\n' "$bench" | grep -o 'nps=[0-9]*' | head -1 | cut -d= -f2)
if [ -n "$nps" ] && [ "$nps" -gt 10000 ]; then ok "bench nps=$nps"; else bad "bench nps 异常：$nps"; fi

if command -v python3 >/dev/null 2>&1 && python3 -c 'import pty, fcntl, termios, struct' 2>/dev/null; then
    if out=$(python3 tests/gomoku/pty_run.py 2>&1); then
        printf '%s\n' "$out" | grep -E '^  ok |^  FAIL '
        printf '%s\n' "$out" | grep -q '^  ok ' && ok "全屏前端（真 PTY）" || bad "全屏前端用例没跑"
    else
        printf '%s\n' "$out"
        bad "全屏前端用例"
    fi
else
    printf '  --   跳过：没有 python3/pty\n'
fi

if [ "$fail" = 0 ]; then printf '\ngomoku: 通过\n'; else printf '\ngomoku: 失败 %d\n' "$fail"; fi
exit $((fail > 0))
