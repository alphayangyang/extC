#!/usr/bin/env bash
# tools/install-vscode-ext.sh —— 把 `tools/vscode-extc/` 装成 VS Code 扩展（本机 + Remote-WSL 两侧）。
#
# 为什么两侧都装：VS Code 的扩展宿主跟着窗口走。用 Remote-WSL 打开的窗口跑的是 **Linux 侧**
# 的扩展宿主，只认 `~/.vscode-server/extensions/`；普通 Windows 窗口认 `%USERPROFILE%\.vscode\extensions`。
# 只装一侧的现象就是"装了但没反应"。
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/tools/vscode-extc"
NAME="extc.extc-lsp-0.1.0"

install_into() {                 # install_into <extensions 目录>
    local dir="$1"
    [ -d "$dir" ] || { echo "  skip  $dir（不存在）"; return 0; }
    rm -rf "$dir/$NAME"
    mkdir -p "$dir/$NAME"
    cp -r "$SRC"/. "$dir/$NAME"/
    echo "  ok    $dir/$NAME"
}

# WSL / Linux 侧的扩展宿主（Remote-WSL 窗口用它）。
install_into "$HOME/.vscode-server/extensions"

# Windows 侧的扩展目录（从 WSL 里能看见就装）。
for win in /mnt/c/Users/*/.vscode/extensions; do
    [ -d "$win" ] && install_into "$win"
done

echo "装好了；在 VS Code 里 Developer: Reload Window，然后输出面板选 extC 看客户端日志。"
