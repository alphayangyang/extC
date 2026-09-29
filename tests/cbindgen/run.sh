#!/usr/bin/env bash
# tests/cbindgen/run.sh —— **绑定生成器自己的判据**（不需要任何真实第三方库，所以哪台机器都能跑）
#
# 两头发力：
#   ① 类型映射（假头文件 `demo.h`）：逐条 grep 生成出来的声明，外加"一条 effects 都不生成"与
#      "生成物能被 extC 编译"；
#   ② 真跑一次（`libcmini.h` → `libcmini.extc`）：`dlopen("libc.so.6")` 调 `getpid()`，
#      判据是拿回来的是我们这个进程的号 —— 生成的绑定真的能调。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

echo "== ① 类型映射（tests/cbindgen/demo.h：不透明句柄 · 函数指针 · 隐式枚举 · float/double）=="
if python3 tools/cbindgen.py --include demo.h -I tests/cbindgen --name demo --all \
        -o "$tmp/demoapi.extc" >"$tmp/gen.log" 2>&1; then
    ok=1
    want=(
      'open: fn(?ref u8, i32) -> ?ref void'                                   # const char* → ?ref u8
      'write: fn(?ref void, ?ref void, u64) -> i32'                            # const void* · unsigned long
      'set_callback: fn(?ref void, fn(?ref void, ?ref u8) -> void, ?ref void) -> void'  # 函数指针
      'scale: fn(f64, f32) -> f64'                                             # double · float
      'close: fn(?ref void) -> void'                                           # void 返回
      'let DEMO_ONE: i32 = 1'
      'let DEMO_TWO: i32 = 2'                                                 # 隐式值：clang 求值，不猜
      'let DEMO_THREE: i32 = 3'
    )
    for w in "${want[@]}"; do
        grep -qF -- "$w" "$tmp/demoapi.extc" || { ok=0; echo "  FAIL 生成物里缺：$w"; }
    done
    # 只看**代码行**：生成物的头部注释里会讲到 effects（那是说明），不算生成了一条
    if grep -v '^//' "$tmp/demoapi.extc" | grep -q "effects"; then
        ok=0; echo "  FAIL 生成物里出现了 effects 子句（应当一条都不生成）"
    fi
    if ! "$EXTC" "$tmp/demoapi.extc" -o /dev/null 2>"$tmp/cc.log"; then
        ok=0; echo "  FAIL 生成物编译不过：$(head -1 "$tmp/cc.log")"
    fi
    if [ "$ok" = 1 ]; then echo "  ok   cbindgen  ->  8 条声明逐条对上 · 零 effects · 生成物能编译（8 个判据）"
    else echo "  FAIL cbindgen  ->  见上"; fail=1; fi
else
    echo "  FAIL cbindgen  ->  生成失败：$(head -2 "$tmp/gen.log")"; fail=1
fi

echo "== ② 生成物与头文件一致（--check，生成物入库）=="
if out=$(python3 tools/cbindgen.py --include libcmini.h -I tests/cbindgen --name libc --all \
            -o tests/cbindgen/libcmini.extc --check 2>&1); then
    echo "  ok   check  ->  $out"
else
    echo "  FAIL check  ->  $out"; fail=1
fi

echo "== ③ 真跑一次：生成的绑定 dlopen libc 并调 getpid() =="
if out=$("$EXTC" --run tests/cbindgen/libcmain.extc 2>&1); then
    case "$out" in
        *generated-ok\ pid=*) echo "  ok   run  ->  $(echo "$out" | tr '\n' '|')（生成的声明真的调到了真库）" ;;
        *) echo "  FAIL run  ->  输出对不上：$(echo "$out" | tr '\n' '|')"; fail=1 ;;
    esac
else
    echo "  FAIL run  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -4; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
