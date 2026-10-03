#!/usr/bin/env bash
# 链接通道（driver 的 -l / -L / --ccflag / --pkg-config）：**真的把库链上**。
#
# 为什么单开一套：在它之前，extern!("lib") 里的库名只用于诊断，消费第三方 C 库只有
# dlopen 一条路（tests/real-lib 就是那么画的）。这套钉住四件事：
#   ① 三条给 flag 的路（-l / 原生 flag / pkg-config）都能编译 + 链接 + 真跑出结果；
#   ② **flag 是承重的**：不给 flag 就链不上（不是"碰巧能过"）；
#   ③ 链接需求写进生成物（extc-libs: / extc-pkg-config:），构建系统能读回去；
#   ④ **不带 flag 时产物一个字都不变**（否则 414 份快照会被无声改掉）。
#
# 库不在本机的条目**显式跳过**（打印原因），不静默通过 —— 与 tests/real-lib 同一口径。
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
EXTC="${EXTC:-$root/build/extc}"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0; skip=0
ok()    { pass=$((pass+1)); printf '  ok   %-22s %s\n' "$1" "$2"; }
bad()   { fail=$((fail+1)); printf '  FAIL %-22s %s\n' "$1" "$2"; }
skipf() { skip=$((skip+1)); printf '  跳过 %-22s %s\n' "$1" "$2"; }
src()   { printf '%s/%s' "$here" "$1"; }

# `--run` 会在 **CWD** 下建 build/ 并把二进制写在那里；套件整体进临时目录跑，
# 既不会往仓库的 build/ 里丢东西，也不会撞上仓库里已有的目录（`build/app/` 就撞过：
# 链接器报 "cannot open output file build/app: Is a directory"）。
in_tmp() { ( cd "$tmp" && "$@" ); }

# 生成一次 C（不带 --run），把 stderr 留在 $tmp/e 里供诊断使用。
gen() { local s="$1" out="$2"; shift 2; "$EXTC" -w --no-line-map "$@" -o "$out" "$s" 2>"$tmp/e"; }

echo "== ① 生成物里的链接需求（构建系统读得回去）=="

# 不带任何 flag：**不能**出现链接需求行（这条保住 414 份快照逐字节不变）。
if gen "$(src zlib_version.extc)" "$tmp/plain.c" && ! grep -q 'extc-libs:' "$tmp/plain.c"; then
    ok no-flag-no-comment "不给 flag ⇒ 不写链接需求行（产物与从前逐字节相同）"
else
    bad no-flag-no-comment "$(head -1 "$tmp/e")"
fi

# 多个 -l：排序 + 去重（同一组库、书写顺序不同 ⇒ 产物必须一样）。
if gen "$(src zlib_version.extc)" "$tmp/multi.c" -l z -l ssl -l z \
   && [ "$(head -1 "$tmp/multi.c")" = "/* extc-libs: ssl z */" ]; then
    ok comment-sorted-dedup "-l z -l ssl -l z ⇒ /* extc-libs: ssl z */"
else
    bad comment-sorted-dedup "$(head -1 "$tmp/multi.c" 2>/dev/null || head -1 "$tmp/e")"
fi

if gen "$(src openssl_version.extc)" "$tmp/pkg.c" --pkg-config openssl \
   && [ "$(head -1 "$tmp/pkg.c")" = "/* extc-pkg-config: openssl */" ]; then
    ok comment-pkg-config "--pkg-config openssl ⇒ /* extc-pkg-config: openssl */"
else
    bad comment-pkg-config "$(head -1 "$tmp/pkg.c" 2>/dev/null || head -1 "$tmp/e")"
fi

echo "== ② 三条路都能真跑（缺库则显式跳过）=="

# -l z：需要 dev 软链 libz.so（本机有；没有就跳过）。
if [ -e /usr/lib/x86_64-linux-gnu/libz.so ] || pkg-config --exists zlib 2>/dev/null; then
    if in_tmp "$EXTC" -w -l z --run "$(src zlib_version.extc)" >"$tmp/o" 2>"$tmp/e"; then
        ok run-lib "-l z + zlibVersion() ⇒ 链上并跑通（rc=0）"
    else
        bad run-lib "$(head -1 "$tmp/e")"
    fi
    # 承重：同一个程序不给 flag 必须**失败**（证明前面那条不是碰巧）。
    if in_tmp "$EXTC" -w --run "$(src zlib_version.extc)" >"$tmp/o" 2>"$tmp/e"; then
        bad flag-is-load-bearing "不给 -l z 也过了 ⇒ 这条判据不承重（本机 cc 默认链 zlib？）"
    else
        ok flag-is-load-bearing "不给 -l z ⇒ 链不上（flag 是承重的）"
    fi
else
    skipf run-lib "本机没有 libz.so 开发软链"
    skipf flag-is-load-bearing "同上"
fi

# --ccflag -l:libsqlite3.so.0：本机 sqlite3 **没有 dev 包**，只有 soname。
if ldconfig -p 2>/dev/null | grep -q 'libsqlite3\.so\.0'; then
    if in_tmp "$EXTC" -w --ccflag -l:libsqlite3.so.0 --run "$(src sqlite_version.extc)" >"$tmp/o" 2>"$tmp/e"; then
        ok run-ccflag-soname "--ccflag -l:libsqlite3.so.0 ⇒ 无头无 .pc 也能链上并跑通"
    else
        bad run-ccflag-soname "$(head -1 "$tmp/e")"
    fi
else
    skipf run-ccflag-soname "本机没有 libsqlite3.so.0"
fi

# --pkg-config openssl：同时验证 cflags 进了命令行（否则 -fsyntax-only 会找不到头）。
if pkg-config --exists openssl 2>/dev/null; then
    if in_tmp "$EXTC" -w --pkg-config openssl --run "$(src openssl_version.extc)" >"$tmp/o" 2>"$tmp/e"; then
        ok run-pkg-config "--pkg-config openssl ⇒ 链上并跑通"
    else
        bad run-pkg-config "$(head -1 "$tmp/e")"
    fi
    if "$EXTC" -w --pkg-config openssl --check-c -o /dev/null "$(src openssl_version.extc)" 2>"$tmp/e"; then
        ok check-c-with-flags "--check-c 也吃这些 flag（头文件可寻址）"
    else
        bad check-c-with-flags "$(head -1 "$tmp/e")"
    fi
else
    skipf run-pkg-config "本机没有 openssl 的 .pc"
    skipf check-c-with-flags "同上"
fi

echo "== ③ 模块自带的链接需求（module.link：use 到了就自动链，没 use 就不链）=="

# `use zmod` + zmod.link 里的 `lib z` ⇒ 不给任何命令行 flag 也要链上。
if [ -e /usr/lib/x86_64-linux-gnu/libz.so ] || pkg-config --exists zlib 2>/dev/null; then
    if in_tmp "$EXTC" -w -I "$(src autolink)" --run "$(src autolink/app.extc)" >"$tmp/o" 2>"$tmp/e"; then
        ok auto-link-module "只给 -I，不给链接 flag：zmod.link 的 lib z 让程序链上并跑通"
    else
        bad auto-link-module "$(head -1 "$tmp/e")"
    fi
    if gen "$(src autolink/app.extc)" "$tmp/auto.c" -I "$(src autolink)" \
       && [ "$(head -1 "$tmp/auto.c")" = "/* extc-libs: z */" ]; then
        ok auto-link-comment "模块带来的需求也写进产物：/* extc-libs: z */"
    else
        bad auto-link-comment "$(head -1 "$tmp/auto.c" 2>/dev/null || head -1 "$tmp/e")"
    fi
    # 整份夹具（连 app.extc 一起）拷到临时目录再去掉 .link：**根文件自己所在的目录**
    # 也在模块搜索路径里，所以只拷模块、把根留在原处，这条判据会被原来那份 .link 满足。
    cp -r "$(src autolink)" "$tmp/al"; rm -f "$tmp/al/zmod.link"
    if in_tmp "$EXTC" -w -I "$tmp/al" --run "$tmp/al/app.extc" >"$tmp/o" 2>"$tmp/e"; then
        bad auto-link-required "没有 .link 也过了 ⇒ 这条判据不承重"
    else
        ok auto-link-required "没有 .link ⇒ 链不上（自动链接是那张表带来的）"
    fi
else
    skipf auto-link-module "本机没有 libz.so 开发软链"
    skipf auto-link-comment "同上"
    skipf auto-link-required "同上"
fi

if ldconfig -p 2>/dev/null | grep -q 'libsqlite3\.so\.0'; then
    if in_tmp "$EXTC" -w -I "$(src soname)" --run "$(src soname/app.extc)" >"$tmp/o" 2>"$tmp/e"; then
        ok auto-link-soname "模块里的 lib :libsqlite3.so.0 ⇒ 无头无 .pc 也链上"
    else
        bad auto-link-soname "$(head -1 "$tmp/e")"
    fi
else
    skipf auto-link-soname "本机没有 libsqlite3.so.0"
fi

if in_tmp "$EXTC" -w -I "$(src unused)" --run "$(src unused/main.extc)" >"$tmp/o" 2>"$tmp/e"; then
    ok auto-link-unused-module "没 use 的模块不参与链接（哪怕它写着不存在的库）"
else
    bad auto-link-unused-module "$(head -1 "$tmp/e")"
fi

in_tmp "$EXTC" -w -I "$(src badlink)" --run "$(src badlink/app.extc)" >"$tmp/o" 2>"$tmp/e"; rc=$?
if [ "$rc" = 2 ] && grep -q 'unknown directive' "$tmp/e"; then
    ok auto-link-bad-directive "模块 .link 里的未知指令 ⇒ rc=2 + 指名道姓"
else
    bad auto-link-bad-directive "rc=$rc：$(head -1 "$tmp/e")"
fi

echo "== ④ 命令行错误要报清楚（不是静默链不上）=="
"$EXTC" -l >/dev/null 2>"$tmp/e"; rc=$?
if [ "$rc" = 2 ] && grep -q 'needs a library name' "$tmp/e"; then
    ok err-missing-arg "-l 缺参数 ⇒ rc=2 + 明确消息"
else
    bad err-missing-arg "rc=$rc：$(head -1 "$tmp/e")"
fi
"$EXTC" -w --pkg-config no_such_lib_xyz -o /dev/null "$(src zlib_version.extc)" >/dev/null 2>"$tmp/e"; rc=$?
if [ "$rc" = 2 ]; then
    ok err-pkg-missing "--pkg-config 包不存在 ⇒ rc=2（pkg-config 自己的 stderr 照传）"
else
    bad err-pkg-missing "rc=$rc：$(head -1 "$tmp/e")"
fi

printf '通过 %d，失败 %d，跳过 %d\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
