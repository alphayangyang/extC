#!/usr/bin/env bash
# @module 入口 + 分发器（./a -m <name> / --help）判据
#   ① 生成物：C 的 main 是分发器，表里两行，入口是 static 的 __extc_entry_*
#   ② 行为：默认入口 / -m 带参数 / -m 不带参数 / --help / 未知模块 exit 2 / -m 缺名字 exit 2
#   ③ 入口文件没有 main：不带 -m 只打印用法并 exit 2
#   ④ 零税：没有 @module 的程序生成物里一个分发器字节都没有
#   ⑤ 负例：注解位置 / 名字冲突 / 签名 —— 每条都必须编译期报错，且报对消息
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
ok()  { echo "  ok   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1  ->  $2"; fail=$((fail+1)); }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 合同编译：-Wall -Wextra -Werror。生成物自己不许有告警。
cc_build() {  # $1 = .c, $2 = 可执行
    cc -std=c11 -fwrapv -Wall -Wextra -Werror -O1 "$1" -o "$2" -lm 2>"$tmp/cc.log"
}

# ---- ① 生成物 ----
if "$EXTC" -w --no-line-map -o "$tmp/m.c" tests/module/main.extc 2>"$tmp/emit.log"; then
    miss=""
    for pat in 'typedef struct ExtcModuleEntry' 'extc_module_entries[] = {' \
               'static int __extc_entry_demo$run(int argc, char **argv)' \
               'static int __extc_entry_main(int argc, char **argv)' \
               'int main(int argc, char **argv) {' 'strcmp(argv[1], "-m")' \
               'extc_module_usage(argv[0]);'; do
        grep -qF "$pat" "$tmp/m.c" || miss="$miss [$pat]"
    done
    [ -z "$miss" ] && ok "生成物：C main 是分发器 + 表里两个入口（其中一个叫 demo）" \
                   || bad "生成物结构" "缺:$miss"
    # 每个入口恰好一份原型 + 一份定义
    n=$(grep -cF 'static int __extc_entry_demo$run(int argc, char **argv);' "$tmp/m.c")
    [ "$n" = 1 ] && ok "入口助手有原型、内部链接（static）" || bad "入口原型" "找到 $n 份"
else
    bad "发射 tests/module/main.extc" "$(head -2 "$tmp/emit.log" | tr '\n' ' ')"
fi

# ---- ② 行为 ----
if [ -f "$tmp/m.c" ] && cc_build "$tmp/m.c" "$tmp/m"; then
    out=$("$tmp/m" 2>&1); rc=$?
    [ "$out" = "root args=1" ] && [ "$rc" = 0 ] && ok "不带 -m 跑根入口（$out, rc=$rc）" \
        || bad "默认入口" "期望 root args=1/rc=0，实得 [$out]/rc=$rc"
    out=$("$tmp/m" -m demo a b 2>&1); rc=$?
    [ "$out" = "demo args=3" ] && [ "$rc" = 7 ] \
        && ok "-m demo 分发 + 命令行原地压紧（argv[0] 仍是程序名：args=3）" \
        || bad "-m demo" "期望 demo args=3/rc=7，实得 [$out]/rc=$rc"
    out=$("$tmp/m" -m calc 2>&1); rc=$?
    [ -z "$out" ] && [ "$rc" = 3 ] && ok "-m calc 不带参数入口（rc=$rc，零输出）" \
        || bad "-m calc" "期望空输出/rc=3，实得 [$out]/rc=$rc"
    out=$("$tmp/m" --help 2>&1); rc=$?
    if printf '%s' "$out" | grep -qF -- '-m demo' \
       && printf '%s' "$out" | grep -qF -- '-m calc' \
       && printf '%s' "$out" | grep -qF 'prints how many arguments it got' \
       && printf '%s' "$out" | grep -qF 'exits "7"'; then
        [ "$rc" = 0 ] && ok "--help 列出 -m 名字与描述（rc=$rc）" || bad "--help rc" "rc=$rc"
    else
        bad "--help" "清单不完整：$(printf '%s' "$out" | head -3 | tr '\n' ' ')"
    fi
    out=$("$tmp/m" -m nope 2>"$tmp/e"); rc=$?
    case "$(cat "$tmp/e")" in
    *"unknown module 'nope'"*)
        [ "$rc" = 2 ] && ok "未知模块 exit 2 并给出可用清单" || bad "未知模块 rc" "rc=$rc" ;;
    *) bad "未知模块" "stderr=$(head -1 "$tmp/e")" ;;
    esac
    "$tmp/m" -m >/dev/null 2>"$tmp/e"; rc=$?
    case "$(cat "$tmp/e")" in
    *"-m needs a module name"*) [ "$rc" = 2 ] && ok "-m 缺名字 exit 2" || bad "-m 缺名字 rc" "rc=$rc" ;;
    *) bad "-m 缺名字" "stderr=$(head -1 "$tmp/e")" ;;
    esac
else
    bad "合同编译 main.extc" "$(head -2 "$tmp/cc.log" | tr '\n' ' ')"
fi

# ---- ③ 没有 main 的程序 ----
if "$EXTC" -w --no-line-map -o "$tmp/n.c" tests/module/nomain/main.extc 2>"$tmp/e" \
   && cc_build "$tmp/n.c" "$tmp/n"; then
    "$tmp/n" >"$tmp/o" 2>&1; rc=$?
    grep -qE 'usage: .*\[-m <module>\]' "$tmp/o" && [ "$rc" = 2 ] \
        && ok "没有 main：不带 -m 打印用法并 exit 2" \
        || bad "没有 main / 默认" "rc=$rc out=$(head -2 "$tmp/o" | tr '\n' ' ')"
    out=$("$tmp/n" -m only 2>&1); rc=$?
    [ "$out" = "only ran" ] && [ "$rc" = 5 ] && ok "没有 main：-m only 照常跑（rc=$rc）" \
        || bad "没有 main / -m" "期望 only ran/rc=5，实得 [$out]/rc=$rc"
    "$tmp/n" --help | grep -q 'Pass -m to pick one' \
        && ok "没有 main：--help 说明必须用 -m" || bad "没有 main / --help" "缺说明行"
else
    bad "合同编译 nomain" "$(head -2 "$tmp/e" | tr '\n' ' ')"
fi

# ---- ④ 零税 ----
if "$EXTC" -w --no-line-map -o "$tmp/p.c" tests/ops/conv_int_float.extc 2>/dev/null; then
    if grep -qE 'extc_module_entries|__extc_entry_|module dispatcher' "$tmp/p.c"; then
        bad "零税" "没有 @module 的程序里出现了分发器"
    else
        ok "零税：没有 @module 的程序生成物里没有分发器"
    fi
else
    bad "零税：发射普通程序" "编译失败"
fi

# ---- ⑤ 负例 ----
for d in tests/module/errors/*/; do
    name=$(basename "$d")
    expect=$(cat "$d/expect.txt")
    "$EXTC" -w -o /dev/null "$d/main.extc" >"$tmp/o" 2>&1; rc=$?
    if [ "$rc" = 0 ]; then
        bad "负例 $name" "应当编译期报错，却成功了"
    elif grep -qF "$expect" "$tmp/o"; then
        ok "负例 $name：$(printf '%s' "$expect" | cut -c1-52)"
    else
        bad "负例 $name" "期望含 [$expect]，实得 $(head -1 "$tmp/o")"
    fi
done

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ]
