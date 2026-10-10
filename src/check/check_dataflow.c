/* Forward, monotone data-flow analysis for reference depths.
 *
 * Rather than materialise a control-flow graph and iterate over blocks, this walks the
 * structured statement tree directly and computes, for every statement, the facts that
 * hold after it. That is the same fixed point:
 *
 *   sequence    feed the facts of one statement into the next
 *   if/else     analyze both arms from the same input facts and join with max
 *   while       iterate the body until the facts stop growing
 *   return      stop, the facts after it are irrelevant
 *   block       a nested scope; facts still flow out, because a binding declared
 *               outside the block can be assigned inside it
 *
 * Every operation only ever raises a depth, so the iteration is monotone and the
 * lattice has finite height. The answer therefore does not depend on the order in
 * which branches are visited.
 */
#include "dataflow.h"
#include "check_internal.h"
#include "plan.h"      /* planCName: the emitted name is a plan fact */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long g_dfRounds, g_dfMax, g_dfCalls;
/**
 * @brief 按需把数据流分析的调用次数与不动点轮数打到 stderr。
 *
 * 只有设置 `EXTC_DBG_TIME` 时才输出。
 */
static void dfReport(void) {
    if (getenv("EXTC_DBG_TIME"))
        fprintf(stderr, "[df] analyze=%ld rounds=%ld maxRounds=%ld\n", g_dfCalls, g_dfRounds, g_dfMax);
}
/**
 * @brief 保证 `dfReport` 只注册一次，供 `atexit` 在进程退出时调用。
 */
static void dfReg(void) { static int done; if (!done) { done = 1; atexit(dfReport); } }

#define DFA_MAX_VARS 256     /* bindings tracked per function */
#define DFA_MAX_FIELDS 4     /* fields tracked per binding, as in the field table */
#define DFA_ROUNDS 64        /* fixed-point iteration limit for loops */

/* One binding's facts. A binding not present in the table has depth 0. */
typedef struct {
    const char *cname;
    int         depth;
    int         nfields;
    struct { const char *name; int depth; } fields[DFA_MAX_FIELDS];
} VarFact;

typedef struct {
    VarFact vars[DFA_MAX_VARS];
    int     n;
    bool    overflow;
} Facts;

static Sym *dfRootOf(Checker *c, Expr *e);

/**
 * @brief 判断两个 C 名是否指同一个绑定。
 *
 * 并非每个绑定都有 C 名：从别的模块导入的模块级常量绑到尚未改名的符号，那里 `cname`
 * 为 NULL。任一方为 NULL 时比较指针就返回，因此 `strcmp` 不会收到 NULL；没有 C 名的绑定
 * 不是本分析追踪的局部量，不匹配任何名字。
 *
 * @param a  一个绑定的 C 名，可为 NULL。
 * @param b  另一个绑定的 C 名，可为 NULL。
 * @return   同名返回 true；两者都为 NULL 也算同名；恰有一个为 NULL 返回 false。
 */
static bool cnameEq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return a == b || strcmp(a, b) == 0;
}

/**
 * @brief 取一个绑定的事实行，没有就新建一行。
 *
 * @param f      事实表。
 * @param cname  绑定的 C 名；为 NULL 时返回 NULL（没有 C 名的绑定不追踪）。
 * @return       该绑定的行；表已满时返回 NULL 并置 `f->overflow`。
 */
static VarFact *factOf(Facts *f, const char *cname) {
    if (!cname) return NULL;
    for (int i = 0; i < f->n; i++)
        if (cnameEq(f->vars[i].cname, cname))
            return &f->vars[i];
    if (f->n >= DFA_MAX_VARS) { f->overflow = true; return NULL; }
    VarFact *v = &f->vars[f->n++];
    memset(v, 0, sizeof *v);
    v->cname = cname;
    return v;
}

/**
 * @brief 抬高一个绑定的深度，只升不降。
 *
 * 深度是上界，只许变大；`depth <= 0` 不留下记录。
 *
 * @param f      事实表。
 * @param cname  绑定的 C 名。
 * @param depth  要抬到的深度。
 */
static void raiseTo(Facts *f, const char *cname, int depth) {
    if (depth <= 0) return;
    VarFact *v = factOf(f, cname);
    if (v && depth > v->depth) v->depth = depth;
}

/**
 * @brief 抬高一个绑定某个字段的深度，只升不降。
 *
 * 字段表满了就把这个深度折进绑定自身的深度，并置 `overflow`：字段的深度算到绑定上只会让
 * 上界更大，也就是更安全的方向。
 *
 * @param f      事实表。
 * @param cname  绑定根的名字；为 NULL 时忽略。
 * @param field  字段名；为 NULL 时忽略。
 * @param depth  要抬到的深度。
 */
static void raiseField(Facts *f, const char *cname, const char *field, int depth) {
    if (!cname || !field) return;
    VarFact *v = factOf(f, cname);
    if (!v) return;
    for (int i = 0; i < v->nfields; i++)
        if (strcmp(v->fields[i].name, field) == 0) {
            if (depth > v->fields[i].depth) v->fields[i].depth = depth;
            return;
        }
    if (v->nfields < DFA_MAX_FIELDS) {
        v->fields[v->nfields].name  = field;      /* interned in the AST, so it lives on */
        v->fields[v->nfields].depth = depth;
        v->nfields++;
        if (depth > v->depth) v->depth = depth;
    } else {
        /* No room for another field: fold it into the binding's own depth, which can
         * only make the bound larger and therefore the answer safer. */
        if (depth > v->depth) v->depth = depth;
        f->overflow = true;
    }
}

/**
 * @brief 把一个事实表并进另一个，取两者的最大值，字段逐个并。
 *
 * @param dst  目标事实表，被就地抬高。
 * @param src  源事实表。
 */
static void joinInto(Facts *dst, const Facts *src) {
    for (int i = 0; i < src->n; i++) {
        const VarFact *s = &src->vars[i];
        raiseTo(dst, s->cname, s->depth);
        for (int k = 0; k < s->nfields; k++)
            raiseField(dst, s->cname, s->fields[k].name, s->fields[k].depth);
    }
    if (src->overflow) dst->overflow = true;
}

/**
 * @brief 判断两张事实表是否相同，用来检出不动的点。
 *
 * 只逐位置比较绑定个数与各深度（含字段深度），不比名字：两张表来自同一次遍历、按同样次序
 * 增长，所以位置可比。
 *
 * @param a  一张事实表。
 * @param b  另一张事实表。
 * @return   个数与所有深度都相同则返回 true。
 */
static bool sameFacts(const Facts *a, const Facts *b) {
    if (a->n != b->n) return false;
    for (int i = 0; i < a->n; i++) {
        const VarFact *x = &a->vars[i], *y = &b->vars[i];
        if (x->depth != y->depth || x->nfields != y->nfields) return false;
        for (int k = 0; k < x->nfields; k++)
            if (x->fields[k].depth != y->fields[k].depth) return false;
    }
    return true;
}

/**
 * @brief 整表复制事实。
 *
 * @param to    目标事实表。
 * @param from  源事实表。
 */
static void copyFacts(Facts *to, const Facts *from) { *to = *from; }

/**
 * @brief 取一个变量在不动点下的深度。
 *
 * 变量自身的深度是其各字段深度的最大值，所以对聚合体与单个引用都成立；分析里没有这个绑定
 * 的事实时返回 0。
 *
 * @param f      事实表；为 NULL 时返回 0。
 * @param cname  绑定的 C 名；为 NULL 时返回 0。
 * @return       该绑定的深度；没有记录时返回 0。
 */
static int factDepthOf(const Facts *f, const char *cname) {
    if (!f || !cname) return 0;
    for (int i = 0; i < f->n; i++)
        if (cnameEq(f->vars[i].cname, cname))
            return f->vars[i].depth;
    return 0;
}

/**
 * @brief 算一个值里所携带引用的深度上界，读表达式树与现有事实。
 *
 * 它照着转移规则回答，而不是照 checker 的 `targetDepth`：分析期间 arena 层级尚未定稿，
 * 所以这里从不读它；绑定深度也取自本分析算出的事实，而不是遍历时顺手写在某个字段上的数。
 *
 * @param c     checker，用于名字解析与类型表。
 * @param f     当前事实。
 * @param e     表达式；为 NULL 时返回 0。
 * @param hops  递归深度；超过 32 时直接返回 0。
 * @return      该表达式可携带的引用深度上界；不含引用时返回 0。
 */
static int dfExprDepth(Checker *c, const Facts *f, Expr *e, int hops) {
    if (!e || hops > 32) return 0;
    switch (e->kind) {
/* `dyn Trait(x)`: the payload is a child expression (`dynv.payload`), so every
     * walker has to look inside it -- the walkers end in `default:`, which is why `-Wswitch`
     * never points at a missing case. */
    case EX_DYN: return dfExprDepth(c, f, e->u.dynv.payload, hops + 1);
    case EX_IDENT: {
        Sym *sy = identBindOf(e);
        if (!sy) return 0;
        if (!sy->cname) return 0;
        VarFact *v = NULL;
        for (int i = 0; i < f->n; i++)
            if (cnameEq(f->vars[i].cname, sy->cname)) {
                v = (VarFact *)&f->vars[i];
                break;
            }
        if (v) return v->depth;
        /* No fact: the binding is not one this analysis tracks, so fall back to what
         * the checker recorded, which is an upper bound. */
        return anRefDepth(sy) > 0 ? anRefDepth(sy) : 0;
    }
    case EX_NEW:
    case EX_GENCALL:
        /* Where the site ends up. `anMinAt` reports the level the promotion decided the
         * site must live to; the lexical level is only the block the expression was born
         * in, so it is a fallback for a site the promotion did not record. The data flow
         * may only raise a depth, so a too-small answer would survive the write-back. */
        if (anMinAt(e) >= 0) return anMinAt(e);
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_FIELD: {
        Sym *root = dfRootOf(c, e);
        if (root) {
            for (int i = 0; i < f->n; i++)
                if (cnameEq(f->vars[i].cname, root->cname)) {
                    for (int k = 0; k < f->vars[i].nfields; k++)
                        if (strcmp(f->vars[i].fields[k].name, e->u.field.name) == 0)
                            return f->vars[i].fields[k].depth;
                    break;
                }
        }
        /* The field is not in the table (never written, or written under a name this
         * analysis did not see). The binding's own depth is the maximum over its fields,
         * so it is the conservative answer; it is also the fixed point's number, whereas
         * the depth the checker recorded while the body was still being walked is
         * pre-fixed-point and can read 0 for a binding whose fields are in doubt. A
         * destination read as 0 makes the level pass believe the store needs no lifetime
         * at all. */
        if (root) {
            int vd = factDepthOf(f, root->cname);
            if (vd > 0) return vd;
        }
        return root && anRefDepth(root) > 0 ? anRefDepth(root) : 0;
    }
    case EX_INDEX:  return dfExprDepth(c, f, e->u.index.obj, hops + 1);
    case EX_DEREF:  return dfExprDepth(c, f, e->u.deref.operand, hops + 1);
    case EX_SIGN:   return dfExprDepth(c, f, e->u.sign.operand, hops + 1);
    /* `?` and a conversion pass the operand's references through; an operator can only carry
     * what its operands carry. This is the oracle the `EXTC_DBG_RHO` diagnostic compares
     * against, and `dfExprDepth` feeds the flow facts, so both have to answer for these. */
    case EX_SLICE:  return dfExprDepth(c, f, e->u.slice.obj, hops + 1);
    case EX_TRY:    return dfExprDepth(c, f, e->u.try_.operand, hops + 1);
    case EX_CONV:   return dfExprDepth(c, f, e->u.conv.operand, hops + 1);
    case EX_UN:     return dfExprDepth(c, f, e->u.un.operand, hops + 1);
    case EX_EXT: return dfExprDepth(c, f, e->u.ext_.call, hops + 1);   /* `ext f(x)`: spawned call */
    case EX_BIN: {
        int a = dfExprDepth(c, f, e->u.bin.left, hops + 1);
        int b = dfExprDepth(c, f, e->u.bin.right, hops + 1);
        return a > b ? a : b;
    }
    case EX_REF:    return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_COALESCE: {
        int a = dfExprDepth(c, f, e->u.coalesce.main, hops + 1);
        int b = dfExprDepth(c, f, e->u.coalesce.fallback, hops + 1);
        return a > b ? a : b;
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            int x = dfExprDepth(c, f, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA: {
        int d = 0;
        for (size_t i = 0; i < e->u.lambda.inits.len; i++) {
            int x = dfExprDepth(c, f, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value,
                                hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
            int x = dfExprDepth(c, f, *(Expr **)vecAt(&e->u.arraylit.elems, i), hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++) {
            int x = dfExprDepth(c, f, *(Expr **)vecAt(&e->u.enumval.args, i), hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_CALL:
    case EX_ASSOC: {
        Vec *args = e->kind == EX_CALL ? &e->u.call.args : &e->u.assoc.args;
        int d = 0;
        for (size_t i = 0; i < args->len; i++) {
            int x = dfExprDepth(c, f, *(Expr **)vecAt(args, i), hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_METHOD: {
        int d = dfExprDepth(c, f, e->u.method.recv, hops + 1);
        for (size_t i = 0; i < e->u.method.args.len; i++) {
            int x = dfExprDepth(c, f, *(Expr **)vecAt(&e->u.method.args, i), hops + 1);
            if (x > d) d = x;
        }
        return d;
    }
    default:
        return 0;
    }
}

/**
 * @brief 判断一个表达式是否可能持有指向存储的东西。
 *
 * 参数可能携带引用，所以 `mentionsParam` 的答案先算数；其余按类型表的引用包含关系回答。
 *
 * @param c  checker，用于类型表。
 * @param e  表达式；为 NULL 时返回 false。
 * @return   可能携带引用则返回 true。
 */
static bool dfCarriesRef(Checker *c, Expr *e) {
    if (!e) return false;
    if (mentionsParam(e->type)) return true;          /* a parameter may carry one */
    return typeContainsRef(c->tt, tsub(c, e->type));
}

static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth);

/**
 * @brief 取一个位置表达式根上的绑定，优先用 checker 钉在节点上的那个。
 *
 * `placeRoot` 走作用域栈按名字解析，而本分析在函数体检查完之后才跑，那时按名字解析是错的。
 *
 * @param c  checker。
 * @param e  位置表达式（名字、字段、下标或切片）。
 * @return   根绑定；表达式不是位置或找不到绑定时返回 NULL。
 */
static Sym *dfRootOf(Checker *c, Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->kind == EX_IDENT) {
            Sym *sy = identBindOf(x);
            return sy ? sy : lookup(c, x->u.ident.name);
        }
        if (x->kind == EX_FIELD) { x = x->u.field.obj; continue; }
        if (x->kind == EX_INDEX) { x = x->u.index.obj; continue; }
        if (x->kind == EX_SLICE) { x = x->u.slice.obj; continue; }
        return NULL;
    }
    return NULL;
}

/**
 * @brief 按顺序遍历一个块里的语句，把块深度加一传下去。
 *
 * 不是块的语句（NULL 或另一种语句）直接交给 `dfStmt` 处理。
 *
 * @param c      checker。
 * @param block  要遍历的块；NULL 或非块时退化为对单条语句的处理。
 * @param f      就地更新的事实表。
 * @param depth  当前块深度；块内语句按 `depth + 1` 遍历。
 */
static void dfBlock(Checker *c, Stmt *block, Facts *f, int depth) {
    if (!block || block->kind != ST_BLOCK) { dfStmt(c, block, f, depth); return; }
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        dfStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i), f, depth + 1);
}

/**
 * @brief 把一条语句的转移规则作用到事实表上，就地抬高各绑定的深度。
 *
 * 走的是结构化的语句树而不是控制流图：顺序语句首尾相接，分支各自从同一起点算再取最大值
 * 汇合，循环迭代到事实不再增长。
 *
 * @param c      checker。
 * @param s      语句；为 NULL 时不做任何事。
 * @param f      就地更新的事实表。
 * @param depth  当前块深度；进入嵌套块时加一。
 */
static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth) {
    if (!s) return;
    /*@@all-kinds*/  /* tagged for the walker gate: `dfStmt` is mutually recursive with
                      * `dfBlock`, so the gate's "mentions its own name" test does not
                      * recognise it as a walker. */
    switch (s->kind) {
    case ST_VAR: {
        if (!s->u.var.init) break;
        /* The initializer may be a struct literal, in which case the depth belongs to
         * the individual fields as well as to the binding. */
        if (s->u.var.init->kind == EX_STRUCTLIT) {
            for (size_t i = 0; i < s->u.var.init->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, i);
                raiseField(f, planCName(s), fi->name, dfExprDepth(c, f, fi->value, 0));
            }
        }
        if (dfCarriesRef(c, s->u.var.init))
            raiseTo(f, planCName(s), dfExprDepth(c, f, s->u.var.init, 0));
        break;
    }
    case ST_ASSIGN: {
        /* Resolve through the binding pinned on the node rather than by name: the
         * analysis runs after the function body is checked, and a name lookup at that
         * point competes with every other scope in the module. */
        Sym *root = dfRootOf(c, s->u.assign.target);
        if (!root) break;
        int d = dfCarriesRef(c, s->u.assign.value)
                ? dfExprDepth(c, f, s->u.assign.value, 0) : 0;
        if (s->u.assign.target->kind == EX_FIELD) {
            const char *fld = s->u.assign.target->u.field.name;
            if (d > 0) raiseField(f, root->cname, fld, d);
            else {
                /* stored something with nothing live in it: the field cannot be deeper
                 * than the destination itself, and the destination is in this frame. */
                VarFact *v = factOf(f, root->cname);
                if (v) for (int i = 0; i < v->nfields; i++)
                    if (strcmp(v->fields[i].name, fld) == 0) v->fields[i].depth = 0;
            }
        } else {
            /* Whole-value assignment. A struct is copied by value, so the destination
             * acquires every field of the source as well as its overall depth; raising
             * only the depth would leave a field fact at 0 while the field itself can
             * hold something live. */
            raiseTo(f, root->cname, d);
            Sym *src = dfRootOf(c, s->u.assign.value);
            if (src) {
                VarFact *sv = factOf(f, src->cname);
                if (sv)
                    for (int i = 0; i < sv->nfields; i++)
                        raiseField(f, root->cname, sv->fields[i].name, sv->fields[i].depth);
            }
        }
        break;
    }
    case ST_IF: {
        Facts thenF, elseF;
        copyFacts(&thenF, f);
        copyFacts(&elseF, f);
        dfBlock(c, s->u.ifs.thenBody, &thenF, depth);
        if (s->u.ifs.elseBody) dfBlock(c, s->u.ifs.elseBody, &elseF, depth);
        /* The join: both arms start from the same input facts, so the result is their
         * maximum, not whichever arm was walked last. */
        joinInto(f, &thenF);
        joinInto(f, &elseF);
        break;
    }
    case ST_WHILE: {
        /* Iterate the body until the facts stop growing. Monotone transfer functions
         * over a lattice of finite height guarantee termination; the round limit is a
         * belt-and-braces bound, and reaching it marks the result as incomplete. */
        Facts saved, body;
        int rounds = 0;
        for (;;) {
            copyFacts(&saved, f);
            copyFacts(&body, f);
            dfBlock(c, s->u.whiles.body, &body, depth);
            joinInto(f, &body);
            rounds++;
            g_dfRounds++;
            if ((long)rounds > g_dfMax) g_dfMax = (long)rounds;
            if (sameFacts(&saved, f)) break;
            if (rounds >= DFA_ROUNDS) { f->overflow = true; break; }
        }
        break;
    }
    case ST_BLOCK:
        dfBlock(c, s, f, depth);
        break;
    case ST_MATCH: {
        Facts acc, arm;
        copyFacts(&acc, f);
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            /* A payload binding holds part of the scrutinee, so give it the depth the
             * scrutinee is known to have. */
            int sd = dfExprDepth(c, f, s->u.match.scrutinee, 0);
            for (size_t k = 0; k < a->binds.len; k++) {
                Sym *bs = *(Sym **)vecAt(&a->binds, k);
                if (bs) raiseTo(f, bs->cname, sd);
            }
            copyFacts(&arm, f);
            dfBlock(c, a->body, &arm, depth);
            joinInto(&acc, &arm);
        }
        joinInto(f, &acc);
        break;
    }
    case ST_DOMAIN:
        /* A store inside `d.run { ... }` is a store in this body, so the walker descends
         * into the domain block like any other block. */
        dfBlock(c, s->u.domain_.body, f, depth);
        break;
    case ST_RETURN:
    case ST_BREAK:
    case ST_CONTINUE:
    case ST_EXPR:
    default:
        break;
    }
}

/**
 * @brief 分析一个函数体并填 `out`。
 *
 * 在函数体检查完之后跑，此时表达式类型与解析好的名字都在树上；只读 AST，从不写它。
 *
 * @param c    checker，用于名字解析与类型表；为 NULL 时 `out` 全零。
 * @param f    要分析其函数体的函数；为 NULL 或没有函数体时 `out` 全零。
 * @param out  填入最终事实；它们不完整时置 `overflow`。
 */
void dfAnalyze(Checker *c, FuncDef *f, DfResult *out) {
    memset(out, 0, sizeof *out);
    if (!c || !f || !f->body) return;

    Facts facts;
    memset(&facts, 0, sizeof facts);

    /* The walk below is the whole analysis. Rows appear as it reaches each binding:
     * `dfStmt` calls `raiseTo`/`raiseField`, which insert one through `factOf` --
     * `raiseTo` only for a depth > 0, `raiseField` for any depth. An absent row reads
     * as depth 0, so readers must tolerate it. */
    dfReg(); g_dfCalls++;
    dfBlock(c, f->body, &facts, 1);

    out->overflow = facts.overflow;
    out->nvars    = facts.n;
    for (int i = 0; i < facts.n && i < DFA_MAX_VARS; i++) {
        out->vars[i].cname  = facts.vars[i].cname;
        out->vars[i].depth  = facts.vars[i].depth;
        out->vars[i].nfields = facts.vars[i].nfields;
        for (int k = 0; k < facts.vars[i].nfields; k++) {
            out->vars[i].fields[k].name  = facts.vars[i].fields[k].name;
            out->vars[i].fields[k].depth = facts.vars[i].fields[k].depth;
        }
    }
}

/**
 * @brief 把结果表还原成转移函数用的 `Facts`，以便在不动点上复用 `dfExprDepth`。
 *
 * @param r  分析结果。
 * @param f  填好的事实表。
 */
static void factsFromResult(const DfResult *r, Facts *f) {
    memset(f, 0, sizeof *f);
    f->overflow = r->overflow;
    f->n = r->nvars;
    for (int i = 0; i < r->nvars && i < DFA_MAX_VARS; i++) {
        f->vars[i].cname   = r->vars[i].cname;
        f->vars[i].depth   = r->vars[i].depth;
        f->vars[i].nfields = r->vars[i].nfields;
        for (int k = 0; k < r->vars[i].nfields; k++) {
            f->vars[i].fields[k].name  = r->vars[i].fields[k].name;
            f->vars[i].fields[k].depth = r->vars[i].fields[k].depth;
        }
    }
}

/**
 * @brief 在 `r` 的不动点下算表达式 `e` 的深度。
 *
 * @param c  checker，用于名字解析。
 * @param r  分析结果；为 NULL 或 `overflow` 时返回 0。
 * @param e  要度量的表达式。
 * @return   `e` 能携带的引用深度的上界；没有可用的分析结果时返回 0。
 */
int dfValueDepth(Checker *c, const DfResult *r, Expr *e) {
    if (!c || !r || r->overflow || !e) return 0;
    Facts f;
    factsFromResult(r, &f);
    return dfExprDepth(c, &f, e, 0);
}

/**
 * @brief 取一个绑定记录下来的深度。
 *
 * @param r      分析结果；为 NULL 或 `overflow` 时返回 -1。
 * @param cname  绑定的 C 名；为 NULL 时返回 -1。
 * @return       深度；分析结果里没有这一条时返回 -1。
 */
int dfLookup(const DfResult *r, const char *cname) {
    if (!r || !cname || r->overflow) return -1;
    for (int i = 0; i < r->nvars; i++)
        if (cnameEq(r->vars[i].cname, cname))
            return r->vars[i].depth;
    return -1;
}
