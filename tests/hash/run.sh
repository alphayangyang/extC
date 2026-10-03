#!/usr/bin/env bash
# tests/hash/run.sh —— 哈希判据：官方向量 · 与 Python 逐字节对拍 · 流式一致 · API 失败面 ·
#                     吞吐与内存（"最快 + 内存少"是可量的，不是形容词）。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"
STD="$PWD/stdlib"
fail=0; pass=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { pass=$((pass+1)); printf '  ok   %-18s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-18s %s\n' "$1" "$2"; }
run() { "$EXTC" -w -I "$STD" --run "tests/hash/$1.extc" 2>&1; }

echo "== 官方向量（FIPS 180-4 / RFC 3174 / RFC 4231，不依赖 Python）=="
if out=$(run vectors) && diff <(echo "$out" | grep -v '^$') tests/hash/expected.txt >"$tmp/d1" 2>&1; then
    ok vectors "8 条向量逐字节一致（含 100 万个 'a' 的流式路径）"
else
    bad vectors "$(head -3 "$tmp/d1" | tr '\n' '|')"
fi
if out=$(run hmac) && diff <(echo "$out" | grep -v '^$') tests/hash/expected_hmac.txt >"$tmp/d2" 2>&1; then
    ok hmac-rfc4231 "8 条（含 131 字节长键、截断到 16 字节）"
else
    bad hmac-rfc4231 "$(head -3 "$tmp/d2" | tr '\n' '|')"
fi

echo "== 与 Python 的 hashlib 对拍（固定模式数据 · 16 种长度 · 含 7 字节分片流式）=="
python3 - >"$tmp/cross.want" <<'PY'
import hashlib
pat = bytes(((i * 31 + 7) & 255) for i in range(131072))
for n in (0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 127, 128, 129, 1000, 4096, 100000):
    print(f"sha256:{n} {hashlib.sha256(pat[:n]).hexdigest()}")
    print(f"sha1:{n} {hashlib.sha1(pat[:n]).hexdigest()}")
    print(f"sha256s:{n} {hashlib.sha256(pat[:n]).hexdigest()}")
print("cross ok")
PY
if out=$(run cross) && diff <(echo "$out" | grep -v '^$') "$tmp/cross.want" >"$tmp/d3" 2>&1; then
    ok cross-python "48 个摘要逐字节一致（流式 == 一次算完）"
else
    bad cross-python "$(head -3 "$tmp/d3" | tr '\n' '|')"
fi

echo "== uuid5 与 Python 的 uuid.uuid5 逐字节对拍（QQBot 主键就是它）=="
python3 - >"$tmp/uuid.want" <<'PY'
import uuid
for label, ns, name in (('dns', uuid.NAMESPACE_DNS, 'example.com'),
                        ('dns2', uuid.NAMESPACE_DNS, 'www.example.com'),
                        ('url', uuid.NAMESPACE_URL, 'https://example.com/a?b=1'),
                        ('oid', uuid.NAMESPACE_OID, '1.3.6.1.4.1'),
                        ('x500', uuid.NAMESPACE_X500, 'CN=Test'),
                        ('utf8', uuid.NAMESPACE_DNS, '群-123456789-钢琴房')):
    print(label, uuid.uuid5(ns, name))
print("uuid ok")
PY
if out=$(run uuid) && diff <(echo "$out" | grep -v '^$') "$tmp/uuid.want" >"$tmp/d4" 2>&1; then
    ok uuid5-python "6 个（4 个标准命名空间 + 中文名字）逐字节一致"
else
    bad uuid5-python "$(head -3 "$tmp/d4" | tr '\n' '|')"
fi

echo "== API 失败面（缓冲不够 ⇒ false / -1，不 trap）=="
if out=$(run api) && echo "$out" | grep -q 'api ok'; then
    ok api-bounds "sha256/sha1 的 final 与 hex 都不越界写"
else
    bad api-bounds "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== 吞吐与内存（编译成二进制再量；RSS 用 /usr/bin/time -v）=="
if "$EXTC" -w -I "$STD" --build -o "$tmp/bench" tests/hash/bench.extc >/dev/null 2>&1; then
    if out=$(/usr/bin/time -v "$tmp/bench" 2>"$tmp/tv"); then
        rss=$(grep -o 'Maximum resident set size (kbytes): [0-9]*' "$tmp/tv" | grep -o '[0-9]*$')
        line=$(echo "$out" | grep 'sha256' | head -1)
        if [ "${rss:-999999}" -lt 8192 ]; then
            ok memory "峰值 RSS ${rss} KB（1 MiB 输入 × 256 轮；零堆分配 ⇒ 只有二进制本身）"
        else
            bad memory "峰值 RSS ${rss} KB ≥ 8192 KB（有人往热路径里塞了分配）"
        fi
        mbps=$(echo "$line" | grep -o '[0-9]* MB/s' | grep -o '[0-9]*')
        if [ "${mbps:-0}" -ge 50 ]; then
            ok throughput "$(echo "$out" | grep MB/s | tr '\n' ' ' | sed 's/  */ /g')"
        else
            bad throughput "只有 ${mbps} MB/s（下标检查漏回热循环了？）"
        fi
    else
        bad bench "bench 跑不起来"
    fi
else
    bad bench "bench 编不出来"
fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
