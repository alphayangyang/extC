#!/usr/bin/env bash
# bench/gc/pause_dist.sh —— 停顿探针的**薄包装**（task-74）。
# 目的：产物**没有 rc 运行期**时（结构性无停顿，如 P1/P3），直接报"结构性 0"并 exit 0，
# 而不是把 tools/rc-pause-probe.py 的"找不到回收运行期"当成假 FAIL（那会让调用方看到
# `exits=0/0/1/1` + `cmp=DIFF`）。
# 用法：pause_dist.sh <生成的 C> <输出插桩 C>
#   rc 记号数 = 0  ⇒ stderr 打 `PAUSE_DIST_STRUCTURAL0 file=… rc_marks=0`，**exit 0**，不产出插桩文件
#   rc 记号数 > 0  ⇒ 只读复用 tools/rc-pause-probe.py（本文件不改它）；成功 exit 0，失败透传其 exit code
set -u
SRC=${1:?用法: pause_dist.sh <gen.c> <out.probe.c>}
DST=${2:?用法: pause_dist.sh <gen.c> <out.probe.c>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ ! -f "$SRC" ]; then echo "pause_dist: 输入不存在 $SRC" >&2; exit 2; fi
marks=$(grep -c 'extc_rc_' "$SRC" || true)
if [ "${marks:-0}" -eq 0 ]; then
    echo "PAUSE_DIST_STRUCTURAL0 file=$SRC rc_marks=0 (产物无 rc 运行期 ⇒ 结构性无停顿，非 FAIL)" >&2
    exit 0
fi
python3 "$ROOT/tools/rc-pause-probe.py" "$SRC" > "$DST"
rc=$?
if [ $rc -ne 0 ]; then echo "pause_dist: 探针插桩失败（rc=$rc，rc_marks=$marks）" >&2; exit $rc; fi
echo "PAUSE_DIST_INSTRUMENTED file=$SRC rc_marks=$marks out=$DST" >&2
exit 0
