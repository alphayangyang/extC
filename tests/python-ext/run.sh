#!/usr/bin/env bash
# tests/python-ext/run.sh —— extC 编译出来的函数，`pip install` 之后能 `import`。
#
# 这条线的目标是"能装进 Python 项目"：`@export` 出来的符号本来就是普通 C 函数（名字不带模块
# 前缀），所以工具 `tools/pybind.py` 只生成一层 `PyArg_ParseTuple` + 调用 + 装箱，没有运行期粘合。
#
# 四条判据：
#   ① 生成包装 + 编译成扩展模块，`import` 后调得通（i64 / f64 / bool / bytes）；
#   ② 参数类型错时由 CPython 报 `TypeError`（不是崩溃，也不是悄悄转）；
#   ③ `--scaffold` 出来的目录能 `pip install`（离线：`--no-build-isolation`），装完 `import` 正确；
#   ④ 工具拒绝不认识的类型 / 重复的 Python 名字（负例）。
#
# 没有 Python 开发头文件时整条测试**跳过**（不红）：它测的是打包通道，不是编译器。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"
CC="${CC:-cc}"
fail=0; pass=0
ok()  { pass=$((pass+1)); printf '  ok   %-22s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-22s %s\n' "$1" "$2"; }

INC=$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["include"])' 2>/dev/null || true)
SUFFIX=$(python3-config --extension-suffix 2>/dev/null || true)
if [ -z "$INC" ] || [ ! -f "$INC/Python.h" ] || [ -z "$SUFFIX" ]; then
    echo "  skip  python-ext  没有 Python 开发头文件（Python.h / python3-config 缺失）"
    exit 0
fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/mod" "$tmp/proj"

# ---- 夹具：一个只有 `@export` 的 extC 文件（没有 main） -------------------------------------
cat > "$tmp/mod/mathmod.extc" <<'EOF'
@export fn add(a: i64, b: i64) -> i64 { return a + b }
@export fn scale(x: f64, k: f64) -> f64 { return x * k }
@export fn isEven(n: i64) -> bool { return n % 2 == 0 }
EOF
cat > "$tmp/mod/bytesmod.extc" <<'EOF'
/* `@export` 不能收 `slice<u8>`（那是两个 C 参数），所以边界上的字节按 `(ref u8, i64)` 两个参数进。
 * `ref u8` 在语言里不能索引（指针不是视图），所以这里只验证"指针 + 长度真的到了 C 那边"——
 * 要读内容得先有一片过了区间检查的视图（`mem::plate::viewAt` 那种门），或者 C 侧自己读。 */
@export fn countLen(p: ref u8, n: i64) -> i64 { return n }
EOF

# ---- ① 生成包装、编译、import ---------------------------------------------------------------
if $EXTC -w --no-line-map -o "$tmp/mod/mathmod.c" "$tmp/mod/mathmod.extc" \
   && $EXTC -w --no-line-map -o "$tmp/mod/bytesmod.c" "$tmp/mod/bytesmod.extc"; then
    if python3 tools/pybind.py --name extcmod --product "$tmp/mod/mathmod.c" \
            -o "$tmp/mod/pymod.c" \
            --fn add:add\(i64,i64\)-\>i64 \
            --fn scale:scale\(f64,f64\)-\>f64 \
            --fn isEven:isEven\(i64\)-\>bool; then
        if $CC -shared -fPIC -O2 -I"$INC" "$tmp/mod/mathmod.c" "$tmp/mod/pymod.c" \
               -o "$tmp/mod/extcmod$SUFFIX" 2>"$tmp/mod/build.log"; then
            got=$(cd "$tmp/mod" && python3 -c '
import extcmod
print(extcmod.add(2, 3), extcmod.scale(1.5, 4.0), extcmod.isEven(10), extcmod.isEven(7))' 2>&1)
            [ "$got" = "5 6.0 True False" ] \
                && ok "extc-export" "import 后调用正确（$got）" \
                || bad "extc-export" "输出是 \`$got\`"
            # ② 参数类型错：CPython 报 TypeError，不是崩
            terr=$(cd "$tmp/mod" && python3 -c '
import extcmod
try:
    extcmod.add("x", 1)
except TypeError as e:
    print("TypeError")' 2>&1)
            [ "$terr" = "TypeError" ] \
                && ok "type-error" "参数类型错由 CPython 报 TypeError" \
                || bad "type-error" "得到 \`$terr\`"
        else
            bad "extc-export" "编不出扩展模块：$(tail -2 "$tmp/mod/build.log" | tr '\n' ' ')"
        fi
    else
        bad "extc-export" "pybind.py 生成失败"
    fi
else
    bad "extc-export" "extC 编译失败"
fi

# ---- bytes：Python 传 bytes，C 侧收 (ref u8, i64) -------------------------------------------
if $EXTC -w --no-line-map -o "$tmp/mod/bytesmod.c" "$tmp/mod/bytesmod.extc" 2>/dev/null \
   && python3 tools/pybind.py --name bytesmod --product "$tmp/mod/bytesmod.c" \
        -o "$tmp/mod/pybytes.c" --fn countLen:countLen\(bytes\)-\>i64 2>/dev/null \
   && $CC -shared -fPIC -O2 -I"$INC" "$tmp/mod/bytesmod.c" "$tmp/mod/pybytes.c" \
        -o "$tmp/mod/bytesmod$SUFFIX" 2>/dev/null; then
    got=$(cd "$tmp/mod" && python3 -c '
import bytesmod
print(bytesmod.countLen(b"abcd"))' 2>&1)
    [ "$got" = "4" ] && ok "bytes" "bytes 按 (ref u8, i64) 传进 C（$got）" \
                     || bad "bytes" "输出是 \`$got\`（期望 4 = len \"abcd\"）"
else
    bad "bytes" "bytes 通道编不出来"
fi

# ---- ③ `pip install`（离线） ----------------------------------------------------------------
if python3 tools/pybind.py --name extcmod --product "$tmp/mod/mathmod.c" --scaffold "$tmp/proj" \
        -o /dev/null --fn add:add\(i64,i64\)-\>i64 --fn isEven:isEven\(i64\)-\>bool 2>/dev/null; then
    if python3 -m pip install -q --no-build-isolation --target "$tmp/site" "$tmp/proj" \
            >"$tmp/pip.log" 2>&1; then
        got=$(PYTHONPATH="$tmp/site" python3 -c '
import extcmod
print(extcmod.add(20, 22), extcmod.isEven(7))' 2>&1)
        [ "$got" = "42 False" ] && ok "pip-install" "scaffold 出来的项目可装可 import（$got）" \
                                || bad "pip-install" "装完 import 输出 \`$got\`"
    else
        bad "pip-install" "pip 装不上：$(tail -2 "$tmp/pip.log" | tr '\n' ' ')"
    fi
else
    bad "pip-install" "scaffold 生成失败"
fi

# ---- ④ 负例：不认识的类型 / 重复名字 --------------------------------------------------------
python3 tools/pybind.py --name x -o /dev/null --fn f:f\(string\)-\>i64 >"$tmp/neg1.log" 2>&1
[ $? -ne 0 ] && grep -q "不认识的类型" "$tmp/neg1.log" \
    && ok "neg-type" "不认识的类型被拒" || bad "neg-type" "没有拒绝 \`string\`"
python3 tools/pybind.py --name x -o /dev/null --fn a:add\(i64\)-\>i64 --fn a:sub\(i64\)-\>i64 >"$tmp/neg2.log" 2>&1
[ $? -ne 0 ] && grep -q "重复的 Python 名字" "$tmp/neg2.log" \
    && ok "neg-dup" "重复的 Python 名字被拒" || bad "neg-dup" "没有拒绝重名"

echo "----"
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
