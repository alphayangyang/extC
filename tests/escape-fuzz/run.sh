#!/usr/bin/env bash
# tests/escape-fuzz/run.sh —— 「new 像 C++ 堆」误拒 fuzz 的基线跑法。
#
# 只读 cases/ 里提交好的语料（改语料跑 gen.py），逐个：
#   extC 编过 ⇒ gcc -O1 -fsanitize=address 编 + 跑 + 比输出
#   extC 被拒 ⇒ 抓完整报错、按判据聚类
#   C++ 对照  ⇒ g++ -O1 编 + 跑
# 输出一张分类表。基线性质：exit 0（语法/类型错会显式列出，便于清零）。
#
# 用法：bash tests/escape-fuzz/run.sh [--jobs N]
set -u
cd "$(dirname "$0")/../.."          # 仓库根，与 tests/arena*/run.sh 同约定
export EXTC=${EXTC:-./build/extc}
export CC=${CC:-cc}
export CXX=${CXX:-g++}
JOBS=8
if [ "${1:-}" = "--jobs" ] && [ -n "${2:-}" ]; then JOBS=$2; fi
export JOBS
exec python3 tests/escape-fuzz/harness.py
