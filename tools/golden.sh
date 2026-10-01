#!/usr/bin/env bash
# 生成物**快照观察**（原"黄金闸门"，2026-09-30 按所有者决定降级 —— 见 docs/DECISIONS.md 定案 98）。
#
# ⚠️ 逐字节比对**不再是判据**，默认只作为观察输出。理由（所有者原话）：
#    「golden 应该降级，因为现在需要修理，代码相同不保证正确。」
# 把这个工具当闸门会得出两个错误结论：产物**改对了**是红、产物**错着不动**是绿。修复期尤其致命——
# 这批 P0/P1/P2 的修复恰恰**要求**生成物变化（浮点字面量补 `.0`、窄类型回截、`for` 步进标签、
# 空视图保护、循环条件里的临时量）。真正管"生成物能不能用"的判据是 `tools/gate_checkc.py`
# （全语料 gcc+clang `-c -O2`）与 `tools/gate_asan_corpus.py`（ASan+UBSan 真跑），
# 它们已经进 `check.sh`。
#
# 本脚本仍然有用的两件事：
#   · 观察：相对上一次冻结的哈希，**有多少产物变了**（重构时看影响面）；
#   · 判据：`gcc -fsyntax-only` —— 生成物必须是合法 C（历史上真冻进过一份非法 C：
#     examples/prelude.extc，2026-09-28）。这一条在任何模式下都是红的。
#
# 用法：
#   EXTC=<编译器> tools/golden.sh              # 观察 + 合法性判据（默认）
#   EXTC=<编译器> tools/golden.sh --strict-bytes   # 旧行为：逐字节也判红（仅在你想钉住实现时用）
#   tools/golden.sh --freeze                   # 用当前编译器重冻 tools/golden.sha256
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
EXTC="${EXTC:-$root/build/extc}"
MAN="$here/golden.sha256"
KNOWN="$here/golden-known-bad.txt"
STRICT=0
FREEZE=0
for a in "$@"; do
    case "$a" in
        --strict-bytes) STRICT=1 ;;
        --freeze)       FREEZE=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

if [ "$FREEZE" -eq 1 ]; then
    # 重冻：哈希清单是"上一次实现"的快照。只在**有意识地**接受当前产物时执行。
    while read -r h f; do
        [ -z "${f:-}" ] && continue
        "$EXTC" -w --no-line-map -o "$tmp/out.c" "$f" 2>/dev/null || { echo "  编译失败，跳过 $f" >&2; continue; }
        printf '%s %s\n' "$(sha256sum "$tmp/out.c" | cut -d' ' -f1)" "$f"
    done < "$MAN" > "$tmp/new.sha256"
    cp "$tmp/new.sha256" "$MAN"
    echo "  已重冻 $(wc -l < "$MAN") 条哈希（$MAN）"
    exit 0
fi

byte_bad=0; syn_bad=0; known=0; n=0
: > "$tmp/changed"
while read -r h f; do
    [ -z "${f:-}" ] && continue
    n=$((n + 1))
    if ! "$EXTC" -w --no-line-map -o "$tmp/out.c" "$f" 2>"$tmp/e0"; then
        echo "  ✗ 编译失败 $f: $(head -1 "$tmp/e0")"; byte_bad=$((byte_bad + 1)); continue
    fi
    got=$(sha256sum "$tmp/out.c" | cut -d' ' -f1)
    [ "$got" != "$h" ] && { byte_bad=$((byte_bad + 1)); echo "$f" >> "$tmp/changed"; }
    if ! gcc -std=c11 -fsyntax-only "$tmp/out.c" 2>"$tmp/e1"; then
        syn_bad=$((syn_bad + 1))
        if grep -qxF "$f" "$KNOWN"; then known=$((known + 1))
        else echo "  ✗ 生成物不是合法 C: $f"; echo "      $(head -1 "$tmp/e1")"; fi
    fi
done < "$MAN"

# 变化面：按目录聚合，便于一眼看出影响范围（观察用，不影响退出码）。
if [ "$byte_bad" -gt 0 ]; then
    echo "  · 产物相对快照有变化（观察项，不是失败）：$byte_bad/$n 份"
    awk -F/ '{ if (NF>1) print $1"/"$2; else print $1 }' "$tmp/changed" | sort | uniq -c | sort -rn | head -6 | sed 's/^/      /'
    if [ "$STRICT" -eq 1 ]; then
        echo "      （--strict-bytes：逐字节不同**判红**，仅用于钉住实现的场合）"
        sed 's/^/      ✗ /' "$tmp/changed" | head -20
    fi
fi

# 判据只有一条：生成物必须是合法 C。
if [ "$syn_bad" -eq "$known" ]; then
    printf '  ok   %d 个生成物：全部是合法 C%s\n' "$n" \
        "$([ "$byte_bad" -gt 0 ] && echo "（其中 $byte_bad 份与冻结快照不同 —— 观察项）" || echo "（与快照逐字节一致）")"
    [ "$STRICT" -eq 0 ] || [ "$byte_bad" -eq 0 ]
else
    printf '  FAIL %d 个生成物：非法 C %d（已知 %d）· 与快照不同 %d（观察项）\n' \
        "$n" "$syn_bad" "$known" "$byte_bad"
    exit 1
fi
