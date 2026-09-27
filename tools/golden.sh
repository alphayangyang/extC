#!/usr/bin/env bash
# 生成物黄金闸门：① 逐字节比对 ② 生成物必须是合法 C（gcc -fsyntax-only）
#
# 为什么要有第 ② 条：2026-09-28 发现基准里冻着一份**非法 C**（examples/prelude.extc），而当时
# 的闸门只比哈希 ⇒ 坏产物会被静静冻进基准，谁也不会发现。"逐字节不变"只有建立在**产物本身正确**
# 的前提上才有意义。已知坏的那几条列在 tools/golden-known-bad.txt，修好就从那里删掉。
#
# 用法：EXTC=<编译器> tools/golden.sh        （EXTC 默认 build/extc）
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
EXTC="${EXTC:-$root/build/extc}"
MAN="$here/golden.sha256"
KNOWN="$here/golden-known-bad.txt"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

byte_bad=0; syn_bad=0; known=0; n=0
while read -r h f; do
    [ -z "${f:-}" ] && continue
    n=$((n + 1))
    if ! "$EXTC" -w --no-line-map -o "$tmp/out.c" "$f" 2>"$tmp/e0"; then
        echo "  ✗ 编译失败 $f: $(head -1 "$tmp/e0")"; byte_bad=$((byte_bad + 1)); continue
    fi
    got=$(sha256sum "$tmp/out.c" | cut -d' ' -f1)
    [ "$got" != "$h" ] && { echo "  ✗ 输出不同 $f"; byte_bad=$((byte_bad + 1)); }
    if ! gcc -std=c11 -fsyntax-only "$tmp/out.c" 2>"$tmp/e1"; then
        syn_bad=$((syn_bad + 1))
        if grep -qxF "$f" "$KNOWN"; then known=$((known + 1))
        else echo "  ✗ 生成物不是合法 C: $f"; echo "      $(head -1 "$tmp/e1")"; fi
    fi
done < "$MAN"

printf '  %s %d 个生成物：逐字节不符 %d · 非法 C %d（已知 %d）\n' \
       "$([ $byte_bad -eq 0 ] && [ $syn_bad -eq $known ] && echo ok || echo FAIL)" \
       "$n" "$byte_bad" "$syn_bad" "$known"
[ "$byte_bad" -eq 0 ] && [ "$syn_bad" -eq "$known" ]
