#!/usr/bin/env bash
# tests/modules/run.sh —— **模块系统（定案 70）的常设验收**
#
# 两条判据，缺一不可：
#   ① 正例：**一个模块 = 一个文件**，`use a::b` + `a::name` 跨模块引用必须跑得通
#      · `samenames` 是**模块 mangle 的验收**：两个模块逐声明重名也必须互不干扰 ✓
#   ② 反例：该挡的必须**编译期**挡住，而且消息要指对文件、说清怎么办 ✓
#      （未 use · @private · 漏限定名 · 环 · 文件不存在 · 模块里写 main · 同名类型歧义）
set -u
cd "$(dirname "$0")/../.."

EXTC=./build/extc
fail=0

echo "== 正例（多文件程序：一个模块 = 一个文件）=="
for d in tests/modules/hello tests/modules/chain tests/modules/samenames tests/modules/crossmod; do
    name=$(basename "$d")
    if ! out=$("$EXTC" --run "$d/main.extc" 2>&1); then
        echo "  FAIL $name  ->  编译/运行失败"; echo "$out" | sed 's/^/        /' | head -6; fail=1; continue
    fi
    # `// expect: a|b|c` —— 每一段都必须出现在输出里 ✓（跟 examples 一个规矩）
    want=$(grep -o '// expect:.*' "$d/main.extc" | sed 's|// expect: *||' | head -1)
    ok=1
    if [ -n "$want" ]; then
        IFS='|' read -ra parts <<< "$want"
        for p in "${parts[@]}"; do
            echo "$out" | grep -qF -- "$p" || ok=0
        done
    fi
    if [ "$ok" = 1 ]; then
        echo "  ok   $name  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL $name  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
done

echo "== 反例（必须编译期挡住，而且消息要指对文件）=="
check_err() {   # check_err 目录名 消息里必须出现的关键字...
    local d=$1 want=$2 out
    if out=$("$EXTC" "tests/modules/errors/$d/main.extc" -o /dev/null 2>&1); then
        echo "  FAIL $d  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    if ! echo "$out" | grep -qF -- "$want"; then
        echo "  FAIL $d  ->  消息里没有「$want」"; echo "$out" | sed 's/^/        /' | head -3; fail=1; return
    fi
    echo "  ok   $d  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-88)"
}
check_err not-imported   "not imported here"
check_err private        "is private to module"
check_err unqualified    "write \`lib::open\`"
check_err cycle          "import cycle"
check_err missing-file   "cannot find module"
check_err main-in-module "must live in the entry file"
check_err ambiguous-type  "ambiguous type \`pair\`"

# ⭐ **判据③：诊断里不许出现 mangle 名**（`$` 在 extC 标识符里不合法 ⇒
#    消息里出现 `$` 就一定是把内部编码漏给了用户 ✗）
#    为什么单独抓一条：这类泄漏**不影响编译结果**，只有人眼才看得出来
#    （踩过：`struct \`alpha$pair\` has no field \`zzz\``）✓
echo "== 判据③：诊断里不许泄漏 mangle 名（消息里不许出现 \$）=="
leak=0
for d in tests/modules/errors/*/ tests/modules/samenames/; do
    [ -f "$d/main.extc" ] || continue
    out=$("$EXTC" "$d/main.extc" -o /dev/null 2>&1 || true)
    # 判据：`$` 夹在标识符字符之间（`alpha$pair`）才叫泄漏；
    # `$EXTC_STD` 这种**环境变量名**是正常文案 ⇒ 前后要是标识符字符 ✓
    if echo "$out" | grep -qE '[A-Za-z0-9_]\$[A-Za-z0-9_]'; then
        echo "  FAIL $(basename "$d")  ->  诊断里出现了 mangle 名（\$）"
        echo "$out" | grep -E '[A-Za-z0-9_]\$[A-Za-z0-9_]' | head -2 | sed 's/^/        /'
        leak=1
    fi
done
# 正例也不能漏（含跨模块的类型/枚举/泛型实例 —— 那几种最容易漏 ✓）
for d in tests/modules/hello tests/modules/chain tests/modules/samenames tests/modules/crossmod; do
    out=$("$EXTC" "$d/main.extc" -o /dev/null 2>&1 || true)
    if echo "$out" | grep -qE '[A-Za-z0-9_]\$[A-Za-z0-9_]'; then
        echo "  FAIL $(basename "$d")(正例)  ->  输出里出现了 mangle 名（\$）"
        echo "$out" | grep -E '[A-Za-z0-9_]\$[A-Za-z0-9_]' | head -2 | sed 's/^/        /'
        leak=1
    fi
done
[ "$leak" = 0 ] && echo "  ok   所有模块测试的诊断/输出里都没有 \$ ✓" || fail=1

exit $fail
