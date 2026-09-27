#!/usr/bin/env bash
# 清掉 fuzz/探针留在 /tmp 的产物，把 tmpfs 的空间还回去。
#
# 为什么需要它：/tmp 在 WSL 里通常是 tmpfs（内存盘），几轮战役（每轮几百到几千次迭代，
# 每次都要落一份 case.extc / case.c / ASan 可执行文件）就能把它写满；写满之后连 bash
# 都起不来 —— 它的暂存也在 /tmp，于是"想删东西却需要 shell"卡死。2026-09-28 撞过一次，
# 记录见 docs/topics/HARDENING.md 第四节。
#
# 用法：
#     tools/clean-tmp.sh            # 只删下面列出的产物
#     tools/clean-tmp.sh --dry-run  # 只打印会删什么
#
# 只碰 /tmp 下这些前缀，绝不进仓库。

set -u
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

TARGETS=(
    /tmp/fuzz*            # 各轮战役的产物（失败用例、日志）
    /tmp/extc-fuzz*
    /tmp/refC /tmp/refC_new   # 生成物参考副本（基准在仓库 tools/golden.sha256 里，这里只是 diff 用）
    /tmp/wt_*             # 隔离构建用的 worktree 副本（含各自的 build/）
    /tmp/extc-fuzz-*
    /tmp/det_*.c /tmp/gm*.c /tmp/re_*.c /tmp/new.c /tmp/pp*.c /tmp/pre*.c /tmp/tr*.c
    /tmp/ub_fail* /tmp/*.extc /tmp/core /tmp/core.*
)

if [ "$DRY" = 1 ]; then
    echo "会删除（dry-run）："
    for t in "${TARGETS[@]}"; do
        for f in $t; do [ -e "$f" ] && du -sh "$f" 2>/dev/null; done
    done
    exit 0
fi

freed=0
for t in "${TARGETS[@]}"; do
    for f in $t; do
        [ -e "$f" ] || continue
        sz=$(du -sk "$f" 2>/dev/null | cut -f1); sz=${sz:-0}
        rm -rf -- "$f" && freed=$((freed + sz))
    done
done
echo "已释放约 $((freed / 1024)) MB"
df -h /tmp 2>/dev/null | tail -1
