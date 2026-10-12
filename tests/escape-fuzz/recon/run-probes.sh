#!/usr/bin/env bash
# tests/escape-fuzz/recon/run-probes.sh —— task-9 勘察的可复跑实验集。
#
# 只读 src/、只写 recon/out/（+ /tmp 临时产物）；不改仓库任何一行源码。
# 退出码恒为 0（这是证据，不是门禁）。用法：
#     bash tests/escape-fuzz/recon/run-probes.sh
set -u
cd "$(dirname "$0")/../../.."          # 仓库根
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
PROBES=tests/escape-fuzz/recon/probes
OUT=tests/escape-fuzz/recon/out
CASES=tests/escape-fuzz/cases
mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 编译 + 跑（带 ASan），输出 stdout/stderr 到指定文件；打印一行结论。
# 用法： run_extc <file.extc> <tag>
run_extc() {
    local f=$1 tag=$2
    "$EXTC" "$f" -o "$TMP/$tag.c" >"$TMP/$tag.cerr" 2>&1
    local rc=$?
    echo "== $tag  ($f)  extc_rc=$rc"
    if [ $rc -ne 0 ]; then
        grep -m1 -E "error:" "$TMP/$tag.cerr" | sed 's/^/   /'
        return 0
    fi
    if $CC -fwrapv -O1 -fsanitize=address -o "$TMP/$tag" "$TMP/$tag.c" >>"$TMP/$tag.cerr" 2>&1; then
        "$TMP/$tag" >"$TMP/$tag.out" 2>"$TMP/$tag.runerr"
        local rrc=$?
        local san
        san=$(grep -m1 -o "ERROR: AddressSanitizer: [a-z-]*" "$TMP/$tag.runerr" || echo CLEAN)
        echo "   run_rc=$rrc  sanitizer=$san  stdout=$(tr '\n' '|' <"$TMP/$tag.out" | cut -c1-40)"
    else
        echo "   generated C did not compile"
    fi
    return 0
}

echo "############ Q1: CALLRES / ARG_DEPTH —— 调用结果的深度"
{
    for p in q1_direct q1_binding q1_loop q1_frame_nonescape q1_inloop_nonescape q1_assign_noescape q1_homefn_loop; do
        run_extc "$PROBES/$p.extc" "$p"
    done
    echo
    echo "-- q1_direct / q1_frame_nonescape / q1_inloop_nonescape / q1_homefn_loop 的生成物：调用传了哪只 arena"
    for p in q1_direct q1_frame_nonescape q1_inloop_nonescape q1_homefn_loop; do
        "$EXTC" "$PROBES/$p.extc" -o "$TMP/$p.c" >/dev/null 2>&1 || true
        printf '%-24s %s\n' "$p" "$(grep -E 'mk\(' "$TMP/$p.c" 2>/dev/null | grep -vE 'static|void |^\s*node \* mk' | grep -m1 -oE 'mk\([^)]*\)')"
        printf '%-24s %s\n' "" "$(grep -m1 -oE '(static node \*|int64_t) *(mk|build|main)\([^)]*\)' "$TMP/$p.c" 2>/dev/null)"
    done
    echo
    echo "-- promoteInto2 对调用结果的 gate（EXTC_DBG_ZONE，只看 mk 那一行）"
    EXTC_DBG_ZONE=1 "$EXTC" "$PROBES/q1_loop.extc" -o /dev/null 2>&1 | grep "gate] call=mk" | head -3
    echo
    echo "-- 记录绑定深度的代码位置：check_stmt.c:481-499（见 RECON.md 引用）"
    sed -n '481,499p' src/check/check_stmt.c
} >"$OUT/q1-callres.txt"

echo "############ Q2: A0_STORE 词序敏感"
{
    for n in m_main_dfield_direct_before m_main_dfield_direct_after; do
        run_extc "$CASES/$n.extc" "$n"
    done
    run_extc "$PROBES/q2_after.extc" q2_after
    run_extc "$PROBES/q2_after_deadassign.extc" q2_after_deadassign
    echo
    echo "-- 两个形状的差别只有一行：q2_after_deadassign 在 store 之前多一条 if i < 0 { cur = p }（运行期永不执行）"
    diff <(cat "$PROBES/q2_after.extc") <(cat "$PROBES/q2_after_deadassign.extc") | sed 's/^/   /'
    echo
    echo "-- DFA（check_dataflow.c，check_top.c:4206 跑在 body 之后）对 cur 的答案：两个形状都一样"
    for n in m_main_dfield_direct_before m_main_dfield_direct_after; do
        echo "   [$n]"
        EXTC_DBG_DFA=1 "$EXTC" "$CASES/$n.extc" -o /dev/null 2>&1 | grep -A4 '\[dfa\] main' | sed 's/^/     /'
    done
    echo
    echo "-- EXTC_DBG_DEFER：把拒绝记录到求解之后再比一次（只比**值**的深度，不比目的地 at）"
    EXTC_DBG_DEFER=1 "$EXTC" "$CASES/m_main_dfield_direct_after.extc" -o /dev/null 2>&1 | grep '\[defer\]' | head -3
    echo
    echo "-- 目的地深度怎么算：check_escape.c:125 与 178-192（storeLayer→slotDepth→targetDepth）"
    sed -n '125,125p;178,193p' src/check/check_escape.c
} >"$OUT/q2-ordering.txt"

echo "############ Q3: A0_RET + 5 例 UAF —— 投影 / 放置"
{
    for p in A_ff1_new B_ff2_new B2_preexisting C1_two_stage C_ff2_mk D_arr_new E_arr_mk F_direct G_bind H_ff1_mk; do
        run_extc "$PROBES/$p.extc" "$p"
    done
    echo
    echo "-- minAt / 自己所在函数有没有 home（EXTC_DBG_MINAT + EXTC_DBG_HOME）"
    for p in A_ff1_new B_ff2_new B2_preexisting C1_two_stage; do
        echo "   [$p]"
        EXTC_DBG_MINAT=1 EXTC_DBG_HOME=1 "$EXTC" "$PROBES/$p.extc" -o /dev/null 2>&1 \
            | grep -E "\[minAt\] build|\[home\] build" | sed 's/^/     /'
    done
    echo
    echo "-- 字段来源记录：noteFieldSrc 只写**已存在**的槽（EXTC_DBG_FS）"
    for p in A_ff1_new B_ff2_new B2_preexisting C1_two_stage; do
        printf '   %-16s fs-lines=%s %s\n' "$p" \
            "$(EXTC_DBG_FS=1 "$EXTC" "$PROBES/$p.extc" -o /dev/null 2>&1 | grep -c '\[fs\] bx')" \
            "$(EXTC_DBG_FS=1 "$EXTC" "$PROBES/$p.extc" -o /dev/null 2>&1 | grep -m1 '\[fs\] bx')"
    done
    echo
    echo "-- 5 例 UAF：所在函数 / minAt / arena / 生成物里 new 落在哪只 arena"
    for n in m_home_pfield_direct m_home_cont_after m_home_ff_direct; do
        echo "   [$n]"
        EXTC_DBG_MINAT=1 EXTC_DBG_HOME=1 "$EXTC" "$CASES/$n.extc" -o "$TMP/$n.c" 2>&1 \
            | grep -E "\[minAt\] (build|main)|\[home\] (build|main)" | sed 's/^/     /'
        grep -m1 -oE 'static node \* (build|main)\([^)]*\)' "$TMP/$n.c" 2>/dev/null | sed 's/^/     sig: /'
        grep -nE 'extc_arena_alloc\(&[^,]*' "$TMP/$n.c" 2>/dev/null | sed 's/^/     /' | head -4
    done
    echo
    echo "-- promoteInto2 对 EX_FIELD / EX_INDEX 的走法：check_escape.c:1095-1100"
    sed -n '1095,1100p' src/check/check_escape.c
    echo
    echo "-- 放置那一趟：check_top.c:6071-6128（minAt==0→HOME / minAt>=1→块层）"
    sed -n '6098,6114p' src/check/check_top.c
} >"$OUT/q3-projections.txt"

echo "############ 反向发现：new 的深度 == 目的地深度时，max-join 会漏收（新洞）"
{
    run_extc "$PROBES/q2_dualpointee.extc" q2_dualpointee
    echo
    echo "-- DFA 对 f 的答案（max join）：cur depth=1"
    EXTC_DBG_DFA=1 "$EXTC" "$PROBES/q2_dualpointee.extc" -o /dev/null 2>&1 | grep -A4 '\[dfa\] f:' | sed 's/^/   /'
    echo
    echo "   cur 可能指向参数（调用者的存储，深度 0），也可能指向帧内 new（深度 1）。"
    echo "   目的地要的是「可能活得最久的被指物」= min=0；记录的是 max=1 ⇒ 放到调用者存储里的 q 出帧即死。"
} >"$OUT/q4-dualpointee-hole.txt"

echo "############ EXTC_DBG=1 兜底普查（promoteFieldsAt 预算耗尽 = rc 134）"
{
    echo "-- EXTC_DBG=1 下逐例 rc（只列 rc=134 的）"
    for f in "$CASES"/*.extc; do
        b=$(basename "$f" .extc)
        EXTC_DBG=1 "$EXTC" "$f" -o /dev/null >/dev/null 2>"$TMP/$b.dbg" 2>/dev/null
        rc=$?
        [ "$rc" = "134" ] && printf '   rc=134  %-34s %s\n' "$b" "$(grep -m1 -oE '\[fallback\][^,;]*' "$TMP/$b.dbg")"
    done
    echo "   （这些是既存兜底，不属于本任务的 7 类；修 promoteFieldsAt 时要重新核对这张表）"
} >"$OUT/q5-excc-dbg-survey.txt"

echo "############ 风险面清单（静态，不跑套件）"
{
    echo "-- 逃逸健全性闸门与其基线（棘轮：修好一条删一条）"
    sed -n '1,8p' tools/gate-escape-known-bad.txt
    echo "-- ASan 闸门基线"
    sed -n '1,6p' tools/gate-asan-known-bad.txt
    echo "-- 相关语料目录"
    ls -d tests/arena-soundness tests/arena-promoted tests/pool-soundness tests/reclaim-shapes tests/pool tests/stl 2>/dev/null
    echo "-- check.sh 里与逃逸/ASan 有关的节"
    grep -nE "闸门·|断言版|arena|sanit" check.sh | head -20
} >"$OUT/q6-risk-surface.txt"

echo "############ 关键代码位置（grep -n，供 RECON.md 引用）"
{
    for pat in \
        "check_stmt.c:415:markCallHomeIfEscaping" \
        ; do :; done
    echo "-- 调用点 arena 的选定"
    grep -n "markCallHomeIfEscaping(c, s->u.var.init" src/check/check_stmt.c
    grep -n "void markCallHomeIfEscaping" src/check/check_top.c
    sed -n '1435,1441p' src/check/check_top.c
    grep -n "void setCallArenaArg" src/check/check_top.c
    sed -n '1519,1542p' src/check/check_top.c
    echo
    echo "-- 绑定深度的记录 / 读取"
    grep -n "anSetRefDepth(sym, d)" src/check/check_stmt.c
    grep -n "anSetRefDepth(slot0, targetDepth" src/check/check_stmt.c
    grep -n "int at0 = storeLayer" src/check/check_stmt.c
    grep -n "checkEscape(c, v, at0" src/check/check_stmt.c
    grep -n "return slotDepth(c, e);" src/check/check_escape.c
    echo
    echo "-- promoteInto2 的分支"
    grep -n "case EX_FIELD:" src/check/check_escape.c
    grep -n "if (!sy || !sy->origin) return false;" src/check/check_escape.c
    grep -n "promoteFieldsAt(c, sy, at, hops + 1)" src/check/check_escape.c
    grep -n "calleeMakesPool(planCallee(val))" src/check/check_escape.c
    echo
    echo "-- 字段来源 / 放置 / DFA"
    grep -n "void noteFieldDepthWrite" src/check/check_top.c
    grep -n "static void noteFieldSrc" src/check/check_top.c
    grep -n "dfAnalyze(c, f, &dfr)" src/check/check_top.c
    grep -n "if (d > anRefDepth(sy)) anSetRefDepth(sy, d);" src/check/check_top.c
    grep -n "if (anMinAt(site) == 0)" src/check/check_top.c
    grep -n "closeReach(&closeCtx, REACH_HOME" src/check/check_top.c
    grep -n "if (!site->arenaArgPending" src/check/check_top.c
    echo
    echo "-- 新 site 的初始层与 refDepth"
    grep -n "planSetArenaLevel(e, (c->curFunc && anNeedsHome(c->curFunc))" src/check/check_expr.c
    echo
    echo "-- 唯一读 planArenaLevel 的 codegen 位置"
    grep -n "arenaRefAt(g, planArenaLevel(e))" src/back/codegen.c
} >"$OUT/q7-citations.txt"

echo
echo "写出："
ls -l "$OUT" | sed 's/^/   /'
echo "全部实验的原始输出在上面 7 个文件里；RECON.md 的每一条结论都指向其中之一。"
exit 0
