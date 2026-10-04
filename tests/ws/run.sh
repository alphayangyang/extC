#!/usr/bin/env bash
# tests/ws/run.sh —— WebSocket：握手向量 / 拒绝面 / 往返 / 变异 fuzz（fixture 自检）
#                   + **帧字节与 Python 独立实现逐字节对拍**（外部预言机）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
if "$EXTC" -w --no-line-map -o "$tmp/w.c" tests/ws/main.extc >/dev/null 2>&1 \
   && gcc -std=c11 -fwrapv -O2 -o "$tmp/w" "$tmp/w.c" 2>"$tmp/w.e"; then
    out=$("$tmp/w" 2>&1); rc=$?
    if [ "$rc" = 0 ]; then
        echo "  ok   rfc6455         ->  $(echo "$out" | tail -1 | cut -c1-110)"
        pass=$((pass+1))
    else
        echo "  FAIL rfc6455         ->  退出码 $rc"; fail=$((fail+1))
    fi
    python3 - > "$tmp/want" <<'PYEOF'
def frame(op, payload, mask):
    b = bytes([0x80 | op]); n = len(payload)
    if n < 126: b += bytes([0x80 | n])
    elif n <= 0xFFFF: b += bytes([0x80 | 126]) + n.to_bytes(2, 'big')
    else: b += bytes([0x80 | 127]) + n.to_bytes(8, 'big')
    b += mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    return b.hex()
pat = bytes((i * 7 + 3) % 256 for i in range(70000))
mask = bytes([1, 2, 3, 4])
for op in (1, 2, 8, 9, 10):
    for n in (0, 1, 125, 126, 127, 1000, 65535):
        if op >= 8 and n > 125: continue
        print("fr", op, n, frame(op, pat[:n], mask))
print("fr 1 65536", frame(1, pat[:65536], mask))
PYEOF
    echo "$out" | grep '^fr ' > "$tmp/got"
    if diff -q "$tmp/got" "$tmp/want" >/dev/null 2>&1; then
        echo "  ok   vs-python       ->  $(wc -l < "$tmp/got") 个帧与 Python 独立实现逐字节一致（含 126/127/65536 三种长度编码）"
        pass=$((pass+1))
    else
        echo "  FAIL vs-python       ->  $(diff "$tmp/got" "$tmp/want" | head -2 | cut -c1-100 | tr '\n' '|')"; fail=$((fail+1))
    fi
else
    echo "  FAIL ws              ->  $(head -2 "$tmp/w.e" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1))
fi
printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
