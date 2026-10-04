#!/usr/bin/env bash
# tests/voidret/run.sh —— 裸 `return` 在 void 函数里的合法性（正例 + 反例）。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail=0
if out=$("$EXTC" -w --run tests/voidret/main.extc 2>&1) && echo "$out" | grep -q 'voidret ok'; then
    echo "  ok   void-methods   ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL void-methods   ->  $(echo "$out" | tail -2 | tr '\n' '|')"; fail=1
fi
# 反例：**非** void 函数里的裸 `return` 必须仍然报错（别把检查一起放宽了）
cat > "$tmp/bad.extc" <<'EOF'
fn needsValue() -> i64 {
    if true { return }
    return i64(0)
}
fn main() -> i32 { return i32(needsValue()) }
EOF
if out=$("$EXTC" -w --no-line-map -o "$tmp/bad.c" "$tmp/bad.extc" 2>&1); then
    echo "  FAIL nonvoid-bare-return -> 编过了（应该报错）"; fail=1
else
    if echo "$out" | grep -q 'must return a value of type `i64`'; then
        echo "  ok   nonvoid-bare-return -> 仍然报错：$(echo "$out" | head -1 | cut -c1-70)"
    else
        echo "  FAIL nonvoid-bare-return -> 报错信息不对：$(echo "$out" | head -1)"; fail=1
    fi
fi
[ "$fail" -eq 0 ]
