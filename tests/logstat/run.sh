#!/usr/bin/env bash
# tests/logstat -- the demo program in examples/logstat does what it claims.
#
# 判据：`gen` 写出的行数与 `summary` 数出的行数一致；报表的关键行在；
# 未知子命令退出码 2；同一个文件两次 summary 输出逐字节相同（无隐藏状态）。
set -u
cd "$(dirname "$0")/../.." || exit 1
fail=0
ok()  { printf '  ok %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
EXTC=${EXTC:-./build/extc}

if ! out=$("$EXTC" run examples/logstat -- gen "$work/a.tsv" 5000 2>&1); then
    bad "gen 跑不起来: $out"
    exit 1
fi
lines=$(wc -l < "$work/a.tsv")
[ "$lines" = "5000" ] && ok "gen 写出 5000 行" || bad "gen 写了 $lines 行，期望 5000"

if ! out=$("$EXTC" run examples/logstat -- summary "$work/a.tsv" 2>&1); then
    bad "summary 跑不起来: $out"
    exit 1
fi
printf '%s\n' "$out" | grep -q '^records=5000 ' && ok "summary 数出 5000 条" || bad "records 行不对: $(printf '%s' "$out" | head -1)"
printf '%s\n' "$out" | grep -q '^top1 ' && ok "报表有 top1 行" || bad "没有 top1 行"
printf '%s\n' "$out" | grep -q '^status 2xx=' && ok "报表有状态分布" || bad "没有状态分布行"

# 同样的输入，同样的输出（报表里没有隐藏状态）
again=$("$EXTC" run examples/logstat -- summary "$work/a.tsv" 2>&1) || true
[ "$again" = "$out" ] && ok "两次 summary 输出一致" || bad "两次 summary 输出不同"

# 未知子命令：用法错误，退出码 2
"$EXTC" run examples/logstat -- bogus "$work/a.tsv" >/dev/null 2>&1
rc=$?
[ "$rc" = "2" ] && ok "未知子命令退出码 2" || bad "未知子命令退出码 $rc，期望 2"

if [ "$fail" = 0 ]; then printf '\nlogstat: 通过\n'; else printf '\nlogstat: 失败 %d\n' "$fail"; fi
exit $((fail > 0))
