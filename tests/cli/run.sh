#!/usr/bin/env bash
# tests/cli/run.sh —— 驱动器的"项目"模式：extc run / extc build。
#
# 单文件 extc --run x.extc 一直是零仪式的，麻烦的是多模块项目：以前要自己拼
# `-I`/--ccflag/`-o`，还得记住入口文件。这两个子命令把"项目"变成约定：
#
#   ① 当前目录（或给定目录）里按约定找入口：`src/main.extc` → `main.extc` →
#      `src/main.extx` → `main.extx` → `packages.toml` 的 `[build] entry`；
#   ② 找不到时把**试过的每一条路径**都打出来（不是一句"没有入口"）；
#   ③ `--` 之后原样交给程序（`args.len` 能看见），这是 `@module` 子命令能用的前提；
#   ④ extc build 只出二进制不运行，`-o` 命名它；
#   ⑤ 不带任何模式也没有输入文件时，行为与以前一样：打印用法、退出 2。
#
# 判据全部在临时目录里跑：项目发现看的是 CWD，不能依赖仓库自身的布局。
set -u
cd "$(dirname "$0")/../.."
ROOT="$PWD"
EXTC="$ROOT/build/extc"
if [ ! -x "$EXTC" ]; then echo "  FAIL 没有 $EXTC（先 make）"; exit 1; fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0; pass=0
ok()  { pass=$((pass+1)); printf '  ok   %-26s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-26s %s\n' "$1" "$2"; }

# ---------------------------------------------------------------- ① 约定入口
mkdir -p "$TMP/a/src"
cat > "$TMP/a/src/util.extc" <<'EOF'
use std::io
fn twice(x: i64) -> i64 { return x * 2 }
EOF
cat > "$TMP/a/src/main.extc" <<'EOF'
use std::io
use util
fn main() -> i32 {
    io::cout << "src/main: " << util::twice(21) << "\n"
    return 0
}
EOF
out="$(cd "$TMP/a" && "$EXTC" run 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "src/main: 42" ]; then
    ok "entry-src-main" "当前目录无参数 → src/main.extc（兄弟模块自动解析）"
else
    bad "entry-src-main" "rc=$rc out=「$out」"
fi

# 平铺的 main.extc 也要认。
mkdir -p "$TMP/b"
cat > "$TMP/b/main.extc" <<'EOF'
use std::io
fn main() -> i32 { io::cout << "flat main\n"  return 0 }
EOF
out="$(cd "$TMP/b" && "$EXTC" run 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "flat main" ]; then
    ok "entry-flat-main" "平铺 main.extc"
else
    bad "entry-flat-main" "rc=$rc out=「$out」"
fi

# `.extx` 是包内部源文件的拼法，约定里同样认。
mkdir -p "$TMP/c/src"
cat > "$TMP/c/src/main.extx" <<'EOF'
use std::io
fn main() -> i32 { io::cout << "extx\n"  return 0 }
EOF
out="$(cd "$TMP/c" && "$EXTC" run 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "extx" ]; then
    ok "entry-extx" "src/main.extx"
else
    bad "entry-extx" "rc=$rc out=「$out」"
fi

# 清单里写的入口名可以是任意路径 —— 这是"约定找不到时"的最后一档。
mkdir -p "$TMP/d/app"
cat > "$TMP/d/packages.toml" <<'EOF'
[project]
name = "demo"

[build]
entry = "app/start.extx"
EOF
cat > "$TMP/d/app/start.extx" <<'EOF'
use std::io
fn main() -> i32 { io::cout << "manifest\n"  return 0 }
EOF
out="$(cd "$TMP/d" && "$EXTC" run 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "manifest" ]; then
    ok "entry-manifest" "packages.toml 的 [build] entry"
else
    bad "entry-manifest" "rc=$rc out=「$out」"
fi

# 给定目录等同于 cd 进去跑。
out="$("$EXTC" run "$TMP/a" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "src/main: 42" ]; then
    ok "entry-dir-arg" "extc run <dir>"
else
    bad "entry-dir-arg" "rc=$rc out=「$out」"
fi

# ---------------------------------------------------------------- ② 找不到入口
mkdir -p "$TMP/empty"
err="$(cd "$TMP/empty" && "$EXTC" run 2>&1 >/dev/null)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s' "$err" | grep -q "no entry file" \
   && printf '%s' "$err" | grep -q "src/main.extc" \
   && printf '%s' "$err" | grep -q "packages.toml"; then
    ok "no-entry" "退出 2，且把试过的路径都列出来"
else
    bad "no-entry" "rc=$rc err=「$err」"
fi

# ---------------------------------------------------------------- ③ 参数透传
mkdir -p "$TMP/e/src"
cat > "$TMP/e/src/main.extc" <<'EOF'
use std::io
fn main(args: slice<slice<u8>>) -> i32 {
    io::cout << "argc=" << args.len << "\n"
    if args.len > 2 { io::cout << "last=" << args[args.len - 1] << "\n" }
    return 0
}
EOF
out="$(cd "$TMP/e" && "$EXTC" run -- a "b c" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "argc=3
last=b c" ]; then
    ok "args-after-dashdash" "分隔符 -- 之后原样交给程序（含带空格的实参）"
else
    bad "args-after-dashdash" "rc=$rc out=「$out」"
fi

# 没有 `--` 时空参数：程序只看见自己的路径。
out="$(cd "$TMP/e" && "$EXTC" run 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "argc=1" ]; then
    ok "args-none" "没写 -- 就不传参数"
else
    bad "args-none" "rc=$rc out=「$out」"
fi

# ---------------------------------------------------------------- ④ build 模式
out="$(cd "$TMP/a" && "$EXTC" build -o "$TMP/a/prog" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ -x "$TMP/a/prog" ] && [ -z "$out" ]; then
    ok "build-only" "extc build -o FILE 只出二进制、不运行"
else
    bad "build-only" "rc=$rc out=「$out」可执行=$([ -x "$TMP/a/prog" ] && echo yes || echo no)"
fi
run_out="$("$TMP/a/prog" 2>&1)"
if [ "$run_out" = "src/main: 42" ]; then
    ok "build-runs" "产出的二进制跑出与 run 相同的结果"
else
    bad "build-runs" "out=「$run_out」"
fi

# ---------------------------------------------------------------- ⑤ 旧行为不变
err="$("$EXTC" 2>&1 >/dev/null)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s' "$err" | grep -q "usage:"; then
    ok "no-args-usage" "没有任何模式与输入 → 用法、退出 2（与以前一致）"
else
    bad "no-args-usage" "rc=$rc"
fi

# --run/--build 这两个旧拼写仍然要能用（脚本与 extpkg 都在用）。
out="$(cd "$TMP/a" && EXTC_RUN_DIR="$TMP/rd" "$EXTC" --run src/main.extc 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "src/main: 42" ]; then
    ok "legacy-run" "--run 拼写不变"
else
    bad "legacy-run" "rc=$rc out=「$out」"
fi

printf '\n  通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
