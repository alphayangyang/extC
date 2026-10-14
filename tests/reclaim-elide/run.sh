#!/usr/bin/env bash
# tests/reclaim-elide/run.sh —— 「计数按编译期收窄」的**红判据语料**（只钉判据，不做门禁）
#
# 判据来源：`docs/RECLAMATION.md` 第五节最后一条 ——
#   「计数按编译期收窄：计数加不加由编译期决定，而不是一律加。能省的情形：对象所在区域
#     反正要被清扫、赋的是全新分配的对象（计数从零起、不可能归零）、编译器能证明该对象
#     还有别的持有者。收益在'大量指针赋值、极少分配'的代码里最大。」
# 度量口径：`prototype-reclaim/README.md`「写密集形状」—— 计数只挂**指针赋值**
#   （`p.val = x` 不记，`p.next = q` 记两次），单价 ≈ 0.11 ns / 指针赋值，且常开。
#
# 语义（同 tests/reclaim-shapes/、tests/arena-soundness/）：**只记录现状**。
# 该省的行在"计数照样出现"时标红 —— 这就是将来实现编译期收窄的判据；脚本照旧 `exit 0`。
#
# 指望（不看代码只看表）：
#   · 期望=该省  → 接线生效后**应为红**（该省却出现计数）；分析落地、计数被省掉才转绿。
#   · 期望=不该省 → 应有计数；接线生效后为绿，计数缺失为红。
#   · task-3（codegen 计数接线）没生效时生成 C 一处计数调用都没有 ⇒ **全是红**：
#     此时"该省"的红不是判据成立，而是判据还测不到 —— 表里注明，别读成绿。
#
# 用法：bash tests/reclaim-elide/run.sh    退出码 0（基线，不是门禁）
set -u
cd "$(dirname "$0")/../.."          # 仓库根：与 tests/reclaim-shapes/run.sh 同一约定

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 颜色只在真终端上加；绿/红两个字**始终**在文本里（管道里也能 grep）
if [ -t 1 ]; then G=$'\033[32m'; R=$'\033[31m'; O=$'\033[0m'; else G=; R=; O=; fi

SHAPES="elide_swept_region elide_fresh_alloc elide_other_holder writes_dense \
        keep_unknown_region keep_shared_overwrite keep_sole_holder keep_count_granularity \
        keep_other_holder_dies keep_two_edges \
        elide_readonly_recursion keep_callee_writes keep_indirect_callee"

# 计数调用点：只数**用户区**里 codegen 发射的那种形状（实参带 `(void` 转换）。两个坑都踩过：
#   ① 运行期 prelude 的**函数体**里有两条 `extc_rc_storeRef(slot, v);`（没有转换），
#      只排 `static void` 定义行排不掉它们 ⇒ 每个形状恒有 2 处"计数"：判据两个方向都是死的
#     · 定义行的排除**按形状**（含 `static`，或以 `{` 收尾），不钉字面 `static void`——
#       否则发射形态一改（例如 `static inline void`）定义行就被当成调用点，判据又变空转/误红。
#      （"该省"永远红、"不该省"被误省也永远绿）。`--selftest` 专门钉这一点。
#   ② 只认 `initRef|storeRef` 会漏掉 `storeRefOwned`/`storeRefUnknown`/`storeStructCons`
#      —— 指针赋值真正发的是后三个。
count_calls() { # <生成的 C> → 调用点行（标准输出）
    grep -nE '\bextc_rc_(initRef|storeRef|storeRefOwned|storeRefUnknown|storeStructCons)\(' "$1" 2>/dev/null \
        | grep -vE '(^|[^A-Za-z_])static([^A-Za-z_]|$)' \
        | grep -vE '\{[[:space:]]*$' \
        | grep '(void' || true
}

if [ "${1:-}" = "--selftest" ]; then
    # 判据自身的牙：合成的样本上，prelude 那种没有转换的调用**不许**被数进来，
    # 而用户区那三种发射形状**都必须**被数到。
    cat >"$TMP/self.c" <<'EOF'
static void extc_rc_initRef(void **slot, void *v) {
    *slot = v;
    extc_rc_inc(v);
}
static inline void extc_rc_storeRefOwned(void **slot, void *v, void *owner) {
    extc_rc_storeRef(slot, v);
}
int main(void) {
    extc_rc_initRef((void **)&x, (void *)x);
    extc_rc_storeRefOwned((void **)&(a->f), (void *)v, (void *)0);
    extc_rc_storeRefUnknown((void **)&(b->g), (void *)w);
    return 0;
}
EOF
    got=$(count_calls "$TMP/self.c" | wc -l | tr -d ' ')
    if [ "$got" = "3" ]; then
        echo "selftest ok：prelude 的 2 处不计（含一处 static inline 定义），用户区 3 处都数到"
        exit 0
    fi
    echo "selftest FAIL：期望 3 处用户区调用，数到 $got 处"
    count_calls "$TMP/self.c" | sed 's/^/    /'
    exit 1
fi

expect_of() { # <用例名> → 该省 / 不该省
    case "$1" in
        elide_*|writes_dense) echo "该省" ;;
        *)                    echo "不该省" ;;
    esac
}
want_of() { # <用例名> → 期望的程序输出（语义面）
    case "$1" in
        elide_swept_region)       echo "swept=28" ;;
        elide_fresh_alloc)        echo "fresh=42" ;;
        elide_other_holder)       echo "keep=7 alias=7" ;;
        writes_dense)             echo "dense=4096 allocs=4" ;;
        keep_unknown_region)      echo "unknown=3" ;;
        keep_shared_overwrite)    echo "a=1 slot=2" ;;
        keep_sole_holder)         echo "sole=9 cleared=true" ;;
        keep_count_granularity)   echo "gran=7" ;;
        keep_other_holder_dies)   echo "v=7" ;;   # 健全性：省错这条 ⇒ v=2
        keep_two_edges)           echo "x=5" ;;   # 健全性：只省一条边 ⇒ x=1
        elide_readonly_recursion) echo "n=0" ;;   # 跨过程 mayDec：纯读递归一行计数都不该有
        keep_callee_writes)       echo "v=0" ;;   # 健全性：被调者写槽 ⇒ 不许省（误省 ⇒ v=1）
        keep_indirect_callee)     echo "v=0" ;;   # 健全性：被调者解析不到 ⇒ 不许省（误省 ⇒ v=1）
    esac
}

echo "== tests/reclaim-elide：计数按编译期收窄 —— 红判据（exit 0 = 记录，不是门禁）=="
echo "   编译：EXTC_RECLAIM=1 $EXTC <形状>.extc -o <生成>.c；计数按生成 C 里"
echo "   用户区的 extc_rc_{initRef,storeRef,storeRefOwned,storeRefUnknown,storeStructCons} 调用点"
echo "   （实参带 (void 转换；运行期 prelude 的 2 处内部调用不计 —— 见 count_calls 与 --selftest）。"
echo

# ── 逐个形状：编译 → 生成 C 编过 → 跑 → 数计数调用 ────────────────────────────────
for n in $SHAPES; do
    fail_reason=""
    if ! EXTC_RECLAIM=1 "$EXTC" "tests/reclaim-elide/$n.extc" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1; then
        fail_reason="extc 编译失败: $(sed -n '1p' "$TMP/$n.cerr" | cut -c1-60)"
        : >"$TMP/$n.calls"
    elif ! "$CC" -fwrapv -O1 -o "$TMP/$n" "$TMP/$n.c" >"$TMP/$n.ccerr" 2>&1; then
        fail_reason="生成的 C 编不过: $(sed -n '1p' "$TMP/$n.ccerr" | cut -c1-60)"
        : >"$TMP/$n.calls"
    else
        out=$("$TMP/$n" 2>&1); rc=$?
        printf '%s\n' "$out" >"$TMP/$n.out"
        want=$(want_of "$n")
        if [ "$rc" -ne 0 ]; then
            fail_reason="跑挂 rc=$rc"
        elif [ "$out" != "$want" ]; then
            fail_reason="输出不符：得到 [$out]，要 [$want]"
        fi
        # 计数调用点：口径与理由见上面的 `count_calls`（`--selftest` 钉它的牙）。
        count_calls "$TMP/$n.c" >"$TMP/$n.calls"
        # 判据不许空转：发射形状若变了（这里一处都抓不到），下面按 `wired=no` 处理并让闸门 rc=2。
    fi
    printf '%s\n' "$fail_reason" >"$TMP/$n.fail"
done

# 接线是否生效：任何一处调用点 ⇒ 生效。全 0 ⇒ task-3 的 codegen 接线还没落地。
all_calls=0
for n in $SHAPES; do
    c=$(wc -l <"$TMP/$n.calls" | tr -d ' '); all_calls=$((all_calls + c))
done
if [ "$all_calls" -gt 0 ]; then wired=yes; else wired=no; fi

# ── 判据 ──────────────────────────────────────────────────────────────────────
table=$TMP/table.tsv
: >"$table"
row() { # 名称 期望 实际计数 颜色 判据 备注 wired
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$wired" >>"$table"
}

for n in $SHAPES; do
    disp=${n//_/-}
    ex=$(expect_of "$n")
    calls=$(wc -l <"$TMP/$n.calls" | tr -d ' ')
    init=$(grep -c 'extc_rc_initRef' "$TMP/$n.calls" 2>/dev/null || true)
    store=$(grep -c 'extc_rc_storeRef' "$TMP/$n.calls" 2>/dev/null || true)
    [ -n "$init" ] || init=0; [ -n "$store" ] || store=0
    counts="$calls 处 (init $init · store $store)"
    fail_reason=$(cat "$TMP/$n.fail")

    # 粒度断言（只对 K4）：数据字段赋值行上不许出现计数调用
    data_rc=0
    if [ -s "$TMP/$n.c" ]; then
        data_rc=$(grep -nE '\->val[[:space:]]*=' "$TMP/$n.c" 2>/dev/null \
                  | grep -cE 'extc_rc_(initRef|storeRef)\(' || true)
        [ -n "$data_rc" ] || data_rc=0
    fi

    color=$R; verdict=红; note=""
    if [ -n "$fail_reason" ]; then
        note="$fail_reason"
    elif [ "$ex" = "该省" ]; then
        if [ "$calls" -gt 0 ]; then
            note="该省却出现 $calls 处计数：无分析 ⇒ 未收窄"
        elif [ "$wired" = yes ]; then
            color=$G; verdict=绿; note="计数已省（收窄生效）"
        else
            note="接线未生效（task-3）：0 处计数=计数还没发射，**不是**省掉了；接线后此处应为红"
        fi
    else
        if [ "$n" = "keep_count_granularity" ] && [ "$data_rc" -gt 0 ]; then
            note="数据字段赋值行上出现 $data_rc 处计数 ⇒ 口径错（计数只该挂指针赋值）"
        elif [ "$calls" -gt 0 ]; then
            color=$G; verdict=绿; note="应有计数，出现 $calls 处"
        elif [ "$wired" = yes ]; then
            note="应有计数却为 0：被误省"
        else
            note="接线未生效（task-3）：应有计数，当前 0 处（接线后应转绿）"
        fi
    fi
    [ -n "$fail_reason" ] && { color=$R; verdict=红; }
    row "$disp" "$ex" "$counts" "$color" "$verdict" "$note"
done

# ── 表 ────────────────────────────────────────────────────────────────────────
print_table() {
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$table" <<'PY'
import re, sys, unicodedata
ansi = re.compile(r'\x1b\[[0-9;]*m')
def w(s):
    s = ansi.sub('', s)
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in s)
head = ['形状', '期望', '实际计数', '判据']
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8')]
cells = [head] + [[r[0], r[1], r[2],
                   f'{r[3]}{r[4]}' + ('\x1b[0m' if r[3] else '') + f'  {r[5]}']
                  for r in rows]
width = [max(w(c[i]) for c in cells) for i in range(3)]
def line(c):
    return '  '.join(c[i] + ' ' * (width[i] - w(c[i])) for i in range(3)) + '  ' + c[3]
print(line(head))
print('  '.join('-' * width[i] for i in range(3)) + '  ' + '-' * 48)
for c in cells[1:]:
    print(line(c))
PY
    else
        # 退路：中文出现在「期望」与末列 ⇒ 不做像素级对齐，一行一条
        printf '%s\n' "形状	期望	实际计数	判据"
        while IFS=$'\t' read -r a b c d e f g; do
            printf '%s\t%s\t%s\t%s%s%s  %s\n' "$a" "$b" "$c" "$d" "$e" "$O" "$f"
        done <"$table"
    fi
}
print_table

# ── 机器可读的判定表（给 gate.py 用）──────────────────────────────────────────
# 不设 `ELIDE_TSV` 就什么都不写 —— 本脚本自身仍是"只记录"的基线（exit 0）。
if [ -n "${ELIDE_TSV:-}" ]; then
    cp "$table" "$ELIDE_TSV"
    echo "判定表 → $ELIDE_TSV（列：形状 期望 计数 颜色 判定 备注 wired）"
fi

# ── 计数调用出现的位置（生成 C 行号）─────────────────────────────────────────────
echo
echo "== 计数调用出现的位置（生成 C 行号；每形状最多列 4 处）=="
for n in $SHAPES; do
    disp=${n//_/-}
    out=$(cat "$TMP/$n.out" 2>/dev/null || true)
    printf '  %-24s out=[%s]\n' "$disp" "$out"
    if [ -s "$TMP/$n.calls" ]; then
        total=$(wc -l <"$TMP/$n.calls" | tr -d ' ')
        sed -n '1,4p' "$TMP/$n.calls" | while IFS=: read -r ln rest; do
            printf '      L%-5s %s\n' "$ln" "$(printf '%s' "$rest" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | cut -c1-88)"
        done
        [ "$total" -gt 4 ] && echo "      …（共 $total 处）"
    else
        echo "      (无计数调用)"
    fi
done

# ── 接线状态 ──────────────────────────────────────────────────────────────────
echo
if [ "$wired" = yes ]; then
    echo "接线（task-3）：**已生效**（$all_calls 处调用点）。该省的行仍红 ⇒ 编译期收窄尚未实现。"
else
    echo "接线（task-3）：**未生效** —— 生成 C 里 0 处计数调用（只有运行期 prelude 里的定义）。"
    echo "  所以本表全部标红：对「不该省」是「应有计数却没有」；对「该省」是「判据还测不到」。"
    echo "  两条都依赖 codegen 的计数接线（task-3）；接线落地后本脚本会自己重新判定。"
fi
echo
echo "红 = 该形状的判据没成立；绿 = 成立。本目录是**基线**：exit 0，不接进 check.sh 的门禁。"
echo "详见 README.md（每类形状为什么该省/不该省，逐条引用 docs/RECLAMATION.md 第五节）。"
exit 0
