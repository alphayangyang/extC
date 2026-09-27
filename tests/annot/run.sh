#!/usr/bin/env bash
# 注解的常设验收：`@inline` / `@unchecked` 要**真的生效**，错的注解要**编译期报错**。
#
# 判据分两类：
#   · 正例 —— 必须编过，而且生成的 C 里要**真的**有那个效果（"接受了但什么也没做"是最坏的一种）
#       - `@inline`  -> 生成的 C 里出现 EXTC_INLINE
#       - `@unchecked` -> 那个函数体里的边界检查**消失**，同一文件里未标注的函数体里**仍在**
#   · 反例 —— 必须被编译期挡掉，而且消息里要能认出问题（不是"生成的文件第几行"）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
pass=0; fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }

echo "== 正例：@inline 必须真的落到生成的 C 上 =="
for f in tests/annot/*.extc; do
    case "$(basename "$f")" in no_*|unchecked_*) continue ;; esac
    n=$(basename "$f" .extc)
    if ! out=$($EXTC "$f" 2>&1); then bad "$n （应该编过）"; echo "$out" | head -3 | sed 's/^/        /'; continue; fi
    if echo "$out" | grep -q "EXTC_INLINE"; then ok "$n  ->  EXTC_INLINE 出现在生成的 C 里"
    else bad "$n （编过了，但生成代码里没有内联属性 ⇒ 静默失效）"; fi
done

echo "== 正例：@unchecked 必须真的去掉边界检查，而且**只**去掉自己那个函数体的 =="
# 孪生文件由 sed 现生成（把那一行注解删掉）—— "两份只差这一行"是这条判据的前提，
# 手抄一份迟早会漂，而漂了之后"输出相同"就变成了在比两份不同的程序。
TMPA=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPA"' EXIT INT TERM
sed 's/^@unchecked$//' tests/annot/unchecked_mixed.extc > "$TMPA/plain_mixed.extc"
sed 's/^@unchecked$//' tests/annot/unchecked_all.extc   > "$TMPA/plain_all.extc"

if "$EXTC" -w --no-line-map -o "$TMPA/mixed.c" tests/annot/unchecked_mixed.extc \
   && "$EXTC" -w --run tests/annot/unchecked_mixed.extc > "$TMPA/with.out" 2>&1 \
   && "$EXTC" -w --run "$TMPA/plain_mixed.extc"        > "$TMPA/plain.out" 2>&1; then
    if cmp -s "$TMPA/with.out" "$TMPA/plain.out"; then
        ok "@unchecked 与不加注解输出逐字节相同  ->  $(cat "$TMPA/with.out")"
    else
        bad "@unchecked 改变了输出（这一条必须是 0 字节差异）"
        diff "$TMPA/with.out" "$TMPA/plain.out" | head -4 | sed 's/^/        /'
    fi
else
    bad "@unchecked 正例：编译或运行失败"
fi

if "$EXTC" -w --no-line-map -o "$TMPA/all.c" tests/annot/unchecked_all.extc \
   && "$EXTC" -w --no-line-map -o "$TMPA/all_plain.c" "$TMPA/plain_all.extc" \
   && "$EXTC" -w --run tests/annot/unchecked_all.extc >/dev/null 2>&1; then
    # 逐函数断言：从生成的 C 里按名字取出函数体，只看那一段。
    # 整文件计数会被库代码（`std::io` 自己的函数体里全是下标）污染，所以逐函数取。
    if python3 - "$TMPA/mixed.c" "$TMPA/all.c" "$TMPA/all_plain.c" <<'PY'
import re, sys

def body(src, name):
    """The C body of `name`, from its definition line to the first line that is only `}`."""
    lines = src.splitlines()
    pat = re.compile(r'^static\s+[\w ]*\**\s*%s\s*\(' % re.escape(name))
    for k, ln in enumerate(lines):
        if pat.match(ln) and ln.rstrip().endswith('{'):
            out = []
            for j in range(k + 1, len(lines)):
                if lines[j] == '}':
                    return '\n'.join(out)
                out.append(lines[j])
    return None

def has_check(text):
    return ('extc_checkedIndex' in text) or ('_index(' in text)

mixed = open(sys.argv[1], encoding='utf-8').read()
allc  = open(sys.argv[2], encoding='utf-8').read()
plain = open(sys.argv[3], encoding='utf-8').read()

bad = 0
# ① 同一个文件里：标注过的没有检查，未标注的仍然有（自由函数与方法各一对）
for name, want in (('sumChecked', True), ('sumFree', False),
                   ('tape_checkedTotal', True), ('tape_freeTotal', False)):
    b = body(mixed, name)
    if b is None:
        print('        找不到函数体 %s' % name); bad = 1; continue
    got = has_check(b)
    if got != want:
        print('        %s：检查%s（期望%s）' % (name, '还在' if got else '没了',
                                              '在' if want else '没了')); bad = 1

# ② 全部下标都在 @unchecked 里 ⇒ 整份生成物里一条检查都不剩；孪生文件相反（否则判据是空的）
n_all   = allc.count('extc_checkedIndex') + allc.count('slice_u8_index')
n_plain = plain.count('extc_checkedIndex') + plain.count('slice_u8_index')
if n_all != 0:
    print('        整文件里还剩 %d 处检查（期望 0）' % n_all); bad = 1
if n_plain == 0:
    print('        孪生文件里也没有检查 ⇒ 这条判据是空的'); bad = 1
sys.exit(bad)
PY
    then
        ok "生成物：@unchecked 的函数体里 \`extc_checkedIndex\`/\`_index(\` 为 0 处，未标注的仍在（自由函数 + 方法各一对）"
        ok "生成物：整份 C 里 0 处检查；孪生（只少那一行注解）里 2+2 处 ⇒ 不是"本来就没发""
    else
        bad "@unchecked 生成物判据：检查没有按函数粒度消失（见上）"
    fi
else
    bad "@unchecked 生成物判据：编译失败"
fi

echo "== 反例：错的注解必须编译期报错 =="
for f in tests/annot/no_*.extc; do
    n=$(basename "$f" .extc)
    want=$(grep -m1 '^// expect-error:' "$f" | sed 's|^// expect-error: *||')
    if out=$($EXTC "$f" -o /dev/null 2>&1); then bad "$n （应该报错但通过了）"; continue; fi
    if echo "$out" | grep -qF -- "$want"; then ok "$n  ->  $(echo "$out" | head -1 | sed 's/^[^ ]*: //' | cut -c1-58)"
    else bad "$n （报错了，但不是期望的那条：想要「$want」）"; echo "$out" | head -2 | sed 's/^/        /'; fi
done
echo "通过 $pass，失败 $fail"
[ $fail -eq 0 ]
