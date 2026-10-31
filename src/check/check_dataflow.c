/**
 * @file check_dataflow.c
 * @brief Forward, monotone data-flow analysis for reference depths.
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
 * @brief Write the analysis call count and fixed-point round count to stderr.
 *
 * Only active when `EXTC_DBG_TIME` is set in the environment.
 */
static void dfReport(void) {
    if (getenv("EXTC_DBG_TIME"))
        fprintf(stderr, "[df] analyze=%ld rounds=%ld maxRounds=%ld\n", g_dfCalls, g_dfRounds, g_dfMax);
}
/**
 * @brief Register `dfReport` with `atexit`, exactly once.
 */
static void dfReg(void) { static int done; if (!done) { done = 1; atexit(dfReport); } }

#define DFA_MAX_VARS 256     /* bindings tracked per function */
#define DFA_MAX_FIELDS 4     /* fields tracked per binding, as in the field table */
#define DFA_ROUNDS 64        /* fixed-point iteration limit for loops */
/* The walk below is recursive, and every `if` / `while` / `match` frame needs a fact table.
 * Block nesting is already capped in the parser, but an `else if` chain adds one frame per
 * arm without adding a block, so this is the bound that keeps a pathological source file
 * from exhausting the compiler's own stack. */
#define DFA_MAX_NEST 256     /* deepest statement nesting the analysis walks */

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
 * @brief Tell whether two C names denote the same binding.
 *
 * Not every binding has a C name: a module-level constant imported from another module
 * is bound to a symbol that has not been renamed, so its `cname` is NULL. Comparing the
 * pointers when either side is NULL means `strcmp` never receives NULL; a binding with
 * no C name is not a local this analysis tracks and matches no name.
 *
 * @param a  C name of one binding; may be NULL.
 * @param b  C name of the other binding; may be NULL.
 * @return   True when the names are equal; two NULLs count as equal; false when exactly
 *           one is NULL.
 */
static bool cnameEq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return a == b || strcmp(a, b) == 0;
}

/**
 * @brief Return the fact row of a binding, creating it when absent.
 *
 * @param f      Fact table.
 * @param cname  C name of the binding; NULL returns NULL, because a binding without a
 *               C name is not tracked.
 * @return       The binding's row; NULL when the table is full, in which case
 *               `f->overflow` is set.
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
 * @brief Raise a binding's depth; depths only grow.
 *
 * The depth is an upper bound, so it may only increase; `depth <= 0` records nothing.
 *
 * @param f      Fact table.
 * @param cname  C name of the binding.
 * @param depth  Depth to raise to.
 */
static void raiseTo(Facts *f, const char *cname, int depth) {
    if (depth <= 0) return;
    VarFact *v = factOf(f, cname);
    if (v && depth > v->depth) v->depth = depth;
}

/**
 * @brief Raise the depth of one field of a binding; depths only grow.
 *
 * When the field table is full, the depth is folded into the binding's own depth and
 * `overflow` is set: attributing a field's depth to the binding only enlarges the upper
 * bound, which is the safe direction.
 *
 * @param f      Fact table.
 * @param cname  C name of the binding root; NULL is ignored.
 * @param field  Field name; NULL is ignored.
 * @param depth  Depth to raise to.
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
 * @brief Join one fact table into another, taking the maximum of both, field by field.
 *
 * @param dst  Destination fact table, raised in place.
 * @param src  Source fact table.
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
 * @brief Tell whether two fact tables are equal, for detecting the fixed point.
 *
 * Compares only the binding count and the depths by position (field depths included),
 * not the names: both tables come from the same walk and grow in the same order, so
 * positions are comparable.
 *
 * @param a  One fact table.
 * @param b  The other fact table.
 * @return   True when the count and every depth are equal.
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
 * @brief Copy a whole fact table.
 *
 * @param to    Destination fact table.
 * @param from  Source fact table.
 */
static void copyFacts(Facts *to, const Facts *from) { *to = *from; }

/**
 * @brief Look up a binding's depth at the fixed point.
 *
 * A binding's own depth is the maximum over its fields, so the answer holds for
 * aggregates and single references alike; 0 is returned when the analysis has no fact
 * for the binding.
 *
 * @param f      Fact table; NULL returns 0.
 * @param cname  C name of the binding; NULL returns 0.
 * @return       The binding's depth; 0 when there is no fact.
 */
static int factDepthOf(const Facts *f, const char *cname) {
    if (!f || !cname) return 0;
    for (int i = 0; i < f->n; i++)
        if (cnameEq(f->vars[i].cname, cname))
            return f->vars[i].depth;
    return 0;
}

/**
 * @brief Compute an upper bound on the reference depth carried by a value, from the
 *        expression tree and the current facts.
 *
 * It answers from the transfer rules rather than from the checker's `targetDepth`: the
 * arena levels are not final while the analysis runs, so it never reads them, and
 * binding depths come from the facts this analysis computes, not from a number written
 * onto a field during the walk.
 *
 * @param c     Checker, for name resolution and the type table.
 * @param f     Current facts.
 * @param e     Expression; NULL returns 0.
 * @param hops  Recursion depth; anything above 32 returns 0.
 * @return      Upper bound on the reference depth the expression can carry; 0 when it
 *              carries none.
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
 * @brief Tell whether an expression may carry something that points to storage.
 *
 * A parameter may carry a reference, so `mentionsParam` answers first; everything else
 * is answered by the type table's reference containment.
 *
 * @param c  Checker, for the type table.
 * @param e  Expression; NULL returns false.
 * @return   True when it may carry a reference.
 */
static bool dfCarriesRef(Checker *c, Expr *e) {
    if (!e) return false;
    if (mentionsParam(e->type)) return true;          /* a parameter may carry one */
    return typeContainsRef(c->tt, tsub(c, e->type));
}

static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth);

/**
 * @brief Allocate a zeroed fact table in the checker's arena.
 *
 * A `Facts` holds 256 `VarFact` rows and is far too large for a recursive frame, so the
 * tables a branch or loop needs live in the arena and are released with it.
 *
 * @param c  Checker, for the arena.
 * @return   A zeroed fact table; never NULL.
 */
static Facts *dfFactsNew(Checker *c) {
    return (Facts *)arenaAllocZero(c->arena, sizeof(Facts));
}

/**
 * @brief Return the binding at the root of a place expression, preferring the one the
 *        checker pinned on the node.
 *
 * `placeRoot` resolves by name through the scope stack, and this analysis runs after the
 * function body has been checked, when resolving by name is wrong.
 *
 * @param c  Checker.
 * @param e  Place expression: a name, field, index, or slice.
 * @return   The root binding; NULL when the expression is not a place or no binding is
 *           found.
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
 * @brief Walk the statements of a block in order, one nesting level deeper.
 *
 * A non-block statement (NULL or another kind) is handed to `dfStmt` directly, also one
 * level deeper: `else if` chains produce exactly that shape, and the nesting budget has to
 * see them.
 *
 * @param c      Checker.
 * @param block  Block to walk; NULL or a non-block degrades to a single statement.
 * @param f      Fact table, updated in place.
 * @param depth  Current nesting level; statements inside are walked at `depth + 1`.
 */
static void dfBlock(Checker *c, Stmt *block, Facts *f, int depth) {
    if (!block || block->kind != ST_BLOCK) { dfStmt(c, block, f, depth + 1); return; }
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        dfStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i), f, depth + 1);
}

/**
 * @brief Apply one statement's transfer rule to the fact table, raising each binding's
 *        depth in place.
 *
 * It follows the structured statement tree rather than a control-flow graph: sequential
 * statements chain end to end, branches are analyzed from the same input facts and joined
 * by taking the maximum, and loops iterate until the facts stop growing.
 *
 * @param c      Checker.
 * @param s      Statement; NULL does nothing.
 * @param f      Fact table, updated in place.
 * @param depth  Current nesting level; one more than the enclosing statement.
 */
static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth) {
    if (!s) return;
    /* The walk is recursive and every branch frame owns a fact table, so a source file
     * nested beyond `DFA_MAX_NEST` is refused here instead of letting the C stack go.
     * Block nesting is capped in the parser, but an `else if` chain adds a frame per arm
     * while adding no block. */
    if (depth > DFA_MAX_NEST) {
        if (!f->overflow) {
            f->overflow = true;
            ctxError(c->ctx, s->line, 1,
                     "Deeply nested syntax comes from a machine, and the compiler stops at a"
                     " fixed depth rather than letting its own recursion overflow the C stack.",
                     "nesting too deep in a block (the compiler's limit is %d levels)",
                     DFA_MAX_NEST);
        }
        return;
    }
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
        Facts *thenF = dfFactsNew(c);
        Facts *elseF = dfFactsNew(c);
        copyFacts(thenF, f);
        copyFacts(elseF, f);
        dfBlock(c, s->u.ifs.thenBody, thenF, depth);
        if (s->u.ifs.elseBody) dfBlock(c, s->u.ifs.elseBody, elseF, depth);
        /* The join: both arms start from the same input facts, so the result is their
         * maximum, not whichever arm was walked last. */
        joinInto(f, thenF);
        joinInto(f, elseF);
        break;
    }
    case ST_WHILE: {
        /* Iterate the body until the facts stop growing. Monotone transfer functions
         * over a lattice of finite height guarantee termination; the round limit is a
         * belt-and-braces bound, and reaching it marks the result as incomplete. */
        Facts *saved = dfFactsNew(c);
        Facts *body = dfFactsNew(c);
        int rounds = 0;
        for (;;) {
            copyFacts(saved, f);
            copyFacts(body, f);
            dfBlock(c, s->u.whiles.body, body, depth);
            joinInto(f, body);
            rounds++;
            g_dfRounds++;
            if ((long)rounds > g_dfMax) g_dfMax = (long)rounds;
            if (sameFacts(saved, f)) break;
            if (rounds >= DFA_ROUNDS) { f->overflow = true; break; }
        }
        break;
    }
    case ST_BLOCK:
        dfBlock(c, s, f, depth);
        break;
    case ST_MATCH: {
        Facts *acc = dfFactsNew(c);
        Facts *arm = dfFactsNew(c);
        copyFacts(acc, f);
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            /* A payload binding holds part of the scrutinee, so give it the depth the
             * scrutinee is known to have. */
            int sd = dfExprDepth(c, f, s->u.match.scrutinee, 0);
            for (size_t k = 0; k < a->binds.len; k++) {
                /* The element type is `const char *`: the binding name itself (see
                 * `MatchArm.binds`). Reading it as `Sym *` would treat that name pointer as
                 * a struct, so `bs->cname` would become an address formed from the first
                 * bytes of the string: a garbage name lands in the fact table, and later
                 * `cnameEq` / `symBucket` `strcmp` calls walk into unmapped memory. */
                const char *bn = *(const char **)vecAt(&a->binds, k);
                if (bn) raiseTo(f, bn, sd);
            }
            copyFacts(arm, f);
            dfBlock(c, a->body, arm, depth);
            joinInto(acc, arm);
        }
        joinInto(f, acc);
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
 * @brief Analyze one function body and fill `out`.
 *
 * Runs after the function body has been checked, when expression types and resolved names
 * are on the tree; it only reads the AST and never writes it.
 *
 * @param c    Checker, for name resolution and the type table; NULL leaves `out` zeroed.
 * @param f    Function whose body is analyzed; NULL or a body-less function leaves `out`
 *             zeroed.
 * @param out  Receives the final facts; `overflow` is set when they are incomplete.
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
 * @brief Rebuild the `Facts` a transfer function uses from a result table, so
 *        `dfExprDepth` can be reused at the fixed point.
 *
 * @param r  Analysis result.
 * @param f  Fact table to fill.
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
 * @brief Compute the depth of an expression under the fixed point in `r`.
 *
 * @param c  Checker, for name resolution.
 * @param r  Analysis result; NULL or `overflow` returns 0.
 * @param e  Expression to measure.
 * @return   Upper bound on the reference depth `e` can carry; 0 when no usable result
 *           exists.
 */
int dfValueDepth(Checker *c, const DfResult *r, Expr *e) {
    if (!c || !r || r->overflow || !e) return 0;
    Facts f;
    factsFromResult(r, &f);
    return dfExprDepth(c, &f, e, 0);
}

/**
 * @brief Return the depth recorded for a binding.
 *
 * @param r      Analysis result; NULL or `overflow` returns -1.
 * @param cname  C name of the binding; NULL returns -1.
 * @return       The depth; -1 when the result has no entry for the binding.
 */
int dfLookup(const DfResult *r, const char *cname) {
    if (!r || !cname || r->overflow) return -1;
    for (int i = 0; i < r->nvars; i++)
        if (cnameEq(r->vars[i].cname, cname))
            return r->vars[i].depth;
    return -1;
}
