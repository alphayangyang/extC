#!/usr/bin/env bash
# tests/reclaim-perf/ablate-tax.sh —— **回收税的消融尺子**（验收用）。
#
# 用途：任何"某个编译期规则能把回收开销压到多少"的主张，都用这条命令在**同一把尺子**上复量。
# 口径与 `gate-tax.sh` 一致：税 = 开档 Ir / 关档 Ir（1.00× = 完全不花钱）。
#
# ⚠️ 消融改的是 `/tmp` 里的生成 C 副本，**语义已经变了** ⇒ 只能当**上界**读，不能当收益承诺。
#    它回答的是"如果这块计数不花，税会变成多少"，不是"这样改是对的"。
#
# 消融：
#   none     原样（对照）
#   initref  去掉 `extc_rc_initRef` 里那一行 `extc_rc_inc(v)`（= 局部引用拷贝的计数）
#   store    去掉 `extc_rc_storeRefOwned` 里新值那一侧的 `holdUpdSlab(...,+1)`（= 字段赋值的计数）
#   all      两者都去掉
#
# 用法：tests/reclaim-perf/ablate-tax.sh [形状…] [消融…]
#   形状：bt（默认）rebuild cyc
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
BIG=$ROOT/bench/bigmatrix
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

SHAPES=${*:-bt}
want_shapes=(); want_abl=()
for a in "$@"; do case "$a" in bt|rebuild|cyc) want_shapes+=("$a");; *) want_abl+=("$a");; esac; done
[ ${#want_shapes[@]} -eq 0 ] && want_shapes=(bt)
[ ${#want_abl[@]} -eq 0 ] && want_abl=(none initref store all)

meta() { # <形状> → 源 / 输入 / 编译档 / 参数
    case "$1" in
        bt)      SRC="$BIG/src/bt.extc";        INP="$BIG/in-small/bt.txt";      FLAGS="-O3 -march=native";  ARGS="";;
        rebuild) SRC="$BIG/src/rebuild.extc";   INP="$BIG/in-small/rebuild.txt"; FLAGS="-O3 -march=native";  ARGS="";;
        cyc)     SRC="$ROOT/tests/reclaim-shapes/cyclic_pressure.extc"; INP=""; FLAGS="-O1";               ARGS="200000";;
        *) echo "未知形状：$1" >&2; exit 2;;
    esac
}

gen() { # <标签> <开档?> → 生成并编译，回显可执行路径
    local tag=$1 on=$2 rc=0; [ "$on" = 1 ] && rc=1
    EXTC_RECLAIM=$rc "$EXTC" -I "$ROOT/stdlib" "$SRC" -o "$TMP/$tag.c" >/dev/null 2>&1 || return 1
    # shellcheck disable=SC2086
    $CC $FLAGS -fwrapv -o "$TMP/$tag" "$TMP/$tag.c" >/dev/null 2>&1 || return 1
    echo "$TMP/$tag"
}

ir_of() { # <可执行> → Ir
    local exe=$1
    local cg=$TMP/$(basename "$exe").cg
    # 超时保护：消融改的是语义，被改坏的程序可能死循环（此时打印 TIMEOUT 而不是挂住）。
    if [ -n "$INP" ]; then
        timeout 300 valgrind --tool=callgrind --callgrind-out-file="$cg" "$exe" $ARGS < "$INP" >/dev/null 2>&1
    else
        timeout 300 valgrind --tool=callgrind --callgrind-out-file="$cg" "$exe" $ARGS >/dev/null 2>&1
    fi
    case $? in
        0) ;;
        124) echo TIMEOUT; return;;
        *) echo ERR; return;;
    esac
    grep '^summary:' "$cg" | awk '{print $2}'
}

ablate() { # <开档 C 路径> <消融名> → 打补丁后的 C 路径
    local src=$1 how=$2
    local dst=$TMP/$(basename "$src" .c).$how.c
    python3 - "$src" "$dst" "$how" <<'PY'
import re, sys, pathlib
src, dst, how = sys.argv[1], sys.argv[2], sys.argv[3]
s = pathlib.Path(src).read_text()
if how in ("initref", "all"):
    s2, n = re.subn(r"(static void extc_rc_initRef\(void \*\*slot, void \*v\) \{\n)(.*?)(\n\})",
                    lambda m: m.group(1) + m.group(2).replace("    extc_rc_inc(v);", "    /* 消融 */").replace("    extc_rc_inc(v);\n", "    /* 消融 */\n") + m.group(3),
                    s, count=1, flags=re.S)
    assert n == 1, "initRef 体没找到"
    s = s2
if how in ("store", "all"):
    s2 = s.replace("        if (sv)   extc_rc_holdUpdSlab(sv, v, +1);\n",
                   "        /* 消融：新值那一侧的 +1 */\n", 1)
    if s2 == s:
        # 发射形状可能变过：退一步找带 sv 的 +1 行
        s2, n = re.subn(r"^ *if \(sv\).*holdUpdSlab\(sv, v, \+1\);.*$", "        /* 消融 */", s, count=1, flags=re.M)
        assert n == 1, "storeRefOwned 新值 +1 没找到"
    s = s2
pathlib.Path(dst).write_text(s)
PY
    [ -f "$dst" ] && echo "$dst"
}

printf '%-8s %-8s %12s %12s %12s %9s %9s\n' 形状 消融 开档Ir 消融后Ir 关档Ir 税 回收占比
for shape in "${want_shapes[@]}"; do
    meta "$shape"
    on=$(gen "$shape.on" 1) || { echo "$shape：开档生成/编译失败" >&2; continue; }
    off=$(gen "$shape.off" 0) || { echo "$shape：关档生成/编译失败" >&2; continue; }
    ON=$(ir_of "$on"); OFF=$(ir_of "$off")
    for how in "${want_abl[@]}"; do
        if [ "$how" = none ]; then AB=$ON; else
            pc=$(ablate "$TMP/$shape.on.c" "$how") || { echo "$shape/$how：打补丁失败" >&2; continue; }
            # shellcheck disable=SC2086
            $CC $FLAGS -fwrapv -o "$TMP/$shape.$how" "$pc" >/dev/null 2>&1 || { echo "$shape/$how：编译失败" >&2; continue; }
            AB=$(ir_of "$TMP/$shape.$how")
        fi
        awk -v s="$shape" -v h="$how" -v on="$ON" -v ab="$AB" -v off="$OFF" \
            'BEGIN{printf "%-8s %-8s %12d %12d %12d %8.3f× %8.1f%%\n", s, h, on, ab, off, ab/off, 100*(ab-off)/ab}'
    done
done
echo
echo "（税 = 消融后 Ir / 关档 Ir；1.00× = 这笔开销完全不花。消融语义已变 ⇒ 只当上界读。）"
