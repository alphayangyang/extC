#!/bin/sh
# tests/tui/run.sh —— std::tui 的行为用例（真 PTY；没有终端就跳过，不算失败）。
#
# 单独跑：tests/tui/run.sh        （EXTC=./build/extc 默认）
set -u
cd "$(dirname "$0")/../.." || exit 1
: "${EXTC:=./build/extc}"
export EXTC
if [ ! -x "$EXTC" ]; then
    echo "  --   跳过：$EXTC 不存在"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  --   跳过：没有 python3"
    exit 0
fi
if ! python3 -c 'import pty, fcntl, termios, struct' 2>/dev/null; then
    echo "  --   跳过：python3 没有 pty/fcntl/termios"
    exit 0
fi
python3 tests/tui/pty_run.py
