#!/usr/bin/env bash
# gen-corpus.sh —— 把一棵语料树里的每个可编译 `.extc` 生成成 C，落到一个输出目录。
#
# 用途：给 cost-model.py / scan-triggers.py 提供"生成物"这一层的输入。
# 只读语料，只写输出目录（默认 /tmp）；不碰仓库里的任何文件。
#
# 用法：
#   tests/reclaim-rootstack/gen-corpus.sh <extc> <root> <outdir> [语料目录...]
# 例：
#   tests/reclaim-rootstack/gen-corpus.sh /tmp/rs-prefix/build/extc . /tmp/rs-gen tests bench
set -u
EXTC=${1:?usage: gen-corpus.sh <extc> <root> <outdir> [dirs...]}
ROOT=${2:?}
OUT=${3:?}
shift 3
DIRS=${*:-tests bench examples stdlib}

mkdir -p "$OUT"
n=0; ok=0
while IFS= read -r src; do
    n=$((n + 1))
    rel=${src#"$ROOT"/}
    dst="$OUT/${rel//\//__}.c"
    if EXTC_RECLAIM=1 "$EXTC" -I "$ROOT/stdlib" "$src" -o "$dst" >/dev/null 2>&1; then
        ok=$((ok + 1))
    else
        rm -f "$dst"
    fi
done < <(for d in $DIRS; do find "$ROOT/$d" -name '*.extc' 2>/dev/null; done | sort)
echo "生成 $ok/$n → $OUT"
