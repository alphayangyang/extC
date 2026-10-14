#!/usr/bin/env bash
# extC 全套自检 —— 编排层已搬到 Python：tools/check.py（检查项是数据，可选项 / 计时 / 分档）
#   用法： ./check.sh            （全部，含基准）
#          ./check.sh smoke      （快速高信号集，≈10s —— 改完代码第一件事跑这个）
#          ./check.sh nobench    （非基准全量，≈220s；CI 跑这档）
#          ./check.sh docs       （只跑文档/工具项，秒级）
#          ./check.sh --list     （看项名 / 组 / 最近耗时）
# `quick` 是 `nobench` 的历史别名（它**不是**"快"的意思：80 项、≈220s），保留只为兼容
# README/CI；跑它会自己提示一句。
set -u
cd "$(dirname "$0")"
exec python3 tools/check.py "$@"
