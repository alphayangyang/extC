#!/usr/bin/env bash
# tests/extpkg/run.sh —— 包工具的全链路判据（`fetch` / `vendor` / `build`）。
#
# **不联网**：假 registry 走 `file://`（把 tests/pkg/honest 打成确定性 tar.gz）。
# 六条判据，每条都能红：
#   ① 首次 fetch：下载 → 校验 → 写 extc.lock；
#   ② 第二次 `--offline`：命中缓存（要联网就会报错，所以"过"就是"没联网"的证据）；
#   ③ 改 lock 里一个字节：必须红，且说清期望/实际哈希；
#   ④ 空缓存 + `--offline`：必须红，且消息给得出下一步该跑什么；
#   ⑤ `vendor` 之后**清空缓存**仍能 `build`（证明 vendor 自足 —— 断网机器靠它）；
#   ⑥ 两次干净 fetch 的 `extc.lock` **逐字节相同**（构建可复现的前提）。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"
fail=0; pass=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { pass=$((pass+1)); printf '  ok   %-22s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-22s %s\n' "$1" "$2"; }
ep()  { python3 tools/extpkg.py "$@"; }          # 省的每行都写全

# ---------------------------------------------------------------- 假 registry
reg="$tmp/registry"; mkdir -p "$reg"
( cd tests/pkg && tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
      -czf "$reg/honest-1.tar.gz" honest ) || { echo "  FAIL 打不出假 registry"; exit 1; }

# ---------------------------------------------------------------- 假项目
proj="$tmp/proj"; mkdir -p "$proj"
cat > "$proj/packages.toml" <<EOF
[project]
name = "demoapp"
version = "0.1.0"

[build]
entry = "main.extc"
out = "build/demoapp"

[dependencies]
honest = { version = "1", source = "$reg/honest-1.tar.gz" }
EOF
cat > "$proj/main.extc" <<'EOF'
use demo
use std::heap
use std::io

fn main() -> i32 {
    var pl: heap::plate = heap::plate::open(heap::DEFAULT_RESERVE)!
    var v: mut slice<u8> = pl.alloc(u64(8))!
    v[0] = u8(5)
    v[1] = u8(7)
    let s: i64 = demo::extc_demo_consume(v.data, i64(2))
    pl.close()
    io::cout << "sum=" << s << "\n"
    return i32(s) - 12
}
EOF

cache="$tmp/cache1"; empty="$tmp/cache-empty"; mkdir -p "$empty"

echo "== ① 首次 fetch：下载 → 校验 → 写 lock =="
if out=$(EXTPKG_CACHE="$cache" ep fetch --project "$proj" 2>&1) \
   && [ -f "$proj/extc.lock" ] && echo "$out" | grep -q '首次获取' \
   && echo "$out" | grep -q 'sha256='; then
    ok first-fetch "下载 + 首次哈希 + 写 $(basename "$proj/extc.lock")"
else
    bad first-fetch "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== ② 第二次 --offline：命中缓存（没过就是偷偷联网了）=="
if out=$(EXTPKG_CACHE="$cache" ep fetch --project "$proj" --offline 2>&1); then
    ok offline-cache-hit "不再联网，直接用缓存与锁"
else
    bad offline-cache-hit "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== ③ 改 lock 里一个字节：必须红 =="
cp "$proj/extc.lock" "$tmp/lock.bak"
python3 - "$proj/extc.lock" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding='utf-8')
s = re.sub(r'(sha256 = ")([0-9a-f])', lambda m: m.group(1) + ('0' if m.group(2) != '0' else '1'), s, count=1)
p.write_text(s, encoding='utf-8')
PY
if out=$(EXTPKG_CACHE="$cache" ep fetch --project "$proj" 2>&1); then
    bad tampered-lock "哈希被改了还过了 ⇒ 校验是摆设"
else
    if echo "$out" | grep -q '哈希不符'; then ok tampered-lock "当场报哈希不符（期望/实际都给出来）";
    else bad tampered-lock "拒了但理由不对：$(echo "$out" | tail -1)"; fi
fi
cp "$tmp/lock.bak" "$proj/extc.lock"

echo "== ④ 空缓存 + --offline：必须红，且消息可执行 =="
if out=$(EXTPKG_CACHE="$empty" ep fetch --project "$proj" --offline 2>&1); then
    bad offline-empty-cache "空缓存 + --offline 居然过了"
else
    if echo "$out" | grep -q '先跑一次'; then ok offline-empty-cache "报错并指出下一步（先 fetch）";
    else bad offline-empty-cache "拒了但消息不可执行：$(echo "$out" | tail -1)"; fi
fi

echo "== ⑤ vendor 之后清空缓存：仍能 build（断网机器靠它）=="
if out=$(EXTPKG_CACHE="$cache" ep vendor --project "$proj" 2>&1) && [ -d "$proj/vendor/honest" ]; then
    rm -rf "$cache"                                     # 模拟"换了一台没有缓存的机器"
    if out=$(EXTPKG_CACHE="$cache" ep build --project "$proj" --offline 2>&1) \
       && [ -x "$proj/build/demoapp" ]; then
        got=$("$proj/build/demoapp"); rc=$?
        if [ "$rc" = 0 ] && [ "$got" = "sum=12" ]; then
            ok vendor-selfcontained "vendor/ 自足：清缓存 + --offline 也能编出并跑对（$got）"
        else
            bad vendor-selfcontained "编出来了但跑得不对：rc=$rc out=$got"
        fi
    else
        bad vendor-selfcontained "$(echo "$out" | tail -2 | tr '\n' '|')"
    fi
else
    bad vendor-selfcontained "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== ⑥ 两次干净 fetch：lock 逐字节相同 =="
if EXTPKG_CACHE="$tmp/c2" ep fetch --project "$proj" >/dev/null 2>&1 \
   && cp "$proj/extc.lock" "$tmp/lock2" \
   && EXTPKG_CACHE="$tmp/c3" ep fetch --project "$proj" >/dev/null 2>&1 \
   && cmp -s "$tmp/lock2" "$proj/extc.lock"; then
    ok lock-deterministic "同一份 packages.toml ⇒ 同一份 lock（逐字节）"
else
    bad lock-deterministic "两次 fetch 的 lock 不同（构建不可复现）"
fi

echo "== ⑦ build 会自动补齐 vendor（缓存还在时）=="
rm -rf "$proj/vendor"
if out=$(EXTPKG_CACHE="$tmp/c2" ep build --project "$proj" --offline 2>&1) \
   && [ -d "$proj/vendor/honest" ] && [ -x "$proj/build/demoapp" ]; then
    ok build-auto-vendor "缺 vendor 时 build 自己补上（fetch 有锁 ⇒ 不用联网）"
else
    bad build-auto-vendor "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
