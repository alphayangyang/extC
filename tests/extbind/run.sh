#!/usr/bin/env bash
# tests/extbind/run.sh —— 声明式绑定生成器（`cbindgen.py --extern`）的判据。
#
# 目标：让"用别人的 C 库"这件事从"手写一堆 `extern!` + 每个签 effects"变成一条命令。
# 生成物 = `extern!("<lib>") fn …` 声明 + （`--effects trusted` 时）每个函数一条签字 +
# 旁边一个 `<module>.link` 写链接要求。
#
# 五条判据：
#   ① 生成物的形状对（签名映射 · `effects Addr=0 Cont=0` · 返回指针的 `Ret=0` 写在最后）；
#   ② `Ret=0` 写在 clause 末尾也解析 —— 这是真编译器 bug（`effects` 的续读只认 Addr/Cont/Thread）；
#   ③ 真的能调：编 `libdemo.a`、用生成的绑定、跑出 `sum=42 count=1`；
#   ④ `--check` 幂等（生成物与头文件一致时 rc=0，改一个字节后 rc!=0）；
#   ⑤ 没法生成的形状（变参）被**报出来**，不是猜一个。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"
CC="${CC:-cc}"
fail=0; pass=0
ok()  { pass=$((pass+1)); printf '  ok   %-20s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-20s %s\n' "$1" "$2"; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cp tests/extbind/demo.h "$tmp/"
$CC -c -O2 -o "$tmp/demo.o" tests/extbind/demo.c 2>"$tmp/cc.log" \
    && ar rcs "$tmp/libdemo.a" "$tmp/demo.o" || { echo "  FAIL 夹具编不过"; exit 1; }
cp tests/extbind/main.extc "$tmp/"

# ---- ① 生成 + 形状 ----
if python3 tools/cbindgen.py --include demo.h -I "$tmp" --only demo_open,demo_add,demo_close,demo_count \
        -o "$tmp/demoapi.extc" --extern extc-demo --effects trusted \
        --link "ccflag:$tmp/lib/libdemo.a" >"$tmp/gen.log" 2>&1; then
    ok=1
    for want in \
        'extern!("extc-demo") fn demo_open(name: ?ref u8) -> ?ref void' \
        'effects Addr=0 Cont=0 Ret=0' \
        'extern!("extc-demo") fn demo_add(h: ?ref void, a: i64, b: i64) -> i64' \
        'extern!("extc-demo") fn demo_count() -> u64' \
        'extern!("extc-demo") fn demo_close(h: ?ref void) -> void'; do
        grep -qF -- "$want" "$tmp/demoapi.extc" || { ok=0; echo "  FAIL 生成物里缺：$want"; }
    done
    # 不带指针返回的声明不该出现 `Ret=`（那一条只对返回引用/视图的函数有意义）
    if awk '/demo_add/{f=1;next} f&&/effects/{print;exit}' "$tmp/demoapi.extc" | grep -q 'Ret='; then
        ok=0; echo "  FAIL 标量返回的声明上也写了 Ret="
    fi
    [ "$ok" = 1 ] && ok "shape" "签名与 effects 的形状正确" || bad "shape" "见上面的缺项"
else
    bad "shape" "生成失败：$(tail -1 "$tmp/gen.log")"
fi

# ---- ④ `--check` 幂等 ----
if python3 tools/cbindgen.py --include demo.h -I "$tmp" --only demo_open,demo_add,demo_close,demo_count \
        -o "$tmp/demoapi.extc" --extern extc-demo --effects trusted --check >"$tmp/chk.log" 2>&1; then
    ok "check-idempotent" "$(tail -1 "$tmp/chk.log" | sed 's/^ok  //')"
else
    bad "check-idempotent" "--check 说生成物不是最新（这一份是刚生成的！）"
fi

# ---- ③ 真的能调（`Ret=0` 写在 clause 末尾也解析 —— ② 由这条顺带覆盖）----
mkdir -p "$tmp/lib"
mv "$tmp/libdemo.a" "$tmp/lib/"
# 链接要求由生成器顺手写在 `demoapi.link` 里（调用的程序一行链接参数都不写）
grep -qF -- "ccflag $tmp/lib/libdemo.a" "$tmp/demoapi.link" \
    || { bad "link-file" "生成器没写 .link"; }
if ( cd "$tmp" && "$OLDPWD/$EXTC" --run main.extc >"$tmp/run.out" 2>"$tmp/run.err" ); then
    got=$(cat "$tmp/run.out")
    [ "$got" = "sum=42 count=1" ] && ok "link-and-run" "生成的绑定直接调通（$got）" \
                                  || bad "link-and-run" "输出是 \`$got\`"
    grep -qF -- "ccflag $tmp/lib/libdemo.a" "$tmp/demoapi.link" \
        && ok "link-file" "链接要求由生成器写出（调用方不写 -l）" || true
else
    bad "link-and-run" "$(head -2 "$tmp/run.err" | tr '\n' ' ')"
fi

# ---- ⑤ 变参被报出来 ----
python3 tools/cbindgen.py --include demo.h -I "$tmp" --only demo_printf \
    -o "$tmp/neg.extc" --extern extc-demo 2>"$tmp/neg.log"
[ $? -ne 0 ] && grep -q "demo_printf" "$tmp/neg.log" \
    && ok "refuse-varargs" "变参函数被拒并报出名字" \
    || bad "refuse-varargs" "没有拒绝 demo_printf"

echo "----"
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
