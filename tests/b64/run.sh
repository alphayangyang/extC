#!/usr/bin/env bash
# tests/b64/run.sh —— Base64：RFC 向量 / URL 表 / 拒绝面 / 往返（fixture 自检）
#                   + 固定语料与 Python `base64` **逐字节对拍**（外部预言机）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
if "$EXTC" -w --no-line-map -o "$tmp/b.c" tests/b64/main.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -O2 -o "$tmp/b" "$tmp/b.c" 2>"$tmp/b.e"; then
    out=$("$tmp/b" 2>&1); rc=$?
    if [ "$rc" = 0 ]; then
        echo "  ok   rfc4648         ->  RFC 向量 · URL 表 · 拒绝面（长度/非法字符/填充位置/填充过多）· 往返 · 缓冲不够响亮失败"
        pass=$((pass+1))
    else
        echo "  FAIL rfc4648         ->  退出码 $rc"; fail=$((fail+1))
    fi
    python3 - > "$tmp/want" <<'PYEOF'
import base64
pat = bytes((i * 7 + 3) % 256 for i in range(32))
for n in range(25):
    print("enc", n, base64.b64encode(pat[:n]).decode())
PYEOF
    echo "$out" | grep '^enc ' > "$tmp/got"
    if diff -q "$tmp/got" "$tmp/want" >/dev/null 2>&1; then
        echo "  ok   vs-python       ->  25 组（长度 0..24）与 Python base64 逐字节一致"
        pass=$((pass+1))
    else
        echo "  FAIL vs-python       ->  $(diff "$tmp/got" "$tmp/want" | head -3 | tr '\n' '|')"; fail=$((fail+1))
    fi
else
    echo "  FAIL b64             ->  $(head -2 "$tmp/b.e" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1))
fi
printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
