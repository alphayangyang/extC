#!/usr/bin/env bash
# extC 全套自检 —— 编排层已搬到 Python：tools/check.py（检查项是数据，可选项 / 计时 / 分档）
#   用法： ./check.sh          （全部，含基准）
#          ./check.sh quick    （只跑测试 + 攻击库，跳过基准）
#          ./check.sh docs     （只跑文档/工具项，秒级）
#          ./check.sh --list   （看项名 / 组 / 最近耗时）
# 这个 shim 保住 README / CI 里的 `./check.sh`、`./check.sh quick`：用法与语义不变。
set -u
cd "$(dirname "$0")"
exec python3 tools/check.py "$@"
