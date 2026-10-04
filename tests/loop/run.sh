#!/usr/bin/env bash
# tests/loop/run.sh —— 定时策略（deadline 表 + epoll_wait 超时计算）的离线判据。
set -u
cd "$(dirname "$0")/../.."
if out=$(./build/extc -w --run tests/loop/main.extc 2>&1) && echo "$out" | grep -q 'loop ok'; then
    echo "  ok   timers   ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL timers   ->  $(echo "$out" | tail -3 | tr '\n' '|')"; exit 1
fi
