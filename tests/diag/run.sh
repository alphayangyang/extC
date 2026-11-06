#!/usr/bin/env bash
# tests/diag -- 诊断通道：一次报多条，但要报得准。
#
# 判据：
#   ① 三处互不相关的类型错误 ⇒ 三条都出现，位置分别是 2/3/4 行；
#   ② 函数体里的错误不在调用点重复报（错误类型不产生级联）；
#   ③ 语法错仍然只报一条（语法恢复风险高，不硬撑）；
#   ④ 超过 20 条 ⇒ 报 20 条 + 一行 "too many errors; stopped after 20"；
#   ⑤ `--diagnostics-json` 每条诊断一行 JSON，severity 都是 error。
set -u
cd "$(dirname "$0")/../.." || exit 1
fail=0
ok()  { printf '  ok %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
EXTC=${EXTC:-./build/extc}

# ① 三处互不相关的错误
cat > "$work/three.extc" <<'EOT'
fn main() -> i32 {
    let a: i32 = "text"
    let b: i64 = true
    let c: bool = 42
    return 0
}
EOT
out=$("$EXTC" --check-only -w "$work/three.extc" 2>&1) && bad "三处错误竟然编过了"
n=$(printf '%s\n' "$out" | grep -c ': error:')
[ "$n" = "3" ] && ok "三处错误都报出来（3 条）" || bad "报了 $n 条错误，期望 3 条"
for ln in 2 3 4; do
    printf '%s\n' "$out" | grep -q "three.extc:$ln:1: error:" && ok "第 $ln 行有诊断" || bad "第 $ln 行没有诊断"
done

# ② 函数体里的错误不在调用点重复
cat > "$work/nochain.extc" <<'EOT'
fn g() -> i32 { return "not an int" }
fn main() -> i32 {
    let c: i32 = g()
    return c
}
EOT
out=$("$EXTC" --check-only -w "$work/nochain.extc" 2>&1) && bad "nochain 竟然编过了"
n=$(printf '%s\n' "$out" | grep -c ': error:')
[ "$n" = "1" ] && ok "函数体内的错误不引发调用点级联（1 条）" || bad "级联成 $n 条，期望 1 条"

# ③ 语法错只报一条
cat > "$work/syntax.extc" <<'EOT'
fn main() -> i32 {
    let a: i32 = 1
EOT
out=$("$EXTC" --check-only -w "$work/syntax.extc" 2>&1) && bad "syntax 竟然编过了"
n=$(printf '%s\n' "$out" | grep -c ': error:')
[ "$n" = "1" ] && ok "语法错只报一条" || bad "语法错报了 $n 条，期望 1 条"

# ④ 上限
{ printf 'fn main() -> i32 {\n'; i=1; while [ "$i" -le 30 ]; do printf '    let v%d: i32 = "s%d"\n' "$i" "$i"; i=$((i+1)); done; printf '    return 0\n}\n'; } > "$work/many.extc"
out=$("$EXTC" --check-only -w "$work/many.extc" 2>&1) && bad "many 竟然编过了"
n=$(printf '%s\n' "$out" | grep ': error:' | grep -vc 'too many errors')
[ "$n" = "20" ] && ok "报满 20 条就停" || bad "报了 $n 条 error，期望 20 条"
printf '%s\n' "$out" | grep -q 'too many errors; stopped after 20' && ok "截断时明确说停在 20 条" || bad "没有说明截断"

# ⑤ JSON
out=$("$EXTC" --check-only --diagnostics-json -w "$work/three.extc" 2>&1)
n=$(printf '%s\n' "$out" | grep -c '"severity":"error"')
[ "$n" = "3" ] && ok "JSON 模式每条一行（3 行）" || bad "JSON 模式 $n 行，期望 3 行"

if [ "$fail" = 0 ]; then printf '\ndiag: 通过\n'; else printf '\ndiag: 失败 %d\n' "$fail"; fi
exit $((fail > 0))
