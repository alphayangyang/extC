#!/usr/bin/env python3
"""第 1 步（A1 + A2）落地脚本 —— 幂等、可审、可复跑。

用法： python3 tools/arena-fix/step1.py <repo-root>
（在**干净 HEAD** 的副本上跑；跑完 make + ./tests/run.sh 即可验收）

改动清单（每一条都对应 ARENA-SOUNDNESS §9 档 0的一件事）：
  A1-1  check_top.c  refreshRootDepth             ：根只许长大（取 max，不覆盖）
  A1-2  check_top.c  noteFieldDepthWrite 元素写   ：不许清表 / 不许覆盖 otherDepth
  A1-3  check_stmt.c 引用型换指向的 refDepth      ：覆盖 → 取 max
  A1-4  check_stmt.c 整值赋值的 refDepth          ：覆盖 → 取 max
  A2-1  check_escape.c  新增 valDepthStructural    ：按**值结构**算上界（不看类型）
  A2-2  check_escape.c  新增 valDepthForStore      ：exprRefDepth ∪ 结构上界
  A2-3  check_stmt.c   所有"记深度"的点改走 valDepthForStore
  A2-4  check_stmt.c   普通赋值那一支补齐字段表（以前完全没记）
  A2-5  check_escape.c EX_IDENT/EX_FIELD 读：再取一次"各字段上界"
  A2-6  check_stmt.c   if / while 合流取 max（快照 + 合并）
"""
import sys, os, re

BLKDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blocks')
def BLK(name):
    return open(os.path.join(BLKDIR, name + '.txt')).read()

def edit(path, pairs, must=True):
    s = open(path).read()
    for old, new in pairs:
        if old not in s:
            if must:
                raise SystemExit("模式未命中：%s\n---\n%s" % (path, old[:200]))
            continue
        s = s.replace(old, new, 1)
    open(path, 'w').write(s)

def main():
    root = sys.argv[1]
    es = os.path.join(root, 'src', 'check_escape.c')
    st = os.path.join(root, 'src', 'check_stmt.c')
    tp = os.path.join(root, 'src', 'check_top.c')
    ih = os.path.join(root, 'src', 'check_internal.h')

    # ── A1-1 根只许长大 ────────────────────────────────────────────────
    edit(tp, [("""static void refreshRootDepth(Sym *s) {
    if (!s) return;
    int m = s->otherDepth;
    for (int i = 0; i < s->nfields; i++) if (s->fields[i].depth > m) m = s->fields[i].depth;
    s->refDepth = m;
}""",
"""static void refreshRootDepth(Sym *s) {
    if (!s) return;
    int m = s->otherDepth;
    for (int i = 0; i < s->nfields; i++) if (s->fields[i].depth > m) m = s->fields[i].depth;
    /* ⭐ A1（ARENA-SOUNDNESS §9 档 0）：**根的记账深度只许长大**（上界性）。
     * 允许它下降 ⇒ "先把深的存进 A 格、再往 B 格写个浅的"就把整根压小，
     * 而浅的那一格**没有**抹掉 A 格里还活着的指针 ⇒ 之后整值拷贝/返回被放行 ✗
     * （反例 A/B/C：ASan heap-use-after-free ✓）*/
    if (s->refDepth > m) m = s->refDepth;
    s->refDepth = m;
}""")])

    # ── A1-2 元素写不许清表 ────────────────────────────────────────────
    edit(tp, [("""    if (!field) {                                  /* 整块赋值 / 元素写 ⇒ 保守 */
        if (!root->addressed) { root->nfields = 0; root->otherDepth = d2; }
        else if (d2 > root->otherDepth) root->otherDepth = d2;""",
"""    if (!field) {                                  /* 整块赋值 / 元素写 */
        /* ⭐ A1：**不许清表**、不许覆盖 otherDepth —— 元素写不会让别的元素/字段失效 ✗
         * （旧行为：`a[1] = x; a[0] = null` ⇒ 清表 + otherDepth 覆盖成 0 ⇒ 整块逃逸被放行 ✗）*/
        if (d2 > root->otherDepth) root->otherDepth = d2;""")])

    # ── A2-1 / A2-2 两条新函数 ─────────────────────────────────────────
    edit(es, [("int exprRefDepth(Checker *c, Expr *e) {",
"""/* ⭐ A2（ARENA-SOUNDNESS §9 档 0）：**按值的结构**算上界 —— 不看类型。
 *
 * 为什么必须有：`exprRefDepth` 的早退条件是 `typeContainsRef(类型)`，
 * 而**字段类型本身是 `ref`** 时它答 false ⇒ `inner { v: ref local }` 这种
 * 值里装着活指针的表达式会被算成 0 ✗（反例 B_field_table_stale）。
 * 这条只看语法结构，只可能把上界**抬高** ⇒ 方向安全 ✓ */
int valDepthStructural(Checker *c, Expr *e) {
    if (!e) return 0;
    int d = 0;
    switch (e->kind) {
    case EX_IDENT: {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return sy->refDepth;
        return 0;
    }
    case EX_REF:    return placeDepth(c, e->u.ref.operand);
    case EX_DEREF:  return valDepthStructural(c, e->u.deref.operand);
    case EX_SIGN:   return valDepthStructural(c, e->u.sign.operand);
    case EX_COALESCE:
        return maxInt(valDepthStructural(c, e->u.coalesce.main),
                      valDepthStructural(c, e->u.coalesce.fallback));
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, valDepthStructural(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value));
        return d;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
        return d;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        return d;
    case EX_INDEX: case EX_FIELD: return placeDepth(c, e);
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.call.args, i)));
        return d;
    case EX_METHOD:
        d = valDepthStructural(c, e->u.method.recv);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.method.args, i)));
        return d;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        return d;
    case EX_NEW: case EX_GENCALL:
        return e->arenaLevel == ARENA_HOME ? 0 : (e->arenaLevel > 0 ? e->arenaLevel : 0);
    default: return 0;
    }
}

/* ⭐ A2：记进任何地方的深度一律走这个 —— `exprRefDepth` 与结构上界**取 max** ✓ */
int valDepthForStore(Checker *c, Expr *e) {
    int a = exprRefDepth(c, e), b = valDepthStructural(c, e);
    return a > b ? a : b;
}

int exprRefDepth(Checker *c, Expr *e) {""")])

    # ── A2-5 读整值时再取一次各字段上界 ─────────────────────────────────
    edit(es, [("""        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = sy->refDepth;
        else d = placeDepth(c, e);
        break;""",
"""        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = sy->refDepth;
        else d = placeDepth(c, e);
        /* ⭐ A2：**别只看 `typeContainsRef`** —— 它对"字段类型本身是 `ref`"的聚合答 false
         * ⇒ `struct holder { p: ?ref i32 }` 的绑定会被判成 0，连字段表都不看 ✗
         * ⇒ 无论如何再取一次"各字段的上界" ✓ */
        if (sy) {
            for (int fi = 0; fi < sy->nfields; fi++)
                if (sy->fields[fi].depth > d) d = sy->fields[fi].depth;
            if (sy->otherDepth > d) d = sy->otherDepth;
        }
        break;""")])

    # ── 声明两条新函数 ────────────────────────────────────────────────
    edit(ih, [("int exprRefDepth (Checker *, Expr *);",
               "int exprRefDepth (Checker *, Expr *);\nint valDepthForStore (Checker *, Expr *);   /* ⭐ A2 */")])

    # ── A1-3 / A1-4 / A2-3 ────────────────────────────────────────────
    edit(st, [
        ("""                            slot0->refDepth = exprRefDepth(c, v);""",
         """                            {   /* ⭐ A1：取 max（原来覆盖 ⇒ 反例 C2 ✗）*/
                                int nd = valDepthForStore(c, v);
                                if (nd > slot0->refDepth) slot0->refDepth = nd;
                            }"""),
        ("""                    if (s->u.assign.target->kind == EX_IDENT) vs->refDepth = d2;
                    else if (d2 > vs->refDepth) vs->refDepth = d2;""",
         """                    /* ⭐ A1：**一律取 max**（原来 EX_IDENT 那一支是覆盖 ⇒ 反例 C3 ✗）*/
                    if (d2 > vs->refDepth) vs->refDepth = d2;"""),
        ("""                int d = exprRefDepth(c, s->u.var.init);""",
         """                int d = valDepthForStore(c, s->u.var.init);"""),
        ("""                    noteFieldDepthWrite(c, sym, fip->name, exprRefDepth(c, fip->value));""",
         """                    /* ⭐ A2：字段初值也要走 valDepthForStore（藏 `ref 局部` 的字面量）✓ */
                    noteFieldDepthWrite(c, sym, fip->name, valDepthForStore(c, fip->value));"""),
        ("""                            int dA = exprRefDepth(c, v);""",
         """                            int dA = valDepthForStore(c, v);"""),
        ("""                        if (rootA && rootA->type && typeContainsRef(c->tt, rootA->type)) {""",
         """                        /* ⚠️ 条件不能只看根的**类型**：`struct inner { v: mut ref i32 }` 里
                         * `typeContainsRef(inner)` 答 false ⇒ 整条记录被跳过 ✗
                         * ⇒ 再加一条：**值里真的带着引用**也要记 ✓ */
                        if (rootA && (dA > 0 ||
                                      (rootA->type && typeContainsRef(c->tt, rootA->type)))) {"""),
        # ── A2-4 普通赋值那一支补齐字段表（块从 blocks/ 读，保证与仓库原文逐字一致 ✓）
        (BLK('a24_old'), BLK('a24_new')),
    ])

    # ── A2-6 合流 join ────────────────────────────────────────────────
    snap = '''
/* ⭐ A2（ARENA-SOUNDNESS §9 档 0 的 join 那一半）：**控制流合流取 max**。
 *
 * `refDepth`/字段表记在**绑定**上，而绑定跨分支只有一个 ⇒ 两个分支各写各的
 * ⇒ **后走的那一支赢**（等于没做 join）✗
 *   `if c==1 { h.q = x } else { h.q = null }` ⇒ else 支把 then 支的深度抹掉
 * ⇒ 快照 + 按 max 合并（= "每个程序点一组可能值"的 join 在这里的落地）✓
 * 取 max **只会更保守**（记大 = 多拒 = 不放过）⇒ 方向天然安全 ✓
 */
typedef struct { const char *name; int refDepth; int otherDepth;
                 struct { const char *fname; int depth; } f[4]; int nf; } DepthSnap;

static void depthSnapshot(Checker *c, Vec *out) {
    vecInit(out, c->arena, sizeof(DepthSnap *));
    for (size_t i = 0; i < c->scopes.len; i++) {
        Scope *sc = *(Scope **)vecAt(&c->scopes, i);
        for (size_t j = 0; j < sc->syms.len; j++) {
            Sym *sy = *(Sym **)vecAt(&sc->syms, j);
            if (!sy->type || !typeContainsRef(c->tt, tsub(c, sy->type))) continue;
            DepthSnap *d = (DepthSnap *)arenaAllocZero(c->arena, sizeof(DepthSnap));
            d->name = sy->name; d->refDepth = sy->refDepth; d->otherDepth = sy->otherDepth;
            d->nf = sy->nfields;
            for (int k = 0; k < sy->nfields && k < 4; k++) {
                d->f[k].fname = sy->fields[k].name; d->f[k].depth = sy->fields[k].depth;
            }
            *(DepthSnap **)vecPush(out) = d;
        }
    }
}

static Sym *depthFindSym(Checker *c, const char *name) {
    for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *sc = *(Scope **)vecAt(&c->scopes, i);
        for (size_t j = sc->syms.len; j-- > 0; ) {
            Sym *sy = *(Sym **)vecAt(&sc->syms, j);
            if (sy->name && strcmp(sy->name, name) == 0) return sy;
        }
    }
    return NULL;
}

static void depthSnapMerge(Checker *c, Vec *snap) {
    for (size_t i = 0; i < snap->len; i++) {
        DepthSnap *d = *(DepthSnap **)vecAt(snap, i);
        Sym *sy = depthFindSym(c, d->name);
        if (!sy || !sy->type || !typeContainsRef(c->tt, tsub(c, sy->type))) continue;
        if (d->refDepth > sy->refDepth) sy->refDepth = d->refDepth;
        if (d->otherDepth > sy->otherDepth) sy->otherDepth = d->otherDepth;
        if (sy->addressed) continue;               /* 取过地址 ⇒ 根级 max 已覆盖 ✓ */
        for (int k = 0; k < d->nf && k < 4; k++) {
            int *slot = fieldDepthEntry(c, sy, d->f[k].fname, true);
            if (slot && d->f[k].depth > *slot) *slot = d->f[k].depth;
        }
        refreshRootDepth(sy);
    }
}

'''
    edit(st, [("void checkStmt(Checker *c, Stmt *s);\n", "void checkStmt(Checker *c, Stmt *s);\n" + snap)])
    edit(st, [
        ("""            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* 出了 then 分支，证明就不算数了 ✓ */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;""",
         """            Vec snapIn, snapThen, snapElse;
            depthSnapshot(c, &snapIn);         /* ⭐ A2：入口快照 */
            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* 出了 then 分支，证明就不算数了 ✓ */
            depthSnapshot(c, &snapThen);       /* ⭐ A2：then 出口快照 */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
                depthSnapshot(c, &snapElse);
                depthSnapMerge(c, &snapIn);    /* ⭐ A2：三条都并（分支可能都不执行）✓ */
                depthSnapMerge(c, &snapThen);
                depthSnapMerge(c, &snapElse);"""),
        ("""            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {""",
         """            } else {
                depthSnapMerge(c, &snapIn);    /* ⭐ A2：没有 else ⇒ 并入口与 then ✓ */
                depthSnapMerge(c, &snapThen);
            }
            if (0) { } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {"""),
        ("""            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            return;""",
         """            Vec snapIn, snapBody;
            depthSnapshot(c, &snapIn);         /* ⭐ A2 */
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            depthSnapshot(c, &snapBody);
            depthSnapMerge(c, &snapIn);        /* ⭐ A2：循环可能一次都不执行 ⇒ 入口也要并 ✓ */
            depthSnapMerge(c, &snapBody);
            return;"""),
    ])
    print("第 1 步（A1 + A2）已应用 ✓")

if __name__ == '__main__':
    main()
