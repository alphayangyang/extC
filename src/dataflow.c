/* Forward, monotone data-flow analysis for reference depths. See dataflow.h for why.
 *
 * The shape of the analysis is the one specified in docs/topics/ARENA-SOUNDNESS.md
 * section 9.3 item 3-a. Rather than materialise a control-flow graph and iterate over
 * blocks, this walks the structured statement tree directly and computes, for every
 * statement, the facts that hold after it. That is the same fixed point:
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
 * which branches are visited, which is the property the old destructive update lacked.
 */
#include "dataflow.h"
#include "check_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static VarFact *factOf(Facts *f, const char *cname) {
    if (!cname) return NULL;
    for (int i = 0; i < f->n; i++)
        if (f->vars[i].cname == cname || strcmp(f->vars[i].cname, cname) == 0)
            return &f->vars[i];
    if (f->n >= DFA_MAX_VARS) { f->overflow = true; return NULL; }
    VarFact *v = &f->vars[f->n++];
    memset(v, 0, sizeof *v);
    v->cname = cname;
    return v;
}

/* Raise one binding's depth. Never lowers: an upper bound may only grow. */
static void raiseTo(Facts *f, const char *cname, int depth) {
    if (depth <= 0) return;
    VarFact *v = factOf(f, cname);
    if (v && depth > v->depth) v->depth = depth;
}

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

/* Join: the maximum of both inputs, field by field. */
static void joinInto(Facts *dst, const Facts *src) {
    for (int i = 0; i < src->n; i++) {
        const VarFact *s = &src->vars[i];
        raiseTo(dst, s->cname, s->depth);
        for (int k = 0; k < s->nfields; k++)
            raiseField(dst, s->cname, s->fields[k].name, s->fields[k].depth);
    }
    if (src->overflow) dst->overflow = true;
}

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

static void copyFacts(Facts *to, const Facts *from) { *to = *from; }

/* Depth of the references inside a value, read from the tree and the facts so far.
 *
 * This mirrors the transfer rule rather than the checker's `exprRefDepth`: it never
 * consults an arena level, because those are not final while the checker runs, and it
 * reads binding depths from the facts computed by this analysis instead of from a
 * field the walk happens to have written.
 */
static int dfExprDepth(Checker *c, const Facts *f, Expr *e, int hops) {
    if (!e || hops > 32) return 0;
    switch (e->kind) {
    case EX_IDENT: {
        Sym *sy = identBindOf(e);
        if (!sy) return 0;
        VarFact *v = NULL;
        for (int i = 0; i < f->n; i++)
            if (f->vars[i].cname == sy->cname || strcmp(f->vars[i].cname, sy->cname) == 0) {
                v = (VarFact *)&f->vars[i];
                break;
            }
        if (v) return v->depth;
        /* No fact: the binding is not one this analysis tracks, so fall back to what
         * the checker recorded, which is an upper bound. */
        return sy->refDepth > 0 ? sy->refDepth : 0;
    }
    case EX_NEW:
    case EX_GENCALL:
        /* The block the site was born in, which does not move. */
        return e->lexicalLevel > 0 ? e->lexicalLevel : 0;
    case EX_FIELD: {
        Sym *root = dfRootOf(c, e);
        if (root) {
            for (int i = 0; i < f->n; i++)
                if (f->vars[i].cname == root->cname || strcmp(f->vars[i].cname, root->cname) == 0) {
                    for (int k = 0; k < f->vars[i].nfields; k++)
                        if (strcmp(f->vars[i].fields[k].name, e->u.field.name) == 0)
                            return f->vars[i].fields[k].depth;
                    break;
                }
        }
        return root && root->refDepth > 0 ? root->refDepth : 0;
    }
    case EX_INDEX:  return dfExprDepth(c, f, e->u.index.obj, hops + 1);
    case EX_DEREF:  return dfExprDepth(c, f, e->u.deref.operand, hops + 1);
    case EX_SIGN:   return dfExprDepth(c, f, e->u.sign.operand, hops + 1);
    case EX_SLICE:  return dfExprDepth(c, f, e->u.slice.obj, hops + 1);
    case EX_REF:    return e->lexicalLevel > 0 ? e->lexicalLevel : 0;
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

/* Does this expression hold anything that could point at storage? */
static bool dfCarriesRef(Checker *c, Expr *e) {
    if (!e) return false;
    if (mentionsParam(e->type)) return true;          /* a parameter may carry one */
    return typeContainsRef(c->tt, tsub(c, e->type));
}

static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth);

/* The binding at the root of a place, preferring the one the checker pinned onto the
 * AST node. `placeRoot` resolves names through the scope stack, which is the wrong tool
 * after the body has been checked. */
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

static void dfBlock(Checker *c, Stmt *block, Facts *f, int depth) {
    if (!block || block->kind != ST_BLOCK) { dfStmt(c, block, f, depth); return; }
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        dfStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i), f, depth + 1);
}

static void dfStmt(Checker *c, Stmt *s, Facts *f, int depth) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR: {
        if (!s->u.var.init) break;
        /* The initializer may be a struct literal, in which case the depth belongs to
         * the individual fields as well as to the binding. */
        if (s->u.var.init->kind == EX_STRUCTLIT) {
            for (size_t i = 0; i < s->u.var.init->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, i);
                raiseField(f, s->u.var.cname, fi->name, dfExprDepth(c, f, fi->value, 0));
            }
        }
        if (dfCarriesRef(c, s->u.var.init))
            raiseTo(f, s->u.var.cname, dfExprDepth(c, f, s->u.var.init, 0));
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
             * acquires every field of the source as well as its overall depth. Copying
             * only the depth would leave `h.q` at 0 after `h = g`, and a later read of
             * `h.q` would then report that nothing live is in there. */
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
        /* The join. This is the step the old implementation did not have: without it the
         * later of the two arms simply overwrote the earlier one. */
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
    case ST_RETURN:
    case ST_BREAK:
    case ST_CONTINUE:
    case ST_EXPR:
    default:
        break;
    }
}

/* Run the analysis and turn the fixed point into a result table. */
void dfAnalyze(Checker *c, FuncDef *f, DfResult *out) {
    memset(out, 0, sizeof *out);
    if (!c || !f || !f->body) return;

    Facts facts;
    memset(&facts, 0, sizeof facts);

    /* Bindings are discovered by walking statements before the analysis proper, so that
     * the fact table has an entry for every one of them; a variable that is only read
     * still needs a row. */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (p->cname) raiseTo(&facts, p->cname, 0);
    }
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

/* Turn a result table back into the `Facts` shape the transfer functions use, so that
 * `dfExprDepth` can be reused on the fixed point. */
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

int dfValueDepth(Checker *c, const DfResult *r, Expr *e) {
    if (!c || !r || r->overflow || !e) return 0;
    Facts f;
    factsFromResult(r, &f);
    return dfExprDepth(c, &f, e, 0);
}

int dfLookup(const DfResult *r, const char *cname) {
    if (!r || !cname || r->overflow) return -1;
    for (int i = 0; i < r->nvars; i++)
        if (r->vars[i].cname == cname || strcmp(r->vars[i].cname, cname) == 0)
            return r->vars[i].depth;
    return -1;
}
