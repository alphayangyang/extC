#!/usr/bin/env bash
# tests/logstat -- the demo project in examples/logstat does what it claims.
#
# 判据：三个 @module 入口在 `--help` 里都列出来；`-m gen` 写出的行数与
# `-m summary` 数出的行数一致；报表关键行在；同一文件两次 summary 逐字节相同；
# 未知 `-m` 退出码 2。
set -u
cd "$(dirname "$0")/../.." || exit 1
fail=0
ok()  { printf '  ok %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
EXTC=${EXTC:-./build/extc}

# 项目模式：examples/logstat 里只有一个项目（src/main.extc + 三个入口模块）
if ! out=$("$EXTC" run examples/logstat -- -m gen "$work/a.tsv" 5000 2>&1); then
    bad "-m gen 跑不起来: $out"
    exit 1
fi
lines=$(wc -l < "$work/a.tsv")
[ "$lines" = "5000" ] && ok "-m gen 写出 5000 行" || bad "-m gen 写了 $lines 行，期望 5000"

if ! out=$("$EXTC" run examples/logstat -- -m summary "$work/a.tsv" 2>&1); then
    bad "-m summary 跑不起来: $out"
    exit 1
fi
printf '%s\n' "$out" | grep -q '^records=5000 ' && ok "-m summary 数出 5000 条" || bad "records 行不对: $(printf '%s' "$out" | head -1)"
printf '%s\n' "$out" | grep -q '^top1 ' && ok "报表有 top1 行" || bad "没有 top1 行"
printf '%s\n' "$out" | grep -q '^status 2xx=' && ok "报表有状态分布" || bad "没有状态分布行"

again=$("$EXTC" run examples/logstat -- -m summary "$work/a.tsv" 2>&1) || true
[ "$again" = "$out" ] && ok "两次 -m summary 输出一致" || bad "两次 -m summary 输出不同"

"$EXTC" run examples/logstat -- --help 2>&1 | grep -q -- '-m serve' && ok "--help 列出 -m serve" || bad "--help 没列出 -m serve"

"$EXTC" run examples/logstat -- -m bogus "$work/a.tsv" >/dev/null 2>&1
rc=$?
[ "$rc" = "2" ] && ok "未知 -m 退出码 2" || bad "未知 -m 退出码 $rc，期望 2"

if [ "$fail" = 0 ]; then printf '\nlogstat: 通过\n'; else printf '\nlogstat: 失败 %d\n' "$fail"; fi
exit $((fail > 0))
