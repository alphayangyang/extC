#!/usr/bin/env bash
# tests/extpkg-system/run.sh —— `extpkg add <pkg-config 名>` 之后**能直接 `use`**。
#
# 判据链：① `add zlib --all` 在临时项目里生成 `vendor/zlib/{zlib.extc,zlib.link,extpkg.toml}`，
# 并把 `packages.toml` 记成系统依赖；② 直接 `use zlib` 的 app 能 `extpkg build` 出来并跑对
# （zlib 往返：`rc=0 in=64 out=19 back=64 same=yes`）；③ 不存在的包被明确拒绝。
#
# 没有 pkg-config / 没有 zlib 开发包的机器上整条**跳过**（不谎报通过）。
set -u
cd "$(dirname "$0")/../.."
EXTC=$(pwd)/build/extc
fail=0; pass=0
ok()  { pass=$((pass+1)); printf '  ok   %-16s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-16s %s\n' "$1" "$2"; }

if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists zlib; then
    echo "  skip  pkg-config / zlib 开发包不在，跳过"
    exit 0
fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/packages.toml" <<'TOML'
[project]
name = "extpkg-system-test"

[build]
entry = "app.extc"
out = "build/app"
TOML
cp tests/extpkg-system/app.extc "$tmp/"

# ---- ① add ----
if python3 tools/extpkg.py add zlib --all --project "$tmp" --extc "$EXTC" >"$tmp/add.log" 2>&1; then
    miss=0
    for f in vendor/zlib/zlib.extc vendor/zlib/zlib.link vendor/zlib/extpkg.toml; do
        [ -f "$tmp/$f" ] || { miss=1; echo "  FAIL add 没生成 $f"; }
    done
    grep -q 'system = true' "$tmp/packages.toml" || { miss=1; echo "  FAIL packages.toml 没记成系统依赖"; }
    grep -qE "^pkgconfig zlib$" "$tmp/vendor/zlib/zlib.link" || { miss=1; echo "  FAIL .link 里没有 pkgconfig:zlib"; }
    grep -q 'extern!("extc-zlib") fn compress' "$tmp/vendor/zlib/zlib.extc" \
        || { miss=1; echo "  FAIL 绑定里没有 compress"; }
    [ "$miss" = 0 ] && ok "add" "生成了绑定 + .link + 台账，并记进 packages.toml" \
                    || bad "add" "见上"
else
    bad "add" "$(tail -2 "$tmp/add.log" | tr '\n' ' ')"
fi

# ---- ② 直接 use + build + 跑 ----
if python3 tools/extpkg.py build --project "$tmp" --extc "$EXTC" >"$tmp/build.log" 2>&1; then
    got=$("$tmp/build/app" 2>&1)
    [ "$got" = "in=64 out=19 back=64 same=yes" ] && ok "use-and-run" "没有一行 extern/effects/链接参数（$got）" \
                                                 || bad "use-and-run" "输出是 \`$got\`"
else
    bad "use-and-run" "$(tail -2 "$tmp/build.log" | tr '\n' ' ')"
fi

# ---- ③ 不存在的包 ----
if python3 tools/extpkg.py add no-such-lib-xyz --all --project "$tmp" --extc "$EXTC" >"$tmp/neg.log" 2>&1; then
    bad "unknown-pkg" "居然成功了"
else
    grep -q "pkg-config 不认识" "$tmp/neg.log" && ok "unknown-pkg" "被明确拒绝" \
                                             || bad "unknown-pkg" "$(tail -1 "$tmp/neg.log")"
fi

echo "----"
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
