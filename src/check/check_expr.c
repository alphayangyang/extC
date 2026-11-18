/**
 * @file check_expr.c
 * @brief Expression checking: the type of every expression, and the checks that only the shape of an
 *        expression can decide.
 *
 * The recursive core of the checker: it resolves names to bindings, rewrites a few nodes in place (a
 * variant access becomes an enum value, a generic call becomes a plain call), and decides the arena
 * tier of every allocation site. The borrow and lifetime rules themselves live on the checker's
 * escape-analysis side.
 */

#include <stdlib.h>
#include "check_internal.h"
#include "plan.h"          /* plan/analysis products: written through setters, read through accessors */
/* stdlib directory, handed to the checker by the loader through `hintIndexSetStdDir`; NULL means the
 * INDEX hint is unavailable. */
static const char *stdDirSeen = NULL;

/**
 * @brief Deepest expression tree the checker will walk, in levels.
 *
 * The parser bounds nesting in the source, not the depth of the tree it produces: `a + b + c + ...`
 * is flat to read and a chain of `EX_BIN` nodes one operand long to walk. Every recursive pass over
 * the tree descends once per level, so past this depth the compile would die of a stack overflow
 * instead of reporting that the program is too big -- the same reasoning as `PARSE_DEPTH_LIMIT`,
 * with a larger number because a chain of operations is easier to produce by machine than a chain of
 * parentheses. 2000 nested operations already fit in the stack of a sanitizer build with room to
 * spare; this limit leaves that margin.
 */
#define EXPR_DEPTH_LIMIT 2048

/**
 * @brief Hand the stdlib directory to the checker, for reading "no such method" hints from `INDEX`.
 *
 * @param stdDir  stdlib directory; the hint reads it on the first miss, on the error path only.
 */
void hintIndexSetStdDir(const char *stdDir) { stdDirSeen = stdDir; }

/**
 * @brief Consult `stdlib/INDEX` and return the module declaring the `impl` method `method`.
 *
 * An `impl` block becomes visible only after its module is **semantically imported**, so "there is no
 * `parseJSON` on string" almost always means "the module declaring it was not imported" -- the most
 * useful sentence a diagnostic can add. Loading candidate modules just to say that would break import
 * isolation, so the answer is precomputed by the index generator and committed as `stdlib/INDEX`
 * (`INDEX`'s columns are fixed by its generator); this function reads it **on the first miss, on the
 * error path only**, once.
 *
 * @param method  Method name (as the user writes it, unflattened); NULL returns immediately.
 * @return        Name of the module declaring the method; NULL when INDEX is unavailable or the method
 *                is absent.
 */
const char *modulesMethodHint(const char *method) {
    static char names[256][96];
    static char mods[256][96];
    static int  n = -1;
    if (n < 0) {
        n = 0;
        if (stdDirSeen && method) {
            char path[1024];
            snprintf(path, sizeof path, "%s/INDEX", stdDirSeen);
            FILE *f = fopen(path, "rb");
            if (f) {
                char line[256];
                while (n < 256 && fgets(line, sizeof line, f)) {
                    if (line[0] == '#') continue;
                    char *tab1 = strchr(line, '\t'); if (!tab1) continue; *tab1++ = '\0';
                    char *tab2 = strchr(tab1, '\t'); if (!tab2) continue; *tab2++ = '\0';
                    /* Columns past the module name (visibility, kind) belong to the manual's
                     * coverage tooling, not to this diagnostic. */
                    char *tab3 = strchr(tab2, '\t'); if (tab3) *tab3 = '\0';
                    char *nl = strchr(tab2, '\n');   if (nl) *nl = '\0';
                    snprintf(names[n], sizeof names[n], "%.95s", line);
                    snprintf(mods[n],  sizeof mods[n],  "%.95s", tab2);
                    n++;
                }
                fclose(f);
            }
        }
    }
    if (!method) return NULL;
    for (int i = 0; i < n; i++) if (strcmp(names[i], method) == 0) return mods[i];
    return NULL;
}



/**
 * @brief Return the closed interval `[lo, hi]` that a **builtin integer** type covers.
 *
 * Folding two integer literals is left to the C compiler (gcc's constant folding), and that is exactly
 * what gcc rejects under `-Werror` (`int32_t n = (2000000000 + 2000000000);`), so the range has to be
 * decided here first.
 *
 * @param t   Type; returns false for NULL, a non-`TY_BUILTIN` type, or an unknown builtin integer.
 * @param lo  Receives the lower bound; untouched when false is returned.
 * @param hi  Receives the upper bound as `unsigned long long` to hold the maximum of u64; untouched when
 *            false is returned.
 * @return    True when the builtin integer type is recognised.
 */
static bool intLitRange(Type *t, long long *lo, unsigned long long *hi) {
    if (!t || t->kind != TY_BUILTIN || !t->name) return false;
    if (strcmp(t->name, "i8")  == 0) { *lo = -128LL;   *hi = 127ULL;   return true; }
    if (strcmp(t->name, "i16") == 0) { *lo = -32768LL; *hi = 32767ULL; return true; }
    if (strcmp(t->name, "i32") == 0) { *lo = -2147483648LL; *hi = 2147483647ULL; return true; }
    if (strcmp(t->name, "i64") == 0) { *lo = (-9223372036854775807LL - 1); *hi = 9223372036854775807ULL; return true; }
    if (strcmp(t->name, "u8")  == 0) { *lo = 0; *hi = 255ULL;   return true; }
    if (strcmp(t->name, "u16") == 0) { *lo = 0; *hi = 65535ULL; return true; }
    if (strcmp(t->name, "u32") == 0) { *lo = 0; *hi = 4294967295ULL; return true; }
    if (strcmp(t->name, "u64") == 0) { *lo = 0; *hi = 18446744073709551615ULL; return true; }
    return false;
}

/* ---------------------------------------------------------------- expressions */


/* ---------------------------------------------------------------- parallel::run
 *
 * `parallel::run(worker, out, n, threads)` is a builtin declared with `@builtin` in
 * stdlib/std/parallel.extc: the language has no first-class function values, so "run the worker on a
 * thread" has to be turned into a compiler-generated trampoline that calls it.
 *
 * Semantics (matching the library header): the interval [0,n) is split into chunks and each worker gets
 * its own [lo,hi); **`out` is split along [lo,hi) into non-overlapping pieces**, one per worker => reads
 * are shared and writes are necessarily partitioned => no new sendability analysis is needed.
 *
 * Syntactic whitelist for a worker body (conservative but sound): operators, literals, subscripts, its
 * own parameters and locals only; any call (user functions and stdlib alike), `new`, and any name that
 * is neither a parameter nor a local (module-level `var` falls in that class) are rejected. Transitivity
 * (a worker calling a user helper) and thread-local arena routing are both decided by
 * `parBodyProblem`. */

/**
 * @brief Tell whether a function declaration is the `parallel::run` builtin.
 *
 * Like the other builtins it is recognised by its rewritten builtin name plus shape, not by module path
 * (the module's path is never consulted), so a user function of the same name is unaffected.
 *
 * @param f  Function definition; NULL returns false.
 * @return   True for the builtin declaration rewritten to `parallel$run`.
 */
static bool isParRunDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "parallel$run") == 0;
}
/**
 * @brief Tell whether a function declaration is the `sys::domain::single` builtin.
 *
 * As with `parallel::run` it is recognised by its rewritten builtin name plus shape, not by module path
 * (the module's path is never consulted), so a user-written `single` function without the `@builtin`
 * marker is unaffected.
 *
 * @param f  Function definition; NULL returns false.
 * @return   True for the `domain$single` builtin declaration whose return type is the `domain` struct.
 */
static bool isDomSingleDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "domain$single") == 0 &&
           f->ret && f->ret->kind == TY_STRUCT && f->ret->name &&
           strcmp(f->ret->name, "domain") == 0;
}
/**
 * @brief Tell whether a function declaration is the `extc_viewOf(p, n)` builtin: a view over a pointer
 *        plus a length.
 *
 * Like the other builtins it is recognised by its rewritten `@builtin` name plus shape, never by module
 * path. It has to be a compiler builtin because a view cannot be **constructed** in extC: its type is a
 * generic instance, so no struct literal can assemble one (`slice<u8> { ... }` in expression position is
 * read as a type and rejected), and the pointer could only come from `ref void`, which is rejected as
 * well. Code generation emits the three C lines that build it on demand.
 *
 * @param f  Function definition; NULL returns false.
 * @return   True for this builtin with exactly two parameters and a view return type.
 */
static bool isViewOfDecl(FuncDef *f) {
    return f && f->isBuiltin && plateIsViewOfName(f->name) &&
           f->params.len == 2 && f->ret && ttIsViewType(f->ret);
}
/**
 * @brief Tell whether a type is the builtin `i64` (by the builtin's name).
 *
 * @param t  Type; NULL returns false.
 * @return   True for the builtin `i64`.
 */
static bool isI64Type(Type *t) {
    return t && t->kind == TY_BUILTIN && t->name && strcmp(t->name, "i64") == 0;
}
typedef struct { Vec names; const char *why; int depth; Checker *c; } ParQ;
static bool parQHas(ParQ *q, const char *n);
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth);
static bool parLocalStmt(void *ctx, Stmt *s);
static bool parBodyExpr(void *ctx, Expr *e);
static bool parBodyStmt(void *ctx, Stmt *s);

/**
 * @brief Collect the local names (`let`/`var`) declared in a worker body.
 *
 * @param ctx  The `ParQ` owning the name table.
 * @param s    Statement node to inspect.
 * @return     True to keep walking the children; collection itself never rejects.
 */
static bool parLocalStmt(void *ctx, Stmt *s) {
    ParQ *q = (ParQ *)ctx;
    if (s->kind == ST_VAR && s->u.var.name) *(const char **)vecPush(&q->names) = s->u.var.name;
    AstVisit v = { NULL, parLocalStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
/**
 * @brief Walk a statement's children recursively and hand their expressions to `parBodyExpr`.
 *
 * @param ctx  The `ParQ` owning the name table.
 * @param s    Statement node to walk.
 * @return     False when a child is rejected, which stops the walk.
 */
static bool parBodyStmt(void *ctx, Stmt *s) {
    AstVisit v = { parBodyExpr, parBodyStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
/**
 * @brief Check one expression in a worker body: only its own parameters and locals, and acceptable
 *        calls, are allowed.
 *
 * On rejection the first reason (English, used as is in the diagnostic) is written to `q->why`.
 *
 * @param ctx  The `ParQ` owning the name table and the reason.
 * @param e    Expression node to check.
 * @return     True when the expression may appear in a worker; false otherwise.
 */
static bool parBodyExpr(void *ctx, Expr *e) {
    ParQ *q = (ParQ *)ctx;
    if (e->kind == EX_IDENT && !parQHas(q, e->u.ident.name)) {
        if (!q->why) q->why = "it names something that is neither a parameter nor a local of the worker";
        return false;
    }
    /* `new` is allowed: every function on a worker chain carries parTlsArena => arenaRefAt yields
     * `(*extc_tls_arena)`; an allocation in the worker body gives it one more hidden home tail
     * parameter, and the trampoline passes the same thread-local arena. Safety comes from the
     * signature: it can only write `out` and only return i64 => an allocation cannot escape. */
    if (exprIsCallKind(e)) {
        FuncDef *g = planCallee(e);
        if (!g) {
            Expr *ce = e->u.call.callee;
            if (ce && ce->kind == EX_IDENT) g = findFunc(q->c, ce->u.ident.name);
        }
        if (!g) { if (!q->why) q->why = "it makes a call this check cannot resolve"; return false; }
        if (g->isExtern) {
            if (g->extThreadMask != 0) {
                if (!q->why) q->why = "it calls into shared runtime state (that declaration is not signed `Thread=0`)";
                return false;
            }
        } else {
            const char *sub = parBodyProblem(q->c, g, q->depth + 1);
            if (sub) { if (!q->why) q->why = sub; return false; }
        }
    }
    AstVisit v = { parBodyExpr, parBodyStmt, ctx };
    return astWalkExprChildren(e, &v);
}

#define PAR_DEPTH_LIMIT 16      /* recursion depth cap: reject rather than overflow the checker's own stack */
/**
 * @brief Tell whether a name is already in the collected name table.
 *
 * @param q  Name table.
 * @param n  Name to look for.
 * @return   True when the name is in the table.
 */
static bool parQHas(ParQ *q, const char *n) {
    for (size_t i = 0; i < q->names.len; i++)
        if (strcmp(*(const char **)vecAt(&q->names, i), n) == 0) return true;
    return false;
}
/**
 * @brief Decide whether a function may appear in a worker, including the **transitive** call chain.
 *
 * Rules: the body touches only its own parameters and locals (a module-level `var` is neither, so it
 * falls in the rejected class without any separate global analysis); a call may only target an extern
 * signed `Thread=0`, or another top-level function that passes this same gate. The recursion is
 * depth-limited, and past the limit it answers "unsafe" (the conservative direction). On success every
 * function on the chain is marked as having a thread-local arena.
 *
 * @param c      Checker; allocates the name table.
 * @param f      Function being checked; a body-less function (extern/builtin) passes directly.
 * @param depth  Current recursion depth, starting at 0.
 * @return       NULL on success; otherwise one rejection reason (English, spliced into the diagnostic
 *               as is).
 */
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth) {
    ParQ q;
    if (depth > PAR_DEPTH_LIMIT) return "the call chain is too deep to check (conservative refusal)";
    if (!f->body) return NULL;                       /* extern/builtin: governed by the rules at the call site */
    vecInit(&q.names, c->arena, sizeof(const char *));
    q.why = NULL; q.depth = depth; q.c = c;
    {
        AstVisit v = { NULL, parLocalStmt, &q };
        astWalkStmtChildren(f->body, &v);
    }
    for (size_t i = 0; i < f->params.len; i++)
        *(const char **)vecPush(&q.names) = (*(Param **)vecAt(&f->params, i))->name;
    {
        AstVisit v = { parBodyExpr, parBodyStmt, &q };
        if (!astWalkStmtChildren(f->body, &v))
            return q.why ? q.why : "an unsupported construct";
    }
    planSetParTlsArena(f, true);        /* every function on this chain: `new` goes to the thread-local arena */
    return NULL;
}

static bool exprHasCall(Checker *c, Expr *e);   /* defined at the end of this file */
static bool exprHasAnyCall(Expr *e);            /* the syntactic question; see its comment */

/**
 * @brief Record that an operator applied to a type parameter must be re-checked per instance.
 *
 * `T` inside a template is opaque: whether `T` supports an operator can only be answered once the type
 * argument is known. This record lets the consumer (`runOpCheck`) re-ask, on the instance, the same
 * predicate the concrete-type path uses. The record lives in exactly one place: three copies would
 * drift, and drift shows up as one operator accepting what another operator rejects.
 *
 * @param c   Checker.
 * @param e   The `EX_BIN` node carrying the operator.
 * @param op  The operator, which decides what an instance has to provide.
 *
 * Outside a function body there is nothing to defer (`c->curFunc` empty returns immediately): a global
 * initializer must be constant, and comparisons there are reported on their own terms.
 */
static void deferOp(Checker *c, Expr *e, const char *op) {
    planSetNeedOp(e, true);
    if (!c->curFunc) return;
    OpCheck *oc = (OpCheck *)arenaAllocZero(c->arena, sizeof(OpCheck));
    oc->node  = e;
    oc->op    = op;
    oc->func  = c->curFunc;
    oc->owner = c->curFunc->owner;   /* NULL for a free function */
    *(OpCheck **)vecPush(&c->opChecks) = oc;
}

/**
 * @brief Supply the same two hidden arguments `EX_METHOD` gets, for an overloadable operator.
 *
 * An operator **is** a method call: the left operand is the receiver, the right operand the single
 * argument, and the callee may allocate (`usesHome`) or build a pool (`makesPool`), so the call site
 * owes the same hidden arguments through the same two functions the method path uses.
 *
 * The ZONE half cannot be skipped: `push`/`append` on an SSO string build a pool when the payload leaves
 * the inline buffer, so operators that read a string (`io >> string`, `ifstream >> string`) are
 * pool-building callees too; without this step the declaration carries `int64_t __extc_home_zone` while
 * the call site passes two arguments -- the generated C would not compile.
 *
 * `setCallZoneArg` fixes the tier first, then `callHomeDepth` records an escape point from both
 * operands; the recording is unconditional (`makesPool` is a closure, still false here), and a final
 * pass filters by the flag and lowers the tier.
 *
 * @param c  Checker.
 * @param e  The `EX_BIN` node carrying the operator.
 * @param m  The operator method the checker resolved for this node; NULL performs only the zone half.
 */
static void setOpCallArgs(Checker *c, Expr *e, FuncDef *m) {
    setCallZoneArg(c, e);
    if (!m) return;
    /* The right operand fills the operator's second parameter, which is by value for the operators an
     * `@value` type defines (`fn ==(self: ref string, other: string)`). Every other by-value position
     * builds the copy with `ckValueCopyNeedsClone`, but an operator is desugared to its method here
     * rather than in the method-call path, so this is the only place that can decide it. Without the
     * decision the call hands the operand's bytes over: the callee's parameter drop on the way out
     * frees storage the caller still points at. */
    if (m->params.len >= 2) {
        Param *rp = *(Param **)vecAt(&m->params, 1);
        if (rp && rp->type && ckValueCopyNeedsClone(c, rp->type, e->u.bin.right))
            planSetArgClone(e->u.bin.right, true);
    }
    if (!c->eSites.arena) return;
    /* Only a `mut ref` operand can be written to, so only then is there an escape point to record:
     * `==` / `<` pass both operands as `ref` and record nothing. */
    bool anyMut = false;
    for (size_t i = 0; i < m->params.len && i < 2; i++) {
        Param *p = *(Param **)vecAt(&m->params, i);
        if (p->type && p->type->kind == TY_REF && p->type->mut) anyMut = true;
    }
    if (!anyMut) return;
    /* The operator has exactly two parameters for two operands, in the same order (`checkOperatorSig`
     * guarantees it), so it can go straight to the method path's own recorder instead of duplicating
     * the depth/escape rules. */
    Vec args;
    vecInit(&args, c->arena, sizeof(Expr *));
    *(Expr **)vecPush(&args) = e->u.bin.left;
    *(Expr **)vecPush(&args) = e->u.bin.right;
    (void)callHomeDepth(c, &args, &m->params, e);
}

/**
 * @brief Match an operator exactly by the right operand's type; a literal has no type of its own, so it
 *        has to be matched by the widths it can fit.
 *
 * When `vec2` defines both `*(vec2, f64)` and `*(vec2, i64)`, the `2` in `v * 2` defaults to `i32` and
 * an exact match finds nothing. The literal's own value already constrains the candidates
 * (`literalFits`), so the first hit is taken in the order `i64`, `f64`, `i32`, ... . This is not
 * overload ranking: the candidate set is decided by the literal's value, and there is no "nearer
 * conversion" rule.
 *
 * @param c         Checker.
 * @param st        Left operand (the type carrying the operator method).
 * @param op        Operator name.
 * @param fallback  Fallback operator name (`!=` -> `==`); may be `NULL`.
 * @param rhs       Right operand expression.
 * @return          The method found; NULL when the right operand is not a literal or no candidate
 *                  matches.
 */
static FuncDef *findOpLiteral(Checker *c, Type *st, const char *op, const char *fallback, Expr *rhs) {
    if (!rhs || !isNumericLit(rhs)) return NULL;
    static const char *cands[] = {"i64", "f64", "i32", "u32", "i16", "u16", "i8", "u8", "f32"};
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        Type *t = ttFromName(c->tt, cands[i]);
        if (!t || ttIsError(t) || !literalFits(rhs, t)) continue;
        FuncDef *m = findOp(c->tt, st, op, ttBase(t), fallback);
        if (m) return m;
    }
    return NULL;
}

/**
 * @brief Reject handing an owning value to a position that would copy it.
 *
 * A `@noCopy` value carries storage. A by-value parameter, an enum payload and a struct-literal
 * field all own what they are given, so the value has to arrive by transfer. A plain name is
 * transferred (the destination owns it from then on); a field, an element or a dereference is not,
 * and copying one would leave two owners of one storage -- both would free it.
 *
 * The transfer bookkeeping lives here rather than at each call site so that every value position
 * reaches the same answer: `f(s)` moves `s`, `f(s[0])` is rejected, `f(make())` binds a temporary.
 *
 * @param c     The checker.
 * @param dst   The type the value is handed to.
 * @param a     The expression handed over.
 * @param what  A short name for the destination, used in transfer bookkeeping.
 */
/**
 * @brief Does this type own storage that a transfer has to carry along?
 *
 * An enum payload is stored **inside** the value being constructed, so an operand that is a plain
 * name is transferred into it rather than copied: the name is dead afterwards and the payload owns
 * what the name held. The question is the one the generator asks with `typeNeedsDrop` -- an enum
 * recurses into its payloads, a struct into its fields -- and it is kept in step here because the
 * checker has to record exactly the move the generator marks, or one side would free storage the
 * other side still owns.
 *
 * @param c      The checker (for the instantiation context).
 * @param t      The type; may be NULL.
 * @param depth  Nesting already walked, capped so a pathological type cannot loop.
 * @return       True when moving the value hands storage to the destination.
 */
static bool payloadOwnsStorage(Checker *c, Type *t, int depth) {
    if (!t || depth > 8) return false;
    Type *st = tsub(c, t);
    if (st && (st->kind == TY_REF || ttIsViewType(st))) return false;
    Type *b = ttBase(st);
    if (b && b->kind == TY_ENUM && b->edef) {
        for (size_t i = 0; i < b->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&b->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (b->edef->typeParams.len > 0 && b->targs.len == b->edef->typeParams.len)
                    pt = ttSubstitute(c->tt, pt, &b->edef->typeParams, &b->targs);
                if (payloadOwnsStorage(c, pt, depth + 1)) return true;
            }
        }
        return false;
    }
    /* Everything that is not an enum is the question `typeHasDrop` already answers: the hook of a
     * struct, or a field that owns storage, recursion included. */
    return typeHasDrop(c, b, depth);
}

/**
 * @brief An argument that gives its storage away may only be moved in, never copied.
 *
 * See the declaration in `check_internal.h` for the rule; this definition exists so the check is
 * available to the expression walker without a second pass.
 *
 * @param c     The checker.
 * @param dst   The parameter type the argument is matched against.
 * @param a     The argument expression.
 * @param what  What the argument is, for the diagnostic.
 */
void ckOwningArgMoveOnly(Checker *c, Type *dst, Expr *a, const char *what) {
    (void)what;   /* the transfer is recorded where the code generator marks moves */
    if (!c || !dst || !a || ttIsError(dst)) return;
    Type *dt = tsub(c, dst);
    if (!dt || dt->kind == TY_REF) return;
    if (ttHasParam(dt)) { ckDeferOwningRead(c, dst, a, what); return; }  /* opaque inside a template */   /* a reference borrows: nothing is owned */
    Type *b = ttBase(dt);
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd || !sd->noCopy) return;
    if (a->kind == EX_IDENT) return;         /* a plain name is transferred, not copied */
    /* A value the callee just produced (a call, a literal, a `new`) is a temporary and moves in
     * place. Reading an element, a pointee or a field hands over a **copy**: that storage belongs
     * to the aggregate, which still frees it. */
    if (a->kind != EX_INDEX && a->kind != EX_DEREF && a->kind != EX_FIELD) return;
    (void)ckRejectOwningRead(c, a, b);
}

/**
 * @brief Would copying this expression into a place of type `dst` share storage?
 *
 * An `@value` type owns its storage, so every by-value copy has to be built by the type's
 * `clone`. The declaration case is handled where the binding is created; the other three copy
 * points are a by-value argument, a struct-literal field initializer and a `return`. A temporary is
 * **moved**, not copied, and needs nothing.
 *
 * Asking the question here is also what keeps the hook reachable: code generation emits the call,
 * and reachability pruning deletes any method instance nobody planned to use -- the generated C
 * would then call a function that has no definition.
 *
 * @param c    The checker.
 * @param dst  The type the value is copied into.
 * @param src  The expression that is copied.
 * @return     true when the copy must go through `clone`.
 */
bool ckValueCopyNeedsClone(Checker *c, Type *dst, Expr *src) {
    if (!c || !dst || !src || ttIsError(dst)) return false;
    /* A reference parameter borrows instead of owning, so there is no copy to build. The question also
     * arrives for an operator's right operand, whose parameter may be `ref T`: answering "clone" there
     * would hand a temporary to `&(...)`, which is not a C lvalue. */
    Type *dt = tsub(c, dst);
    if (!dt || dt->kind == TY_REF) return false;
    /* Inside a template the parameter type is still a type parameter: neither the clone hook nor
     * "is this a place" can be decided here, so record the question for the instance replay. The
     * code generator decides the same question per instance with the substituted type. */
    if (ttHasParam(dt)) {
        ckDeferArgClone(c, dst, src);
        return false;
    }
    Type *b = ttBase(dt);
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd || !sd->valueCopy || !sd->cloneHook) return false;
    /* A malformed hook is reported on the declaration path, where the message can name the type;
     * copying through it is simply not planned. */
    if (sd->cloneHook->params.len != 1) return false;
    /* A place has storage of its own: a binding, a field / index / slice of one, and `*p` (the
     * pointee of a reference -- `isPlace` stops at the dereference, but the storage it names is the
     * caller's and a copy must be built from it). */
    /* `ttBase` unwraps a reference, so the operand's type is read as it is: TY_REF means the
     * dereference names the caller's storage. */
    bool place = isPlace(src) ||
                 (src->kind == EX_DEREF && src->u.deref.operand && src->u.deref.operand->type &&
                  src->u.deref.operand->type->kind == TY_REF);
    if (!place) return false;
    planSetUsed(sd->cloneHook, true);
    ckNoteInstUse(sd->cloneHook, b);
    return true;
}

/**
 * @brief Record that a method/operator `m` was used on this receiver type.
 *
 * Only recorded when the receiver is a **concrete instance** of that generic struct: template method
 * bodies are shared by all instances, while the checker's instance replay and code generation expand per
 * instance, so a template-level use bit cannot tell which instance really used it.
 *
 * @param m  Template definition of the method/operator.
 * @param t  Receiver type.
 */
void ckNoteInstUse(FuncDef *m, Type *t) {
    if (!m || !m->owner || m->owner->typeParams.len == 0) return;
    Type *b = t ? ttBase(t) : NULL;
    if (!b || b->kind != TY_GENERIC || b->sdef != m->owner || !b->name) return;
    planSetInstUsed(m, b->name);
}

/**
 * @brief Bookkeeping for a `new T[n]` that becomes a `@buffer` constructor call.
 *
 * `let b: buf<T> = new T[n]` is not written as `buf<T>::withCap(n)`, so the associated-call case
 * never runs for it; this does the two things that case does and codegen depends on: mark the callee
 * (and its **instance**, since a generic method body is per instance) as used, and compute the hidden
 * arena/zone arguments the constructor receives.
 *
 * @param c     The checker.
 * @param e     The `new` expression standing for the call.
 * @param ctor  The associated function taking the element count.
 * @param owner The type instance the call is made on.
 * @param args  A one-element vector holding the element-count expression.
 */
void ckNewAsBufferCall(Checker *c, Expr *e, FuncDef *ctor, Type *owner, Vec *args) {
    planSetUsed(ctor, true);
    ckNoteInstUse(ctor, owner);
    setCallZoneArg(c, e);
    if (args) (void)callHomeDepth(c, args, &ctor->params, e);
}

/**
 * @brief Compute the result type of a binary arithmetic operator.
 *
 * The result rules live only here: the bitwise operators use this same copy, and a second copy would
 * drift.
 *
 * @param c   Checker.
 * @param e   The `EX_BIN` node under check; its operator and source position are used.
 * @param lt  Left operand type.
 * @param rt  Right operand type.
 * @return    The common type of the two operands; the error type after a diagnostic was reported.
 *
 * A literal operand adapts to the other side, so `u32 + 1` has type `u32`; `%` is restricted to integers
 * here. Operands of reference type never arrive, the caller reports them first.
 */

static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;

    /* User-defined operators are tried before the numeric rules, so a struct operand reaches its own
     * method instead of "an arithmetic operator accepts only numeric types". The protocol is the same as
     * for `==`: the method name is the operator, and `checkOperatorSig` already validated the signature
     * at the definition. An operator is matched exactly by the right operand's type, so `vec2 * f64` can
     * be defined as a separate method on `vec2` (the operand's exact type picks it). */
    if (isArithOp(op)) {
        StructDef *sd = structOf(ttBase(lt));
        if (sd) {
            FuncDef *m = findOp(c->tt, ttBase(lt), op, rt, NULL);
            if (!m) m = findOpLiteral(c, ttBase(lt), op, NULL, e->u.bin.right);
            if (!m) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note,
                          "define it inside `%s`:\n"
                          "      fn %s(self: ref %s, other: %s) -> %s { ... }",
                          DN(sd), op, DN(sd), DN(sd), DN(sd));
                ckError(c, e->line, bufCstr(&note), "`%s` does not define `%s`", DN(sd), op);
                return err;
            }
            planSetCallee(e, m);
            planSetUsed(m, true);
            ckNoteInstUse(m, lt);
            setOpCallArgs(c, e, m);
            return lt;
        }
        /* **Either** operand being a type parameter has to be deferred, not just the left one:
         * `value()` returns the template's `T`, which arrives from the **right**. The instance decides
         * what the operands really are; the result type stays pinned to the parameter (the instance's
         * answer decides). */
        if (lt->kind == TY_PARAM || rt->kind == TY_PARAM) {
            deferOp(c, e, op);
            return lt->kind == TY_PARAM ? lt : rt;
        }
    }

    if (!ttIsNumeric(lt) || !ttIsNumeric(rt)) {
        ckError(c, e->line, "arithmetic operators only accept numeric types",
                "cannot apply `%s` to `%s` and `%s`", op, typeStr(c, lt), typeStr(c, rt));
        return err;
    }
    if (strcmp(op, "%") == 0 && (!ttIsInteger(lt) || !ttIsInteger(rt))) {
        ckError(c, e->line, "`%` is only meaningful for integers",
                "cannot apply `%%` to `%s` and `%s`", typeStr(c, lt), typeStr(c, rt));
        return err;
    }

    /* Two integer literals and an arithmetic operator: the folding happens in C, and C's constant
     * folding is exactly what gcc rejects -- `int32_t n = (2000000000 + 2000000000);` is "integer
     * overflow in expression", an error under `-Werror` (`-fwrapv` only defines run-time behavior). So
     * report it **on the spot**, which is how the language tells the user what to write (`i64(...)`);
     * letting it wrap would spread the boundary case through the whole language. */
    if ((strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0) &&
        e->u.bin.left->kind == EX_INT && e->u.bin.right->kind == EX_INT &&
        ttEquals(lt, rt) && ttIsInteger(lt)) {
        long long lo; unsigned long long hi;
        if (intLitRange(lt, &lo, &hi)) {
            long long a = e->u.bin.left->u.ival;
            long long b = e->u.bin.right->u.ival;
            bool over = false;
            long long r = 0;
            /* Check for overflow before computing: `a + b` itself may overflow `long long`, which is
             * undefined behavior. A result outside `long long` is already outside any of these types. */
            if (op[0] == '+') {
                if (b > 0 && a > 9223372036854775807LL - b) over = true;
                else if (b < 0 && a < (-9223372036854775807LL - 1) - b) over = true;
                else r = a + b;
            } else if (op[0] == '-') {
                if (b < 0 && a > 9223372036854775807LL + b) over = true;
                else if (b > 0 && a < (-9223372036854775807LL - 1) + b) over = true;
                else r = a - b;
            } else {   /* `*` */
                if (a == 0 || b == 0) r = 0;
                else if (a > 0 && b > 0 && a > 9223372036854775807LL / b) over = true;
                else if (a > 0 && b < 0 && b < (-9223372036854775807LL - 1) / a) over = true;
                else if (a < 0 && b > 0 && a < (-9223372036854775807LL - 1) / b) over = true;
                else if (a < 0 && b < 0 && a < 9223372036854775807LL / b) over = true;
                else r = a * b;
            }
            /* The upper bound is unsigned (u64 does not fit in `long long`), so convert only
             * **non-negative** results: converting `-1` to unsigned yields a huge value and would make
             * every negative constant look like an overflow. */
            if (!over && (r < lo ||
                          (r >= 0 && hi <= 9223372036854775807ULL && (unsigned long long)r > hi)))
                over = true;
            if (over) {
                ckError(c, e->line,
                        "a constant that does not fit its type is reported here, not left to the C"
                        " compiler: it folds the expression and rejects the overflow",
                        "the constant `%lld %s %lld` overflows `%s`; write the type you mean,"
                        " e.g. `i64(...)`",
                        a, op, b, typeStr(c, lt));
                return err;
            }
        }
    }

    /* A literal adapts by value to the other side: the `1` in `u32 + 1` is a u32. */
    if (isNumericLit(e->u.bin.left) && ttIsNumeric(rt)) {
        if (!literalFits(e->u.bin.left, rt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, rt));
            return err;
        }
        return rt;
    }
    if (isNumericLit(e->u.bin.right) && ttIsNumeric(lt)) {
        if (!literalFits(e->u.bin.right, lt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, lt));
            return err;
        }
        return lt;
    }

    if (ttEquals(lt, rt)) return lt;
    if (ttCanWiden(lt, rt)) return rt;
    if (ttCanWiden(rt, lt)) return lt;

    ckError(c, e->line, "these two types have no lossless conversion; convert explicitly first",
            "`%s` and `%s` have no common type for `%s`",
            typeStr(c, lt), typeStr(c, rt), op);
    return err;
}
/**
 * @brief Resolve the operator of a compound assignment and return the type of `target op value`.
 *
 * The statement keeps its written form and the operation is handed to the same arithmetic check `x + y`
 * goes through, so the arithmetic rules, literal adaptation, reference rejection, and user-defined
 * operators all have exactly one answer. A user-defined operator takes its receiver as a reference, and
 * the generated call has to name the target once more, so a target containing a call is reported with a
 * hint to split it over two lines.
 *
 * @param c       Checker.
 * @param op      Compound operator as written; its first character is the matching binary operator (all
 *                five are single characters).
 * @param target  Left-hand target, already checked.
 * @param value   Right-hand value, already checked.
 * @param tgt     Type of the target.
 * @param val     Type of the value.
 * @param out     Receives the resolved `EX_BIN` node; NULL when the caller does not need it.
 * @return        The type of `target op value`; the error type when the operator does not apply or the
 *                target contains a call.
 */
Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                      Type *tgt, Type *val, Expr **out) {
    Expr *bin = exprNew(c->arena, EX_BIN, target->line);
    bin->u.bin.op    = arenaStrndup(c->arena, op, 1);   /* `+=` asks about the `+` operator's meaning */
    bin->u.bin.left  = target;
    bin->u.bin.right = value;
    Type *rt = checkArith(c, bin, tgt, val);
    if (out) *out = bin;
    if (ttIsError(rt)) return rt;
    if (planCallee(bin) && exprHasAnyCall(target)) {
        ckError(c, target->line,
                "the statement is rewritten as `x = add(&x, y)`, so the target has to be a"
                " place that can be named twice without side effects",
                "the target has a call in it -- split it into two lines: `var t = ...`"
                " then `t %s ...`", op);
        return ttError(c->tt);
    }
    return rt;
}

/**
 * @brief Tell whether `op` is one of the bit operators `&`, `|`, `^`, `<<`, `>>`.
 *
 * The result type of these operators comes from `checkArith`; the rule is not copied here, so the two
 * cannot drift apart.
 *
 * @param op  Operator spelling.
 * @return    True when it is one of the five.
 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

/**
 * @brief Bit width of a builtin integer type (`i32` -> 32); 0 when it is not a builtin integer with a
 *        width.
 *
 * The only name shape carrying a number is `[iu]\d+`, the same measure codegen reads the width with: a
 * shift amount has to be comparable with it at compile time.
 *
 * @param t  Type.
 * @return   The bit width; 0 for `bool`, floats, and non-builtin types.
 */
static int builtinIntBits(Type *t) {
    if (!t || t->kind != TY_BUILTIN || !t->name) return 0;
    const char *q = t->name;
    if (*q != 'i' && *q != 'u') return 0;
    q++;
    if (*q < '0' || *q > '9') return 0;
    int bits = 0;
    while (*q >= '0' && *q <= '9') { bits = bits * 10 + (*q - '0'); q++; }
    return *q == 0 ? bits : 0;
}

/**
 * @brief Tell whether a type is a reference (nullable or not).
 *
 * @param t  Type; NULL yields false.
 * @return   True when it is `TY_REF`.
 */
static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/**
 * @brief Report "an arithmetic or comparison operator was applied to a reference" and return the error
 *        type.
 *
 * In C this would silently become pointer arithmetic, which is not what the user wrote; an error is
 * better than an answer that looks right while actually moving a pointer.
 *
 * @param c   Checker.
 * @param e   The `EX_BIN` node being checked; its source location is used.
 * @param lt  Left operand type.
 * @param rt  Right operand type.
 * @param op  Operator spelling, used in the message.
 * @return    The error type, which the caller may return directly.
 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

/**
 * @brief The two buffer primitives: `extc_sizeOf<T>()` and `extc_viewOfT<T>(p, n)`.
 *
 * `extc_sizeOf<T>()` is the size of one element in bytes. extC has no `sizeof`, and a
 * buffer has to convert between "elements" and "bytes of reservation", so the one
 * question the language cannot ask is answered here.
 *
 * `extc_viewOfT<T>(p, n)` re-types a byte pointer as a `mut slice<T>` of `n` elements.
 * Like `extc_viewOf` it checks **nothing** at run time: the caller owns the range (the
 * buffer keeps it inside one reservation, and its own `len`/`cap` are what bounds it).
 * It exists because `extern!` may not return a view and extC has no pointer casts.
 *
 * @param c     Checker.
 * @param e     Generic call node; its name and arguments are used.
 * @param elem  The resolved element type `T`.
 * @return      `i64` for `extc_sizeOf`, `mut slice<T>` for `extc_viewOfT`; the error
 *              type when the arguments do not match.
 */
static Type *checkBufPrim(Checker *c, Expr *e, Type *elem) {
    TypeTable *tt = c->tt;
    const char *nm = e->u.gencall.name;
    if (strcmp(nm, "extc_sizeOf") == 0) {
        if (e->u.gencall.args.len != 0) {
            ckError(c, e->line, NULL, "`extc_sizeOf` takes no arguments, got %zu",
                    e->u.gencall.args.len);
            return ttError(tt);
        }
        return c->tI64;
    }
    /* `elemDrop<T>(v, from, to)` / `elemTake<T>(v, i)`: the two element-level operations a
     * container needs and the language cannot express -- run the element's `@drop` hook in place
     * over a range, and move one element out leaving a zero behind. The view argument names the
     * element type, and it has to be **writable** (`mut slice<T>`): both operations write the slot
     * they visit. A hook-less element type makes `elemDrop` emit nothing, so the container code
     * does not have to be written twice. */
    /* `elemPut<T>(v, i, val)`: store a value into a slot that holds **no** element, without
     * running the old content's hook. A container built on dirty, reused storage cannot use an
     * ordinary store: `t[i] = val` is a *replacement*, so the generator first drops whatever the
     * slot already holds -- and for a slot the container does not own that would run a hook on
     * leftover bytes from the storage's previous user. The value is **transferred** into the slot:
     * the caller loses it, the slot owns it, and the name it came from is dead from here on. */
    if (strcmp(nm, "elemPut") == 0) {
        if (e->u.gencall.args.len != 3) {
            ckError(c, e->line, NULL,
                    "`elemPut` takes 3 arguments (a `mut slice<T>`, an index, and the value)",
                    "got %zu", e->u.gencall.args.len);
            return ttError(tt);
        }
        Expr *av = *(Expr **)vecAt(&e->u.gencall.args, 0);
        Type *tv = ttBase(checkValue(c, av));
        if (!ttIsError(tv) && !ttIsViewType(tv)) {
            ckError(c, av->line, NULL,
                    "`elemPut` writes one element of a view, so it wants a `mut slice<T>`",
                    "found `%s`", typeStr(c, tv));
            return ttError(tt);
        }
        if (!ttIsError(tv) && !tv->mut) {
            ckError(c, av->line, NULL, NULL,
                    "`elemPut` writes the slot it stores into, so the view must be `mut`");
            return ttError(tt);
        }
        Type *ti = checkValue(c, *(Expr **)vecAt(&e->u.gencall.args, 1));
        if (!ttIsError(ti) && !ttIsInteger(ti))
            ckError(c, e->line, NULL, "`elemPut` wants an integer index", "found `%s`", typeStr(c, ti));
        Expr *val = *(Expr **)vecAt(&e->u.gencall.args, 2);
        Type *et = ttIsError(tv) ? ttError(tt) : *(Type **)vecAt(&tv->targs, 0);
        Type *ta = ttIsError(et) ? ttError(tt) : checkInto(c, et, val);
        if (!ttIsError(et) && !ttIsError(ta))
            checkAssignable(c, et, ta, val, "stored value");
        if (!ttIsError(et)) {
            ckOwningArgMoveOnly(c, et, val, "stored value");
            /* The slot owns it now: the name it came from is dead (see the enum-payload case). */
            if (val->kind == EX_IDENT && payloadOwnsStorage(c, et, 0)) {
                Sym *srcSym = lookup(c, val->u.ident.name);
                if (srcSym && srcSym->movedLine == 0) {
                    srcSym->movedLine = e->line;
                    srcSym->movedInto = "a slot";
                }
            }
        }
        return c->tBool;
    }

    if (strcmp(nm, "elemDrop") == 0 || strcmp(nm, "elemTake") == 0) {
        const bool take = nm[4] == 'T';
        const size_t want = take ? 2 : 3;
        if (e->u.gencall.args.len != want) {
            ckError(c, e->line, NULL,
                    take ? "`elemTake` takes 2 arguments (a `mut slice<T>` and an index)"
                         : "`elemDrop` takes 3 arguments (a `mut slice<T>` and the range start and end)",
                    "got %zu", e->u.gencall.args.len);
            return ttError(tt);
        }
        Expr *av = *(Expr **)vecAt(&e->u.gencall.args, 0);
        Type *tv = ttBase(checkValue(c, av));
        if (!ttIsError(tv) && !ttIsViewType(tv)) {
            ckError(c, av->line, NULL,
                    take ? "`elemTake` moves an element out of a view slot, so it wants a `mut slice<T>`"
                         : "`elemDrop` runs the element hook in place, so it wants a `mut slice<T>`",
                    "found `%s`", typeStr(c, tv));
            return ttError(tt);
        }
        if (!ttIsError(tv) && !tv->mut) {
            ckError(c, av->line, NULL, NULL,
                    take ? "`elemTake` writes the slot it moves out of, so the view must be `mut`"
                         : "`elemDrop` writes the slot it visits, so the view must be `mut`");
            return ttError(tt);
        }
        for (size_t i = 1; i < e->u.gencall.args.len; i++) {
            Type *ti = checkValue(c, *(Expr **)vecAt(&e->u.gencall.args, i));
            if (!ttIsError(ti) && !ttIsInteger(ti)) {
                ckError(c, e->line, NULL,
                        take ? "`elemTake` wants an integer index" : "`elemDrop` wants integer range bounds",
                        "found `%s`", typeStr(c, ti));
                return ttError(tt);
            }
        }
        if (take && !ttIsError(tv)) return *(Type **)vecAt(&tv->targs, 0);
        /* `elemDrop` is a statement primitive: it has no result to use. */
        return ttVoid(tt);
    }

    /* `extc_viewOfT<T>(p, n)`: the same two shapes `extc_viewOf` insists on -- a byte
     * pointer and an integer length -- with the element type coming from the type argument. */
    if (e->u.gencall.args.len != 2) {
        ckError(c, e->line, NULL, "`extc_viewOfT` takes 2 arguments (a byte pointer and an element count), got %zu",
                e->u.gencall.args.len);
        return ttError(tt);
    }
    Expr *ap = *(Expr **)vecAt(&e->u.gencall.args, 0);
    Expr *an = *(Expr **)vecAt(&e->u.gencall.args, 1);
    Type *tp = checkExpr(c, ap);   /* the reference **is** the value here, as in extc_viewOf */
    Type *tn = checkValue(c, an);
    if (!ttIsError(tp) && !(tp->kind == TY_REF &&
                            ttBase(tp->inner) && ttBase(tp->inner)->kind == TY_BUILTIN))
        ckError(c, ap->line,
                "It re-types a pointer without checking anything, so it insists on the one"
                " shape it can mean: `ref u8`.",
                "`extc_viewOfT` wants a `ref u8`, found `%s`", typeStr(c, tp));
    if (!ttIsError(tn) && !ttIsInteger(tn))
        ckError(c, an->line, NULL,
                "`extc_viewOfT` wants an integer element count, found `%s`", typeStr(c, tn));
    return ttViewMut(tt, sliceOf(c, elem), true);
}
/**
 * @brief Check the weak-reference primitives `weakAddr` / `weakTag` / `weakGen` / `weakUpgrade`.
 *
 * A handle is three words the library owns: the address, the slot version and the slab serial. The
 * compiler part is the type. The target type has to be written out in C so the runtime can hand a
 * `T *` back, and `weakUpgrade` has to come back as `?ref T`: a reference that may be absent,
 * which is exactly what a reclaimed object is. That is why the zero value of the result is
 * `null` rather than a runtime error -- object death is an expected event, not a failure.
 *
 * @param c     The checker.
 * @param e     The `EX_GENCALL` node.
 * @param elem  The resolved type argument `T`.
 * @return      `i64` for the three words of a handle, `?ref T` for `weakUpgrade`.
 */
static Type *checkWeakPrim(Checker *c, Expr *e, Type *elem) {
    TypeTable *tt = c->tt;
    const char *nm = e->u.gencall.name;
    const bool upgrade = strcmp(nm, "weakUpgrade") == 0;
    if (e->u.gencall.args.len != (upgrade ? 3u : 1u)) {
        ckError(c, e->line, NULL, "`%s` takes %s, got %zu", nm,
                upgrade ? "the three words of a handle" : "one reference", e->u.gencall.args.len);
        return ttError(tt);
    }
    if (!upgrade) {
        Expr *a = *(Expr **)vecAt(&e->u.gencall.args, 0);
        Type *ta = checkExpr(c, a);   /* the reference **is** the value here, as in extc_viewOf */
        if (!ttIsError(ta) && !(ta->kind == TY_REF && ttBase(ta->inner) && ttEquals(ttBase(ta->inner), elem)))
            ckError(c, a->line,
                    "A weak reference names one object, so the argument has to be a reference to the"
                    " type argument.",
                    "`%s` wants a `ref %s`, found `%s`", nm, typeStr(c, elem), typeStr(c, ta));
        return c->tI64;   /* the handle word: the library stores it, the runtime reads it */
    }
    for (size_t i = 0; i < 3; i++) {
        Expr *a = *(Expr **)vecAt(&e->u.gencall.args, i);
        Type *ta = checkValue(c, a);
        if (!ttIsError(ta) && !ttIsInteger(ta))
            ckError(c, a->line, NULL, "`weakUpgrade` takes the three words of a handle (the fields of a `weak<T>`),"
                    " found `%s` for argument %zu", typeStr(c, ta), i + 1);
    }
    Type *r = ttRef(tt, elem);
    r->nullable = true;   /* the object may be gone: `null` is the answer, not a trap */
    return r;
}

/**
 * @brief Check the batch copy `copyInto<T>(dst, src, n)` and type it `i64`.
 *
 * One checked `memmove`: the destination and the source are views of the same element type, the
 * destination is `mut`, and the count is an integer. There is no other way in the language to move
 * a batch (a loop pays the bounds check per element, and a container pays its own guard per
 * element), and no library function can express it: `extern!` may not return `slice<T>` and
 * extC has no pointer casts, so the generator writes the element type out.
 *
 * The arena tier is `deliberately` not recorded here: the batch copy does not allocate, it writes
 * into storage that already exists, so the escape rules have nothing to say about it.
 *
 * @param c     Checker.
 * @param e     Generic call node; its name and arguments are used.
 * @param elem  Element type written at the call site.
 * @return      `i64` (the count); the error type when the arguments are wrong.
 */
static Type *checkCopyInto(Checker *c, Expr *e, Type *elem) {
    TypeTable *tt = c->tt;
    const char *nm = e->u.gencall.name;
    /* `copyInto<T>(dst: mut slice<T>, src: slice<T>, n) -> i64`: one checked `memmove`, the only way in
     * the language to move a batch (a loop pays the bounds check per element, and the container pays its
     * own guard per element). The generator writes the element type out, which is why the name needs a
     * branch of its own in the backend. */
    if (strcmp(nm, "copyInto") == 0) {
        if (e->u.gencall.args.len != 3) {
            ckError(c, e->line, NULL, "`copyInto` takes 3 arguments (the destination, the source,"
                                      " and how many elements)");
            return ttError(tt);
        }
        Expr *dE = *(Expr **)vecAt(&e->u.gencall.args, 0);
        Expr *sE = *(Expr **)vecAt(&e->u.gencall.args, 1);
        Expr *nE = *(Expr **)vecAt(&e->u.gencall.args, 2);
        Type *dT = checkValue(c, dE);
        Type *sT = checkValue(c, sE);
        Type *nT = checkValue(c, nE);
        if (!ttIsError(dT)) {
            Type *de = viewElemOf(dT);
            if (!de || !ttEquals(de, elem) || !dT->mut) {
                ckError(c, dE->line, NULL,
                        "`copyInto` writes into its destination, so that must be `%s`, not `%s`",
                        typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, dT));
                return ttError(tt);
            }
        }
        if (!ttIsError(sT)) {
            Type *se = viewElemOf(sT);
            if (!se || !ttEquals(se, elem)) {
                ckError(c, sE->line, NULL, "`copyInto` copies from `%s`, not `%s`",
                        typeStr(c, sliceOf(c, elem)), typeStr(c, sT));
                return ttError(tt);
            }
        }
        if (!ttIsError(nT) && !ttIsInteger(nT)) {
            ckError(c, nE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, nT));
            return ttError(tt);
        }
        if (elem->kind == TY_PARAM || ttHasParam(elem)) recordNewSizeCheck(c, elem, e->line);
        return c->tI64;
    }
    /* The caller routes only `copyInto` here; the fall-through keeps the checker total. */
    return c->tI64;
}

/**
 * @brief Whether a trait's whole interface is one `call` method (the shape a closure can satisfy).
 *
 * A closure has exactly one method, `call`; a trait with any other name or with a second method
 * asks for something the compiler cannot synthesise from an environment struct.
 *
 * @param c   The checker.
 * @param tr  The trait.
 * @return    true when the trait is a one-method `call` interface.
 */
static bool traitIsCallable(Checker *c, TraitDef *tr) {
    (void)c;
    if (!tr || tr->methods.len != 1) return false;
    FuncDef *m = *(FuncDef **)vecAt(&tr->methods, 0);
    return m && m->name && strcmp(m->name, "call") == 0 && m->params.len >= 1;
}

/**
 * @brief The synthesised `call` method of a lambda's environment type, or NULL.
 *
 * @param pt  The environment type.
 * @return    The `call` method.
 */
static FuncDef *lambdaCallMethod(Type *pt) {
    if (!pt || !pt->sdef) return NULL;
    for (size_t i = 0; i < pt->sdef->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&pt->sdef->methods, i);
        if (m->name && strcmp(m->name, "call") == 0) return m;
    }
    return NULL;
}

/**
 * @brief Whether a value of this type is a self-contained copy -- no pointer into anyone's storage.
 *
 * The payload of a `dyn` value lives in a slot on a plate; nothing about it is region-checked, so a
 * pointer inside it would outlive whatever it points at without the analysis noticing. Only plain
 * scalars, `fn` pointers (code, not storage), fixed arrays of those, and structs whose fields are all
 * of those qualify; references, views, `dyn` handles, enums with payloads and generic instances are
 * refused. The recursion is capped: a struct nested deeper than the cap is refused rather than
 * examined further, since the conservative answer is the safe one.
 *
 * @param t      The captured value's type.
 * @param depth  Recursion depth (refused past four levels of struct nesting).
 * @return       true when every byte of the value is the value's own.
 */
static bool captureOwnsItsBytes(Type *t, int depth) {
    if (!t || depth > 4) return false;
    if (t->kind == TY_REF || t->kind == TY_DYN) return false;
    if (ttIsViewType(t)) return false;
    switch (t->kind) {
    case TY_BUILTIN: return !ttIsViewType(t);
    case TY_FN:      return true;
    case TY_ARRAY:   return captureOwnsItsBytes(t->inner, depth + 1);
    case TY_STRUCT: {
        if (!t->sdef) return false;
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            if (!captureOwnsItsBytes(fd->type, depth + 1)) return false;
        }
        return true;
    }
    default:
        /* kind-default: every other kind -- a `ref`, a `dyn` handle, an enum with a payload, a generic
         * instance -- either points outside the value or is refused above; only a self-contained copy
         * may be a `dyn` payload, and the conservative answer for a shape nobody listed is "no". */
        return false;
    }
}

/**
 * @brief Whether a lambda can be a `dyn` payload for a callable trait -- signature and captures.
 *
 * Reported diagnostics: a capture that is not a plain value (the payload would point at a frame),
 * and a signature that does not match the trait's `call`.
 *
 * @param c     The checker.
 * @param tr    The trait (already known to be a one-method `call` interface).
 * @param pt    The environment type.
 * @param line  Source line for the diagnostics.
 * @return      true when the compiler may write `impl tr for pt` itself.
 */
static bool lambdaCallMatches(Checker *c, TraitDef *tr, Type *pt, int line) {
    StructDef *sd = pt->sdef;
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (fd->type && mentionsParam(fd->type)) {
            /* A layout that depends on a type parameter has one layout per instance of the function
             * the closure is written in (`LamVar` in the code generator); a `dyn` payload has one
             * layout, decided by the trait, so there is nothing to write here. */
            ckError(c, line,
                    "A `dyn` payload has one layout, decided by the trait it implements. A closure"
                    " whose captured type depends on a type parameter has a different layout per"
                    " instance of the function it is written in, so it cannot be that payload.",
                    "this closure captures a value whose type depends on a type parameter");
            return false;
        }
        if (!captureOwnsItsBytes(fd->type, 0)) {
            ckError(c, line,
                    "A `dyn` value is a `{pool, slot, generation}` handle; it cannot describe a"
                    " lifetime, so the payload has to own everything it reads. A closure that"
                    " captures a reference, a view, or a value containing one would leave the"
                    " payload pointing into a frame.",
                    "this closure captures %s, so it cannot be kept in a `dyn` value",
                    (fd->type && fd->type->kind == TY_REF) || (fd->type && ttIsViewType(fd->type))
                        ? "a reference"
                        : (fd->type && fd->type->kind == TY_DYN
                               ? "a dynamically dispatched value"
                               : "a value that contains a reference"));
            return false;
        }
    }
    FuncDef *want = *(FuncDef **)vecAt(&tr->methods, 0);
    FuncDef *have = lambdaCallMethod(pt);
    if (!have) return false;
    if (have->params.len != want->params.len) {
        ckError(c, line, NULL, "`%s` takes %zu argument%s, but this closure takes %zu",
                want->name, want->params.len - 1, want->params.len == 2 ? "" : "s",
                have->params.len - 1);
        return false;
    }
    for (size_t i = 1; i < want->params.len; i++) {
        Param *wp = *(Param **)vecAt(&want->params, i);
        Param *hp = *(Param **)vecAt(&have->params, i);
        if (!wp->type || !hp->type || !ttEquals(ttBase(wp->type), ttBase(hp->type))) {
            ckError(c, line, NULL,
                    "the closure is kept in `dyn %s`, so its `call` has to take the types the"
                    " trait writes", tr->name);
            return false;
        }
    }
    Type *wr = want->ret ? ttBase(want->ret) : NULL;
    Type *hr = have->ret ? ttBase(have->ret) : NULL;
    if (wr && hr && !ttEquals(wr, hr)) {
        ckError(c, line, NULL,
                "the closure is kept in `dyn %s`, so its `call` has to return the type the trait"
                " writes", tr->name);
        return false;
    }
    return true;
}

/**
 * @brief Resolve the trait named by a `dyn` form and require that the payload implements it.
 *
 * `dyn Trait(x)` (a value) and `dyn Trait(x).m(...)` (the immediate form) share this one helper, because
 * there is only one rule and the record of `impl Trait for T` is the answer.
 *
 * @param c          Checker.
 * @param traitName  Trait name written after `dyn`.
 * @param payT       Payload type whose implementation is looked up.
 * @param line       Source line the diagnostic points at.
 * @return           The trait that matched; a diagnostic is reported and NULL returned when the trait is
 *                   unknown; when the payload does not implement it a diagnostic is reported but the
 *                   trait is still returned.
 */
static TraitDef *dynTraitOf(Checker *c, const char *traitName, Type *payT, int line) {
    TraitDef *tr = NULL;
    for (size_t i = 0; i < c->m->traits.len && !tr; i++) {
        TraitDef *cand = *(TraitDef **)vecAt(&c->m->traits, i);
        if (cand->name && strcmp(cand->name, traitName) == 0) tr = cand;
    }
    if (!tr) {
        ckError(c, line, "A trait is declared with `trait Name { ... }`, and a `dyn` value must name"
                " one that exists.", "`dyn` on unknown trait `%s`", traitName);
        return NULL;
    }
    /* The uniform table is emitted only for traits that really were dispatched on -- which is also why
     * object-unsafe methods (no receiver, generic, returning `Self`) need not appear in it. */
    planSetUsedDyn(tr, true);
    planSetUsesDyn(c->m, true);
    Type *pt = ttBase(payT);
    bool impl = false;
    for (size_t i = 0; i < c->m->impls.len && !impl; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&c->m->impls, i);
        /* `impl<T> Tag for pair<T>` covers **every** instance of `pair`, so what counts is sameness of
         * declaration: comparing types directly would never match `pair<i64>` against `pair<T>`. */
        Type *it = planImplTarget(im) ? ttBase(planImplTarget(im)) : NULL;
        impl = planImplTrait(im) == tr && it && pt &&
               (ttEquals(it, pt) || (it->sdef && it->sdef == pt->sdef));
        /* The methods of a **generic** impl are shared by every instance, so they are marked as used
         * here: that is why the per-instance emission loop writes out `pair_i64_tag`; without it the
         * `dyn` table would point at a function nobody emits. */
        if (impl && it && it->sdef && it->kind == TY_GENERIC)
            for (size_t k = 0; k < im->methods.len; k++) {
                FuncDef *mf = *(FuncDef **)vecAt(&im->methods, k);
                planSetUsed(mf, true);
                planSetDynTable(mf, true);   /* its body is named by the vt thunk, from outside */
            }
    }
    /* A **closure** used as a `dyn` payload: its environment type is synthesised by the compiler,
     * so the user cannot write `impl` for it. There is exactly one shape where the compiler can
     * write the block itself -- a trait whose whole interface is one `call` method, and a closure
     * whose synthesised `call` has the same signature. That is the heterogeneous-container case:
     * `var fns: varArray<dyn IntFn> = ...` holding two different lambdas.
     *
     * The capture set has to be **plain values**: a by-value capture of a scalar is a copy, so the
     * dyn value stays valid wherever it goes. A capture that is a reference or a view would make
     * the payload point at a frame, and `dyn` values have no way to express that lifetime (they are
     * `{pool, slot, generation}` handles), so that shape is refused with its own sentence instead
     * of being left to a runtime accident. */
    if (!impl && pt && pt->sdef && anLamSig(pt->sdef) && traitIsCallable(c, tr) &&
        lambdaCallMatches(c, tr, pt, line)) {
        ImplDef *im = (ImplDef *)arenaAllocZero(c->arena, sizeof *im);
        im->line = line;
        im->ctx = c->ctx;
        im->modName = pt->sdef->modName;
        vecInit(&im->typeParams, c->arena, sizeof(const char *));
        vecInit(&im->traitArgs, c->arena, sizeof(Type *));
        vecInit(&im->typeArgs, c->arena, sizeof(Type *));
        vecInit(&im->methods, c->arena, sizeof(FuncDef *));
        planSetImplTrait(im, tr);
        planSetImplTarget(im, pt);
        FuncDef *cf = lambdaCallMethod(pt);
        *(FuncDef **)vecPush(&im->methods) = cf;
        planSetUsed(cf, true);
        planSetDynTable(cf, true);
        *(ImplDef **)vecPush(&c->m->impls) = im;
        impl = true;
    }
    if (pt && pt->sdef && !impl)
        ckError(c, line, "A `dyn` value names a trait its payload implements, because the"
                " implementation is what the table points at.",
                "`%s` does not implement `%s`", pt->name, traitName);
    return tr;
}


/* ---------------------------------------------------------------- lambda literals --------------
 *
 * Syntax: `fn(x: i64) -> i64 { ... }`; parameters are monomorphized, so the language does not introduce
 * function pointers; a **writable capture** has to be written `[mut ref x]`, because a read capture is
 * captured by value and needs no entry.
 *
 * The order below is the whole design:
 *
 *   1. the body is checked **in place**, with the lambda's parameters in scope, so every name in the body
 *      resolves by the ordinary rules -- including shadowing; a pre-scan of names alone would get that
 *      wrong;
 *   2. what the body refers to **outside** that scope is exactly the capture set: it is read off the
 *      resolved `Sym`s, not guessed from the text;
 *   3. each capture becomes a field of a generated struct -- by value (one copy, the closure sees the
 *      value as of creation) or, for `mut ref x`, a reference to the outer variable;
 *   4. the body becomes the struct's `call` method, and every capture name is emitted as `self.<field>`
 *      (`(*self.<field>)` when writable). The last step needs no AST edit: codegen prints identifiers
 *      through their `cname`, so the rewrite is an assignment to a `cname`.
 *
 * `f(x)` (`f` holding such a value) is a call through the value; the call site rewrites it in place into
 * the method call `f.call(x)` -- the same rewrite the enum-construction branch of EX_CALL performs.
 */
typedef struct {
    Checker *c;
    size_t   nOuter;      /* scopes below this index belong to the enclosing functions */
    Vec      caps;        /* Sym*: the capture set, in declaration order */
    Vec      written;     /* Sym*: the captured variables the body writes */
    bool     usedNew;
    bool     sawReturn;
} LamWalk;

/**
 * @brief Add a binding to a vector, skipping one that is already there.
 *
 * @param v  Target vector (`Sym *`).
 * @param s  Binding to add; NULL is ignored.
 */
static void lamAddUnique(Vec *v, Sym *s) {
    if (!s) return;
    for (size_t i = 0; i < v->len; i++)
        if (*(Sym **)vecAt(v, i) == s) return;
    *(Sym **)vecPush(v) = s;
}

/**
 * @brief Tell whether a binding lives below the lambda's own scope, that is, comes from the enclosing
 *        function.
 *
 * Membership is decided by the pointer's position in the scope stack: `Sym.depth` is an **arena** depth
 * and answers a different question.
 *
 * @param w  Walk state; `nOuter` is how many scopes sit outside the lambda.
 * @param s  Binding to check.
 * @return   True when the binding lives in an outer scope.
 */
static bool lamIsOuter(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->nOuter && i < w->c->scopes.len; i++) {
        Scope *sc = *(Scope **)vecAt(&w->c->scopes, i);
        for (size_t j = 0; j < sc->syms.len; j++)
            if (*(Sym **)vecAt(&sc->syms, j) == s) return true;
    }
    return false;
}

/**
 * @brief Tell whether a capture is written in the body.
 *
 * @param w  Walk state.
 * @param s  Binding to check.
 * @return   True when it is in the write set.
 */
static bool lamWritten(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->written.len; i++)
        if (*(Sym **)vecAt(&w->written, i) == s) return true;
    return false;
}

/**
 * @brief Return the root binding of an assignment target: `x[3].f = ...` writes `x`.
 *
 * @param t  Assignment target expression.
 * @return   The innermost node reached through subscripts, fields, and dereferences; the current node
 *           when the walk cannot go further.
 */
static Expr *lamRootOf(Expr *t) {
    while (t) {
        if (t->kind == EX_INDEX)      t = t->u.index.obj;
        else if (t->kind == EX_FIELD) t = t->u.field.obj;
        else if (t->kind == EX_DEREF) t = t->u.deref.operand;
        else break;
    }
    return t;
}

static bool lamWalkExpr(void *ctx, Expr *e);
static bool lamWalkStmt(void *ctx, Stmt *s);

/**
 * @brief Expression visitor of the capture scan: records outer bindings that are referenced and `new`
 *        those expressions use.
 *
 * @param ctx  Walk state `LamWalk`.
 * @param e    Expression; NULL passes straight through.
 * @return     Always true, so the walk continues.
 */
static bool lamWalkExpr(void *ctx, Expr *e) {
    LamWalk *w = (LamWalk *)ctx;
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        if (sy && lamIsOuter(w, sy)) lamAddUnique(&w->caps, sy);
    } else if (e->kind == EX_NEW) {
        w->usedNew = true;
    }
    AstVisit v = { lamWalkExpr, lamWalkStmt, w };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement visitor of the capture scan: records `return` and assignments whose root is an outer
 *        binding.
 *
 * @param ctx  Walk state `LamWalk`.
 * @param s    Statement; NULL passes straight through.
 * @return     Always true, so the walk continues.
 */
static bool lamWalkStmt(void *ctx, Stmt *s) {
    LamWalk *w = (LamWalk *)ctx;
    if (!s) return true;
    if (s->kind == ST_RETURN) w->sawReturn = true;
    if (s->kind == ST_ASSIGN) {
        Sym *sy = identBindOf(lamRootOf(s->u.assign.target));
        if (sy && lamIsOuter(w, sy)) lamAddUnique(&w->written, sy);
    }
    AstVisit v = { lamWalkExpr, lamWalkStmt, w };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Tell whether a name is already written in the lambda's capture list.
 *
 * @param lam   Lambda expression node.
 * @param name  Name to look for.
 * @return      True when the capture list contains that name.
 */
static bool lamCapListed(Expr *lam, const char *name) {
    for (size_t i = 0; i < lam->u.lambda.captures.len; i++)
        if (strcmp((*(LamCap **)vecAt(&lam->u.lambda.captures, i))->name, name) == 0) return true;
    return false;
}

/* Step 4: rewrite capture names into fields of the environment. */
typedef struct { Checker *c; LamWalk *w; Vec *fields; } LamRw;   /* fields: const char*, per cap */
static bool lamRwExpr(void *ctx, Expr *e);
static bool lamRwStmt(void *ctx, Stmt *s);

/**
 * @brief Expression visitor of the capture rewrite: replaces a capture name with the C name of the
 *        environment field.
 *
 * @param ctx  Rewrite state `LamRw`.
 * @param e    Expression; NULL passes straight through.
 * @return     Always true, so the walk continues.
 */
static bool lamRwExpr(void *ctx, Expr *e) {
    LamRw *r = (LamRw *)ctx;
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        for (size_t i = 0; i < r->w->caps.len; i++) {
            if (*(Sym **)vecAt(&r->w->caps, i) != sy) continue;
            const char *fn = *(const char **)vecAt(r->fields, i);
            /* A writable capture stores a reference: `(*self->c0)` is an lvalue, so reading and writing
             * are both correct, and the node's type never changes. */
            planSetCName(e, arenaPrintf(r->c->arena,
                                        lamWritten(r->w, sy) ? "(*self->%s)" : "self->%s", fn), RKIND_EXPR);
            break;
        }
    }
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkExprChildren(e, &v);
}
/**
 * @brief Statement visitor of the capture rewrite; walks the children recursively.
 *
 * @param ctx  Rewrite state `LamRw`.
 * @param s    Statement; NULL passes straight through.
 * @return     Always true, so the walk continues.
 */
static bool lamRwStmt(void *ctx, Stmt *s) {
    LamRw *r = (LamRw *)ctx;
    if (!s) return true;
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkStmtChildren(s, &v);
}

static Type *checkExprInner(Checker *c, Expr *e);   /* the struct-literal path is re-entered below */

/**
 * @brief Check a literal lambda, synthesizing its environment struct and `call` method when needed.
 *
 * Order: build the environment type first (its fields are filled in later, because the capture set is
 * only known once the body has been checked), then build the `call` method carrying `self`; then check
 * the body in place inside the parameter scope, read the capture set off the resolved bindings, verify
 * that a writable capture is written `[mut ref x]`, and finally generate one field per capture (by value
 * or by mutable reference) and rewrite the names in the body into fields.
 *
 * @param c  Checker.
 * @param e  The `EX_LAMBDA` node.
 * @return   The synthesized environment struct type; the error type after a diagnostic for a nested
 *           lambda, a body containing `new`, an inconsistent capture set, or a missing result type.
 */
static Type *checkLambda(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    /* A lambda at a call site is checked twice: once while the argument list is being checked and
     * once more when the callee's signature (possibly instantiated) is applied to it. The
     * environment's type is created by the first check, so the second one has to return that same
     * type -- a fresh `$lam$` environment would be a structurally identical but *different* type, and
     * an argument that is a bare lambda would then compare unequal to the parameter it was inferred
     * from. Inside a generic body the environment may carry a type parameter in a field; the type is
     * still one AST node, and code generation lays it out once per instance (`planLamScope`), so the
     * same memoised type is the right answer there too. */
    if (e->type && e->u.lambda.sdef && !ttIsError(e->type))
        return e->type;

    if (c->lamDepth > 0) {
        ckError(c, e->line,
                "A lambda inside a lambda would have to lay its environment out inside the outer"
                " one; version 1 refuses that instead of laying it out wrongly.",
                "a lambda inside a lambda is not supported yet");
        return ttError(tt);
    }
    c->lamDepth++;

    /* The environment's type exists before the body is checked, because the body is checked as the
     * `call` method of it: the method's `self` is a `ref` to this very type. Its fields are filled in
     * after the body (that is where the capture set comes from) -- layout is read later, by codegen,
     * so an empty field list now is not a problem. */
    const char *owner = (c->curFunc && c->curFunc->name) ? c->curFunc->name : "top";
    const char *lname = arenaPrintf(c->arena, "$lam$%s$%d", owner, c->lamSeq++);
    StructDef *sd = arenaAllocZero(c->arena, sizeof *sd);
    sd->name = lname;
    sd->srcName = lname;
    sd->ctx = c->ctx;
    sd->modName = c->curFunc ? c->curFunc->modName : NULL;
    sd->line = e->line;
    planSetLamScope(sd, c->curFunc);
    vecInit(&sd->typeParams, c->arena, sizeof(const char *));
    vecInit(&sd->fields, c->arena, sizeof(FieldDef *));
    vecInit(&sd->methods, c->arena, sizeof(FuncDef *));
    Type *lt = arenaAllocZero(c->arena, sizeof *lt);
    lt->kind = TY_STRUCT;
    lt->name = lname;
    lt->sdef = sd;
    vecInit(&lt->targs, c->arena, sizeof(Type *));

    /* The `call` method exists **before** the body is checked: a `return` inside the lambda belongs to
     * this lambda, and the checker reads the return type from `curFunc`. With no `->` written, `ret`
     * stays NULL while the body is checked and is filled in by the first `return` afterwards -- the same
     * freedom a named function has in the grammar. */
    FuncDef *cf = arenaAllocZero(c->arena, sizeof *cf);
    cf->name = "call";
    cf->modName = sd->modName;
    cf->ctx = c->ctx;
    cf->line = e->line;
    cf->ret = e->u.lambda.ret;
    vecInit(&cf->params, c->arena, sizeof(Param *));
    {
        Param *self = arenaAllocZero(c->arena, sizeof *self);
        self->name = self->cname = "self";
        self->line = e->line;
        Type *st = arenaAllocZero(c->arena, sizeof *st);
        st->kind = TY_REF;
        st->inner = lt;
        st->mut = false;                     /* `call` reads the environment */
        self->type = st;
        *(Param **)vecPush(&cf->params) = self;
    }
    for (size_t i = 0; i < e->u.lambda.params.len; i++)
        *(Param **)vecPush(&cf->params) = *(Param **)vecAt(&e->u.lambda.params, i);
    /* A declared function's signature is resolved before its body is checked; a synthesized one
     * goes through the same step, so its parameter types are resolved rather than left as the
     * parser's raw nodes. */
    resolveSignature(c, cf);
    {   /* the signature the user wrote, for diagnostics: `fn(i64) -> i64` */
        Buf sig;
        bufInit(&sig, c->arena);
        bufPuts(&sig, "fn(");
        for (size_t i = 0; i < e->u.lambda.params.len; i++) {
            Param *p = *(Param **)vecAt(&e->u.lambda.params, i);
            bufPrintf(&sig, "%s%s", i ? ", " : "", typeStr(c, p->type));
        }
        bufPrintf(&sig, ") -> %s", cf->ret ? typeStr(c, cf->ret) : "?");
        anSetLamSig(sd, bufCstr(&sig));
    }

    /* (1) the parameters in scope, so the body's names resolve by the ordinary rules -- shadowing
     * included, which a name-only pre-pass would get wrong. */
    const size_t nOuter = c->scopes.len;     /* below this: the enclosing functions' scopes */
    pushScope(c);
    for (size_t i = 0; i < e->u.lambda.params.len; i++) {
        Param *p = *(Param **)vecAt(&e->u.lambda.params, i);
        Sym *sy = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sy->cname;
    }
    FuncDef *saveCur = c->curFunc;
    c->curFunc = cf;
    resultsEnterOwner(cf);   /* a lambda body is its own scope */
    /* With no `->` written the result type is decided by the body's first `return <value>`, filled in by
     * the ST_RETURN branch. */
    anSetLamInferRet(cf, (cf->ret == NULL));
    checkStmt(c, e->u.lambda.body);
    anSetLamInferRet(cf, false);
    c->curFunc = saveCur;
    resultsEnterOwner(saveCur);
    popScope(c);

    if (!cf->ret) {
        ckError(c, e->line,
                "A lambda's result type has to be inferable: either write `-> T`, or give the body a"
                " `return <value>`.",
                "cannot infer the result type of this lambda");
        cf->ret = ttError(tt);
    }

    /* (2) the capture set, read off the resolved bindings (not guessed from the text) */
    LamWalk w;
    w.c = c; w.nOuter = nOuter; w.usedNew = false; w.sawReturn = false;
    vecInit(&w.caps, c->arena, sizeof(Sym *));
    vecInit(&w.written, c->arena, sizeof(Sym *));
    {
        AstVisit v = { lamWalkExpr, lamWalkStmt, &w };
        astWalkStmtChildren(e->u.lambda.body, &v);
    }
    /* The `call` method is skipped by `checkFunc` (its body was already checked above), so the
     * "does a value-returning function return?" check that a named function gets from there has to
     * happen here -- otherwise the generated C falls off the end of a non-void function. */
    if (cf->ret && !ttIs(cf->ret, "void") && !w.sawReturn)
        ckError(c, e->line,
                "The lambda declares a result type, so its body has to return one.",
                "this lambda returns no value, but its type says `%s`", typeStr(c, cf->ret));

    if (w.usedNew) {
        ckError(c, e->line,
                "A lambda runs wherever the value is called from, so which arena a `new` inside it"
                " would belong to is not decided yet. Take the place as a parameter instead.",
                "a lambda body may not allocate yet");
    }

    /* Every writable capture is written in the source -- the escape and arena walks read this too: every
     * writable alias appears in the program text (so the reader's pass can see every writable alias). */
    for (size_t i = 0; i < w.written.len; i++) {
        Sym *sy = *(Sym **)vecAt(&w.written, i);
        if (!lamCapListed(e, sy->name))
            ckError(c, e->line,
                    "A lambda that writes an enclosing variable must say so, so that every writable"
                    " alias stays visible in the source: write `[mut ref %s]` after the parameter"
                    " list.",
                    "the body writes `%s`, but the capture list does not name it", sy->name);
    }
    for (size_t i = 0; i < e->u.lambda.captures.len; i++) {
        LamCap *cap = *(LamCap **)vecAt(&e->u.lambda.captures, i);
        bool found = false;
        for (size_t j = 0; j < w.caps.len && !found; j++)
            found = strcmp((*(Sym **)vecAt(&w.caps, j))->name, cap->name) == 0;
        if (!found)
            ckError(c, cap->line, NULL,
                    "`%s` is not an enclosing variable this body uses", cap->name);
    }

    /* (3) the environment: one field per capture, in declaration order, and the value it is built
     * with at the place the lambda is written. */
    Vec fields;                              /* const char*: the generated field of caps[i] */
    vecInit(&fields, c->arena, sizeof(const char *));
    Vec inits;                               /* FieldInit* */
    vecInit(&inits, c->arena, sizeof(FieldInit *));
    for (size_t i = 0; i < w.caps.len; i++) {
        Sym *sy = *(Sym **)vecAt(&w.caps, i);
        bool wr = lamWritten(&w, sy);
        FieldDef *fd = arenaAllocZero(c->arena, sizeof *fd);
        fd->name = arenaPrintf(c->arena, "c%zu", i);   /* index-based: two captures may share a name */
        fd->line = e->line;
        if (wr) {
            Type *rt = arenaAllocZero(c->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = sy->type;
            rt->mut = true;
            fd->type = rt;
        } else {
            /* A copy: the closure sees the value it was made with. A type that mentions a type
             * parameter is fine here -- the environment is laid out once per instance of the enclosing
             * body (see `planLamScope`), so the field's C type is the instance's concrete type. */
            fd->type = sy->type;
        }
        *(FieldDef **)vecPush(&sd->fields) = fd;
        *(const char **)vecPush(&fields) = fd->name;

        Expr *id = exprNew(c->arena, EX_IDENT, e->line);
        id->u.ident.name = sy->name;
        planSetCName(id, sy->cname, RKIND_EXPR);
        id->type = sy->type;
        Expr *val = id;
        if (wr) {                            /* `mut ref x`: a reference to the variable itself */
            Expr *rf = exprNew(c->arena, EX_REF, e->line);
            rf->u.ref.operand = id;
            rf->type = fd->type;
            val = rf;
        }
        FieldInit *fi = arenaAllocZero(c->arena, sizeof *fi);
        fi->name = fd->name;
        fi->value = val;
        *(FieldInit **)vecPush(&inits) = fi;
    }

    {   /* (4) rewrite the captured names to the environment's fields */
        LamRw rw; rw.c = c; rw.w = &w; rw.fields = &fields;
        AstVisit v = { lamRwExpr, lamRwStmt, &rw };
        astWalkStmtChildren(e->u.lambda.body, &v);
    }

    cf->owner = sd;
    cf->body = e->u.lambda.body;
    anSetLamChecked(cf, true);                   /* checked above, in the scope it was written in */
    *(FuncDef **)vecPush(&sd->methods) = cf;
    /* The struct must be reachable from the module: codegen collects the struct types and methods to
     * emit from `m->structs`. */
    *(StructDef **)vecPush(&c->m->structs) = sd;

    c->lamDepth--;

    /* The value **is** the environment. It stays an EX_LAMBDA node -- codegen writes the literal out
     * from the field values stored here -- because the generated type has no name in the source for
     * the ordinary struct-literal path to look up. */
    e->u.lambda.sdef = sd;
    e->u.lambda.tname = lname;
    e->u.lambda.inits = inits;
    e->type = lt;
    return lt;
}

/**
 * @brief Return the type of a **function name used as a value**: the function's own signature, made into
 *        an `fn` type.
 *
 * The signature has to match C's exactly, so three shapes are each rejected with a reason: **generic**
 * (one body per instance, so the bare name is not a single code pointer), **method** (the first parameter
 * is the receiver, and a call through a pointer would not write it; a free function wrapping it is the
 * honest spelling), **coroutine** (its value is a frame handle, not a code pointer). A fourth rejection
 * can only be left to codegen: a function that ends up with a hidden home arena or zone parameter has one
 * more C parameter than an `fn` type has, and those two flags are only settled once every body has been
 * checked.
 *
 * Only the smallest kind of `fn` value is covered here: the address of an existing function; a
 * capture-less lambda becoming an `fn` value is not on this path.
 *
 * @param c  Checker.
 * @param e  The `EX_IDENT` node carrying the name.
 * @param f  The function the name resolved to.
 * @return   The function type, also written back onto the node; the error type after a diagnostic.
 */
static Type *fnValueOf(Checker *c, Expr *e, FuncDef *f) {
    if (f->typeParams.len > 0) {
        ckError(c, e->line,
                "Which instance? A `fn` value is one code pointer, and a generic function has one"
                " body per instantiation. Call it, or write a free function with the signature you"
                " want.",
                "`%s` is generic, so its bare name is not a value", e->u.ident.name);
        return ttError(c->tt);
    }
    if (funcIsMethod(f)) {
        ckError(c, e->line,
                "A method's first parameter is its receiver, which a call through a pointer would"
                " have to write out. Write a free function that forwards to it.",
                "`%s` is a method, not a function, so its name is not a `fn` value", e->u.ident.name);
        return ttError(c->tt);
    }
    if (planIsCoro(f)) {
        ckError(c, e->line,
                "A coroutine's value is a frame handle that some task drives, not a code pointer.",
                "`%s` is a coroutine, so its name is not a `fn` value", e->u.ident.name);
        return ttError(c->tt);
    }

    Vec ps;
    vecInit(&ps, c->arena, sizeof(void *));
    for (size_t i = 0; i < f->params.len; i++)
        *(Type **)vecPush(&ps) = tsub(c, (*(Param **)vecAt(&f->params, i))->type);
    Type *ft = ttFn(c->tt, &ps, tsub(c, f->ret ? f->ret : ttVoid(c->tt)));

    planSetCallee(e, f);
    /* Taking the address is a use like any other: codegen emits only the functions something
     * calls or names, so without this the definition would be missing and the C compiler would
     * report an undeclared identifier. */
    planSetUsed(f, true);
    return ft;
}

/**
 * @brief Compute the type a **field access** ought to have without checking it fully.
 *
 * Why this exists instead of a `checkInto` on the callee: `o.f(x)` resolves as a method call, and before
 * the method path claims it, "is `f` a field of function type" has to be answerable cheaply; checking
 * `o.f` to answer that would re-run the receiver's check on the ordinary path, and that path is taken by
 * every method call of every program.
 *
 * @param c      Checker.
 * @param field  The `EX_FIELD` node.
 * @return       The field type, with a generic receiver already substituted; NULL when the answer cannot be
 *               had that cheaply. NULL only means "not my case", never an error: the caller falls through
 *               to the ordinary path, which reports what is wrong with the program.
 */
static Type *peekFieldType(Checker *c, Expr *field) {
    if (!field || field->kind != EX_FIELD) return NULL;
    Expr *obj = field->u.field.obj;
    if (!obj || obj->kind != EX_IDENT) return NULL;
    Sym *s = lookup(c, obj->u.ident.name);
    if (!s || !s->type) return NULL;
    Type *bt = ttBase(tsub(c, s->type));
    if (ttIsError(bt)) return NULL;
    StructDef *sd = structOf(bt);
    if (!sd) return NULL;
    FieldDef *fd = findField(sd, field->u.field.name);
    if (!fd || !fd->type) return NULL;
    /* The same substitution the field-access case does: a field type may mention the receiver's
     * type arguments. */
    return (bt->kind == TY_GENERIC)
               ? tsub(c, ttSubstitute(c->tt, fd->type, &sd->typeParams, &bt->targs))
               : tsub(c, fd->type);
}

/**
 * @brief Materialize the parameters of an `fn` type into `Param` nodes.
 *
 * Argument rules are written against a declared parameter list (per-parameter `checkAssignable`, then
 * `checkCallRefArgs` over the whole list), while a function type is a signature with no declaration.
 * Writing those rules a second time for indirect calls would let one rule become two that drift apart, so
 * this gives them the same shape. Names are left empty on purpose: a function type has no parameter names,
 * and nothing on this path prints them.
 *
 * @param c   Checker, used for allocation.
 * @param ft  The function type.
 * @return    A vector of `Param *` in one-to-one correspondence with `ft->params`.
 */
static Vec fnTypeParams(Checker *c, Type *ft) {
    Vec ps;
    vecInit(&ps, c->arena, sizeof(void *));
    for (size_t i = 0; i < ft->params.len; i++) {
        Param *p = (Param *)arenaAllocZero(c->arena, sizeof(Param));
        p->name = p->cname = "";
        p->type = *(Type **)vecAt(&ft->params, i);
        *(Param **)vecPush(&ps) = p;
    }
    return ps;
}

static bool lendViewArg (Checker *c, Type *want, Expr **pslot);

/**
 * @brief Check one argument and, when the parameter wants a view and a container was passed, lend it in
 *        place.
 *
 * This is the post-check counterpart of `lendViewArg`: the argument is checked by the ordinary rules
 * first, and only when the type does not match while the parameter is a (read-only or writable) view and
 * the argument type does have `asSlice()`/`asMutSlice()` is the argument node replaced by the lend call
 * and checked again. That makes `io::println(f"total={n}")` -- a value f-string passed directly as an
 * argument -- work, while the diagnostic for a lend that really is impossible still comes from
 * `checkAssignable` at the call site.
 *
 * @param c     Checker.
 * @param slot  Argument slot (replaced in place when lending).
 * @param want  Declared parameter type.
 * @return      The argument type obtained by checking (and, if needed, lending first).
 */
static Type *checkBorrowArg(Checker *c, Expr **slot, Type *want) {
    Expr *a = *slot;
    if (!a) return ttError(c->tt);
    adoptContextType(a, want);
    Type *at = checkInto(c, want, a);
    if (!want || !at || ttIsError(at)) return at ? at : ttError(c->tt);
    Type *wb = ttBase(want);
    if (!wb || !ttIsViewType(wb)) return at;
    if (ttEquals(want, at) || ttCanWiden(at, want)) return at;
    if (!lendViewArg(c, want, slot)) return at;
    /* The new node has not been checked yet; the inner argument is already checked and will not report a
     * second time (the unused warning would repeat it, and item (1) removes it once the builtin
     * print/println are desugared). */
    return checkInto(c, want, *slot);
}
/**
 * @brief Check one call argument, treating a **by-value** parameter as the new owner of a
 *        `@noCopy` name.
 *
 * A by-value parameter owns what it is given and runs its `@drop` hook on the way out, exactly
 * like `var w = v` does for a local. Passing a `@noCopy` value by value is therefore a transfer,
 * not a copy: the state gets a single name, which is the whole point of the annotation. The
 * permission is granted only for a by-value parameter -- a `ref` / `mut ref` argument lends the
 * object and keeps the old rule.
 *
 * @param c     Checker.
 * @param slot  Argument slot; may be rewritten in place by the view-lending step.
 * @param want  The parameter's declared type.
 * @return      The argument's type.
 */
static Type *checkArgBorrow(Checker *c, Expr **slot, Type *want) {
    bool savedMove = c->moveOkHere;
    const char *savedInto = c->moveIntoName;
    if (want && want->kind != TY_REF) {
        c->moveOkHere = true;
        c->moveIntoName = "the callee";
    }
    Type *at = checkBorrowArg(c, slot, want);
    c->moveOkHere = savedMove;
    c->moveIntoName = savedInto;
    return at;
}



/**
 * @brief The name of the method that lends a container as a view: `view` (read-only) or `viewMut`
 *        (writable), with the older `asSlice` / `asMutSlice` spelling still accepted.
 *
 * @param base     The container type (already stripped of `ref`).
 * @param wantMut  True for the writable view.
 * @return         The declared method's name, or NULL when the type declares neither spelling.
 */
static const char *feeldLendName(Type *base, bool wantMut) {
    if (!base || !structOf(base)) return NULL;
    const char *primary = wantMut ? "viewMut" : "view";
    const char *legacy = wantMut ? "asMutSlice" : "asSlice";
    if (findMethod(base, primary)) return primary;
    if (findMethod(base, legacy)) return legacy;
    return NULL;
}

/**
 * @brief Lend a "container" argument in place as a read-only view: `varArray<T>` is written `v.asSlice()`,
 *        a fixed-size array is written `a[..]`.
 *
 * A read-only view parameter (`slice<T>`, not `mut`) accepts a container argument of the same element,
 * and the `.asSlice()` / `[..]` spelling is supplied here: **a real method call / slice node is built and
 * checked again normally**, so borrowing and lifetime bookkeeping work exactly as if the user had written
 * it, rather than as an implicit conversion that skips the check. A writable view (`mut slice<T>`) is not
 * included: that is asking for write permission and has to be written out.
 *
 * @param c      Checker.
 * @param want   Parameter type.
 * @param pslot  Argument slot; replaced by the lend node (already checked) on a hit.
 * @return       True when it matched and replaced; false otherwise (the caller then checks the original
 *               argument as usual).
 */
static bool lendViewArg(Checker *c, Type *want, Expr **pslot) {
    if (!c || !want || !pslot || !*pslot) return false;
    Type *wb = ttBase(want);
    if (!wb || ttIsError(wb) || !ttIsViewType(wb)) return false;
    /* A read-only view lends `asSlice` / `a[..]`; a writable view lends `asMutSlice` / `a[..]` (the view the
     * latter produces depends on the array's writability). A writable lend requires the container itself to
     * be a writable place -- the borrow checker has the final say further on. */
    bool wantMut = want->mut;
    Type *wantElem = viewElemOf(wb);
    if (!wantElem) return false;
    Expr *a = *pslot;
    /* The argument has not been checked at this position yet (`checkInto` comes later), so only the most
     * common shape whose type can be looked up directly is recognized: an identifier. Other expressions go
     * through the ordinary check, and the diagnostic says what should be written. */
    Type *got = a->type;
    if (!got && a->kind == EX_IDENT && a->u.ident.name) {
        Sym *sym = lookup(c, a->u.ident.name);
        if (sym) got = sym->type;
    }
    if (!got || ttIsError(got) || got->kind == TY_REF) return false;
    Expr *node = NULL;
    /* The element type may carry type parameters (`slice<T>` of `sum<T>(v: slice<T>)`): after lending,
     * inference reads `T` from the type of the lend node, so it is enough here that they can match. */
    bool elemOk = wantElem && ttHasParam(wantElem);
    if (got->kind == TY_ARRAY && got->inner &&
        (elemOk || ttEquals(got->inner, wantElem)) && isPlace(a)) {
        /* `a[..]`: endpoints omitted, and the checker fills in the fixed-size array's length as a literal. */
        node = exprSliceNew(c->arena, a, NULL, NULL, a->line);
    } else {
        /* Any type can declare its own lend method: `view() -> slice<T>` (read-only) and
         * `viewMut() -> mut slice<T>` (writable); the older `asSlice()` / `asMutSlice()` spelling is still
         * accepted. The criterion is "the method really exists and returns a view whose element matches" --
         * the naming convention is not the contract, the signature is. This keeps user-written containers
         * out of the checker. */
        const char *name = feeldLendName(ttBase(got), wantMut);
        FuncDef *borrow = name ? findMethod(ttBase(got), name) : NULL;
        if (borrow && borrow->ret) {
            Type *rb = borrow->ret;
            /* When the method is declared on the template, the return type has `T` in it: substitute with the
             * type arguments of the receiver's instance. */
            if (got->kind == TY_GENERIC && borrow->owner == got->sdef)
                rb = ttSubstitute(c->tt, rb, &borrow->owner->typeParams, &got->targs);
            Type *rbBase = ttBase(rb);
            Type *be = rbBase && ttIsViewType(rbBase) ? viewElemOf(rbBase) : NULL;
            if (be && (elemOk || ttEquals(be, wantElem)) && (!wantMut || rb->mut))
                node = exprMethodNew(c->arena, a, name, a->line);
        }
    }
    if (!node) return false;
    *pslot = node;
    return true;
}

/**
 * @brief The `fn(...)` type a `.call(...)` receiver holds, without checking the receiver.
 *
 * Only the shapes whose type is known without walking them: a name (its binding's type) and a field
 * access (the field's declared type). Anything else returns NULL and takes the ordinary method path,
 * which reports the usual diagnostic.
 *
 * @param c  The checker.
 * @param e  The receiver expression.
 * @return   Its `fn` type, or NULL.
 */
static Type *peekCallableType(Checker *c, Expr *e) {
    if (!e) return NULL;
    Type *t = NULL;
    if (e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        t = sy ? sy->type : NULL;
    } else if (e->kind == EX_FIELD) {
        t = peekFieldType(c, e);
    }
    if (!t) return NULL;
    t = tsub(c, t);
    return (t && ttBase(t) && ttBase(t)->kind == TY_FN) ? ttBase(t) : NULL;
}

/**
 * @brief Check `f(x)` where `f` **holds a value of function type** rather than a declaration.
 *
 * The signature is the value's own, so every argument is matched against that type.
 *
 * @param c   Checker.
 * @param e   Call node `EX_CALL`.
 * @param ft  The callee's function type.
 * @return    The return type the signature carries; a diagnostic is reported when the argument count or
 *            type does not match.
 *
 * The callee expression is checked, not looked up: codegen prints it as written, so its binding (`cname`)
 * has to be frozen by the checker like any other identifier. `checkCallRefArgs` deliberately receives a
 * **NULL** callee: a code pointer carries no `effects` signature (a signature is a property of a
 * declaration, and this value has no declaration), so the honest summary is the conservative one -- "it
 * may store anything given to it"; that is the same default an `extern!` without a clause gets.
 */
static Type *checkCallThroughFn(Checker *c, Expr *e, Type *ft) {
    checkInto(c, NULL, e->u.call.callee);      /* freezes the callee's C name, so codegen's printer can use it */

    Vec ps = fnTypeParams(c, ft);
    if (e->u.call.args.len != ps.len) {
        ckError(c, e->line, NULL, "this `fn` value expects %zu argument(s), got %zu",
                ps.len, e->u.call.args.len);
        return ft->ret;
    }
    for (size_t i = 0; i < ps.len; i++) {
        Param *p = *(Param **)vecAt(&ps, i);
        Expr **slot = (Expr **)vecAt(&e->u.call.args, i);
        Type *at = checkArgBorrow(c, slot, p->type);
        Expr  *a = *slot;

        /* The same spelling rule as an ordinary call: passing a reference to a `ref T` parameter is
         * written `ref ...` at the site, so a reader sees a reference and not a copy. */
        if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
            ckError(c, a->line,
                    "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                    "`ref` is a mutable reference, so the target must be a `var`",
                    "argument expects `%s`; write `ref ...` here to pass a reference",
                    typeStr(c, p->type));
            continue;
        }
        checkAssignable(c, p->type, at, a, "argument");
    }
    /* Whose signature does this call follow? The **type** of an `fn` value carries no `effects` -- a clause
     * is written on a declaration, and a function type is not a declaration. What can carry it is the
     * **slot** the pointer comes from: a field of function type can be written `effects Addr=0 Cont=0`, and
     * that field is exactly the callee of this call. A `fn` copied into a local loses the signature, and
     * that is the most honest direction to lose it in.
     *
     * The mask follows the **same** rule a signed `extern!` follows: `checkCallRefArgs` only needs a
     * synthesized declaration, so the two cannot drift apart.
     *
     * A slot with no clause gets the worst case, as an unsigned `extern!` does: every argument may be
     * retained forever (`collectEffects` fills the same all-ones mask for that tier). That is what makes
     * "the signature is a promise" mean something at the call site. */
    FuncDef *sig = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    sig->isExtern    = true;        /* "a black box whose signature is the whole basis" */
    anSetEffComplete(sig, true);
    {
        FieldDef *fd = (e->u.call.callee && e->u.call.callee->kind == EX_FIELD)
                           ? planField(e->u.call.callee) : NULL;
        if (fd && fd->hasEffects) {
            sig->name     = fd->name;
            sig->hasEffects = true;
            anSetAddrMask(sig, fd->effAddrMask);
            anSetContMask(sig, fd->effContMask);
        } else {
            /* Named after the slot when there is one: "argument 1 of `copy` may be kept by C
             * forever" points at the line to sign. The message wraps this in backticks, so it has to
             * be a bare word. */
            sig->name = fd ? fd->name : "fn";
            for (size_t i = 0; i < ps.len && i < 32; i++) {
                anSetAddrMask(sig, anAddrMask(sig) | (1u << i));
                anSetContMask(sig, anContMask(sig) | (1u << i));
            }
        }
    }
    checkCallRefArgs(c, sig, &e->u.call.args, &ps, 0, e->line, sig->name);

    planSetCallViaFn(e, true);
    return ft->ret;
}

/**
 * @brief Check a format specifier in an f-string slot (the reserved `__extc_pad` call the parser desugars
 *        out).
 *
 * A specifier only says in the source how to lay things out, but which pad primitive is used depends on
 * the slot value's **static type**: integers go to `padInt`, floats to `padF`, `slice<u8>` to `padText`.
 * That choice can only be made here -- only here is the value's type known together with the width/fill/
 * alignment/precision that were written; the emitting side follows the choice recorded in the plan.
 *
 * @param c  Checker.
 * @param e  The reserved-name EX_METHOD; arguments are, in order, the slot value, width, fill byte,
 *           alignment, precision, type code.
 * @return   The output stream's type; the error type when the arguments are inconsistent or the value type
 *           does not support a specifier (diagnostic already reported).
 */
static Type *checkPadCall(Checker *c, Expr *e) {
    if (e->u.method.args.len != 6) return ttError(c->tt);   /* built by the parser itself; cannot happen */
    Type *rt = checkExpr(c, e->u.method.recv);
    if (!rt || rt->kind == TY_ERROR) return ttError(c->tt);
    Type *rb = ttBase(rt);
    /* Only an output stream can carry a specifier: `padText` is a method on the stream, so it is the gate for
     * "is this a stream" */
    if (!findMethod(rb, "padText")) {
        ckError(c, e->line,
                "A format specifier writes through a stream: put the f-string on the right of `<<`,"
                " as in `io::cout << f\"{x:>8}\"`.",
                "`%s` cannot take a format specifier", typeStr(c, rt));
        return ttError(c->tt);
    }
    Type *vt = checkExpr(c, *(Expr **)vecAt(&e->u.method.args, 0));
    if (!vt || vt->kind == TY_ERROR) return ttError(c->tt);
    const char *padName = NULL;
    PlanPadKind kind = PLAN_PAD_NONE;
    if (ttIsInteger(vt)) { padName = "padInt"; kind = PLAN_PAD_INT; }
    else if (ttIsFloat(vt)) { padName = "padF"; kind = PLAN_PAD_FLOAT; }
    else if (ttIsViewType(vt) && vt->targs.len == 1 &&
             ttIs(*(Type **)vecAt(&vt->targs, 0), "u8")) {
        padName = "padText"; kind = PLAN_PAD_TEXT;
    }
    if (!padName) {
        ckError(c, e->line,
                "A format specifier picks the integer, floating-point or text padder from the"
                " value's type; anything else goes through `<<`.",
                "`%s` cannot take a format specifier", typeStr(c, vt));
        return ttError(c->tt);
    }
    /* The written type code has to agree with the value's type: `{x:d}` says this is an integer, so it has to
     * really be one */
    const long long code = (*(Expr **)vecAt(&e->u.method.args, 5))->u.ival;
    const long long prec = (*(Expr **)vecAt(&e->u.method.args, 4))->u.ival;
    if (code >= 0) {
        const long long want = kind == PLAN_PAD_INT ? 0 : kind == PLAN_PAD_FLOAT ? 1 : 2;
        if (code != want) {
            const char wc = code == 0 ? 'd' : code == 1 ? 'f' : 's';
            ckError(c, e->line,
                    "The type code says how the value is written, so it has to match what the"
                    " value is.",
                    "`%c` says the value is %s, but it is %s", (int)wc,
                    code == 0 ? "an integer" : code == 1 ? "a float" : "text",
                    kind == PLAN_PAD_INT ? "an integer" : kind == PLAN_PAD_FLOAT ? "a float" : "text");
            return ttError(c->tt);
        }
    }
    if (prec >= 0 && kind != PLAN_PAD_FLOAT) {
        ckError(c, e->line,
                "Precision (`.N`) applies to floats, and this value is not one.",
                "a precision (`.N`) applies to floats, and `%s` is not one", typeStr(c, vt));
        return ttError(c->tt);
    }
    /* Width/fill/alignment/precision are literals built by the parser, and they have to go through here too
     * (later passes read their types) */
    for (size_t i = 1; i < e->u.method.args.len; i++)
        (void)checkExpr(c, *(Expr **)vecAt(&e->u.method.args, i));
    FuncDef *f = findMethod(rb, padName);
    if (!f) return ttError(c->tt);
    /* Record that this method was called: instance methods are emitted only for the instances that call them
     * (the pad primitives are utility methods, so without collecting it the definition would be missing) */
    planSetUsed(f, true);
    planSetPadKind(e, kind);
    e->type = f->ret;
    return f->ret;
}

/**
 * @brief Check a slot carrying a specifier in a value f-string (the reserved `__extc_fstr_pad` call the
 *        parser desugars out).
 *
 * A value position wants the pad function that **returns a string** (the stream's `padInt`/`padF`/
 * `padText` write to an fd and cannot be used here). Which one it is likewise depends on the slot value's
 * static type: integer -> `stl::stringio::padI64`, float -> `padF64`, `slice<u8>` -> `padText`. The node is
 * rewritten in place into an `EX_ASSOC` and then goes down the ordinary free-function call path, so module
 * import checking and the plan-side bookkeeping need not be repeated.
 *
 * @param c  Checker.
 * @param e  The reserved-name `EX_CALL`; arguments are, in order, the slot value, width, fill byte,
 *           alignment, precision, type code.
 * @return   The result type (`string`); the error type when the value type does not support a specifier or
 *           the specifier contradicts itself.
 */
static Type *checkFstrPadCall(Checker *c, Expr *e) {
    if (e->u.call.args.len != 6) return ttError(c->tt);   /* built by the parser itself; cannot happen */
    Type *vt = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 0));
    if (!vt || vt->kind == TY_ERROR) return ttError(c->tt);
    const char *name = NULL;
    PlanPadKind kind = PLAN_PAD_NONE;
    if (ttIsInteger(vt)) { name = "padI64"; kind = PLAN_PAD_INT; }
    else if (ttIsFloat(vt)) { name = "padF64"; kind = PLAN_PAD_FLOAT; }
    else if (ttIsViewType(vt) && vt->targs.len == 1 &&
             ttIs(*(Type **)vecAt(&vt->targs, 0), "u8")) {
        name = "padText"; kind = PLAN_PAD_TEXT;
    }
    if (!name) {
        ckError(c, e->line,
                "A format specifier picks the integer, floating-point or text padder from the"
                " value's type; anything else has to be converted first.",
                "`%s` cannot take a format specifier", typeStr(c, vt));
        return ttError(c->tt);
    }
    /* The default direction when no alignment is written: text on the left, numbers on the right -- the same
     * rule the `<<` path applies in codegen (`genPadCall` turns -1 into 0/1); here the checker turns it into a
     * literal so the library's pad* functions do not each have to guess. */
    {
        Expr *alignE = *(Expr **)vecAt(&e->u.call.args, 3);
        if (alignE->kind == EX_INT && alignE->u.ival < 0) {
            Expr *lit = exprIntNew(c->arena, kind == PLAN_PAD_TEXT ? 0 : 1, alignE->line);
            lit->type = c->tI64;
            *(Expr **)vecAt(&e->u.call.args, 3) = lit;
        }
    }
    const long long code = (*(Expr **)vecAt(&e->u.call.args, 5))->u.ival;
    const long long prec = (*(Expr **)vecAt(&e->u.call.args, 4))->u.ival;
    if (code >= 0) {
        const long long want = kind == PLAN_PAD_INT ? 0 : kind == PLAN_PAD_FLOAT ? 1 : 2;
        if (code != want) {
            const char wc = code == 0 ? 'd' : code == 1 ? 'f' : 's';
            ckError(c, e->line,
                    "The type code says how the value is written, so it has to match what the"
                    " value is.",
                    "`%c` says the value is %s, but it is %s", (int)wc,
                    code == 0 ? "an integer" : code == 1 ? "a float" : "text",
                    kind == PLAN_PAD_INT ? "an integer" : kind == PLAN_PAD_FLOAT ? "a float" : "text");
            return ttError(c->tt);
        }
    }
    if (prec >= 0 && kind != PLAN_PAD_FLOAT) {
        ckError(c, e->line,
                "Precision (`.N`) applies to floats, and this value is not one.",
                "a precision (`.N`) applies to floats, and `%s` is not one", typeStr(c, vt));
        return ttError(c->tt);
    }
    for (size_t i = 1; i < e->u.call.args.len; i++)
        (void)checkExpr(c, *(Expr **)vecAt(&e->u.call.args, i));
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.call.args, 0);
    *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.call.args, 1);   /* width */
    if (kind == PLAN_PAD_FLOAT) {
        *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.call.args, 4);  /* precision (absent => 6) */
        if (prec < 0) {
            Expr *six = exprIntNew(c->arena, 6, e->line);
            six->type = c->tI64;
            *(Expr **)vecAt(&args, args.len - 1) = six;
        }
    }
    *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.call.args, 2);   /* pad byte */
    *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.call.args, 3);   /* alignment */
    /* The node the parser built has the reserved name as its callee; replace it with the assembled flat name
     * (`stringio$padI64`) and hand it back to the ordinary free-function call path -- argument checking,
     * arena/zone bookkeeping, plan marking, and C emission all take that one existing route. When the module
     * has not been imported the merged module has no such name, and an actionable hint is given here. */
    char *flat = arenaPrintf(c->arena, "string$%s", name);
    if (!findFunc(c, flat)) {
        ckError(c, e->line,
                "A value f-string's format specifier needs `std::text::string`, which is missing from"
                " this standard library.",
                "the sugar imports `std::text::string` on its own, so a miss here means the standard"
                " library is incomplete, not that a `use` line is missing");
        return ttError(c->tt);
    }
    e->u.call.args = args;
    Expr *id = exprIdent(c->arena, flat, e->line);
    id->u.ident.srcName = name;
    e->u.call.callee = id;
    /* The name has been "qualified" by the checker (the source did not write `stringio::`), so the module
     * prefix must not be demanded again. */
    anSetQualified(e, true);
    return checkExprInner(c, e);
}

/**
 * @brief When two/three-subscript sugar applies to an `ndarray`, lend a view argument automatically.
 *
 * `v[i, j]` is unconditionally desugared by the parser into `nd::$at2(v, i, j)`; the parameters of
 * `at2`/`set2`/`slice2` are `ndview<T>`, and `ndarray<T>` is not a view (it holds the data) -- passing it
 * directly reports "cannot infer type parameter(s)" already during inference. So before the arguments are
 * checked, an `ndarray` argument is rewritten into `v.view()` / `v.viewMut()` (the latter for writing
 * functions); writability is left to the borrow checker, so a `let` ndarray still rejects `viewMut()`.
 *
 * @param c  Checker.
 * @param e  Call node being checked (`EX_CALL`).
 */
static void sugarNdViewArg(Checker *c, Expr *e) {
    if (!e->u.call.callee || e->u.call.callee->kind != EX_IDENT || e->u.call.args.len == 0) return;
    const char *name = e->u.call.callee->u.ident.name;
    const char *how = NULL;
    if (strcmp(name, "nd$set2") == 0 || strcmp(name, "nd$set3") == 0) how = "viewMut";
    else if (strcmp(name, "nd$at2") == 0 || strcmp(name, "nd$at3") == 0 ||
             strcmp(name, "nd$slice2") == 0) how = "view";
    if (!how) return;
    Expr **slot = (Expr **)vecAt(&e->u.call.args, 0);
    Expr *a = *slot;
    Type *t = a->type;
    if (!t && a->kind == EX_IDENT) {
        Sym *sy = lookup(c, a->u.ident.name);
        if (sy) t = sy->type;
    }
    if (!t) return;
    Type *b = ttBase(t);
    const char *nm = b ? b->name : NULL;
    if (!nm && b && b->sdef) nm = b->sdef->name;
    /* Only `ndarray` is recognized: `ndview` is itself a view and needs no lend. */
    if (!nm || !strstr(nm, "ndarray")) return;
    *slot = exprMethodNew(c->arena, a, how, e->line);
}

/**
 * @brief How many arguments a call has to supply: the parameters ahead of the first default.
 *
 * A call may leave out a suffix of the parameter list, and only a suffix: the declaration check
 * rejects a parameter without a default that follows one with a default, so the count below is
 * both the minimum and the boundary between "written" and "filled in".
 *
 * @param f      Callee.
 * @param first  Index of the first parameter an argument corresponds to (1 for a method, whose
 *               parameter 0 is the receiver).
 * @return       Minimum number of arguments the call must write.
 */
static size_t minArgsFor(FuncDef *f, size_t first) {
    size_t total = f->params.len - first;
    for (size_t i = 0; i < total; i++)
        if ((*(Param **)vecAt(&f->params, i + first))->dflt) return i;
    return total;
}

/**
 * @brief Tell whether a call's callee name is one of the print builtins.
 *
 * The builtin spellings are bare (`print` / `println`); the module-qualified spellings
 * (`io::print`, or the mangled `io$print` a qualified name can arrive as) mean the same thing.
 *
 * @param name  Callee name as written; may carry a module part.
 * @return      `true` when this call is the print builtin.
 */
/**
 * @brief Fill the arguments a call leaves out from the callee's default values.
 *
 * The defaults are cloned per call site: the plan side table is addressed by node pointer, so two
 * call sites sharing one node would share one set of facts. Cloning is safe precisely because a
 * default is a compile-time constant — it carries no facts worth sharing.
 *
 * @param c      Checker.
 * @param args   Argument list to extend (the call node's own vector).
 * @param f      Callee.
 * @param first  Index of the first parameter an argument corresponds to.
 */
static void fillDefaultArgs(Checker *c, Vec *args, FuncDef *f, size_t first) {
    size_t total = f->params.len - first;
    while (args->len < total) {
        Param *p = *(Param **)vecAt(&f->params, args->len + first);
        *(Expr **)vecPush(args) = exprClone(c->arena, p->dflt);
    }
}

/**
 * @brief Tell whether a call's callee name is one of the print builtins.
 *
 * The builtin spellings are bare (`print` / `println`); the module-qualified spellings
 * (`io::print`, or the mangled `io$print` a qualified name can arrive as) mean the same thing.
 * Comparing the last component keeps the two spellings one construct.
 *
 * @param name  Callee name as written; may carry a module part.
 * @return      `true` when this call is the print builtin.
 */
static bool isPrintBuiltinName(const char *name) {
    if (!name) return false;
    const char *shortName = name;
    const char *sep = strstr(name, "::");
    if (sep && sep[2]) {
        shortName = sep + 2;
    } else {
        const char *dollar = strrchr(name, '$');
        if (dollar && dollar[1]) shortName = dollar + 1;
    }
    return strcmp(shortName, "print") == 0 || strcmp(shortName, "println") == 0;
}

/**
 * @brief Check an expression and return its type: the recursive core of the checker.
 *
 * `checkExpr` wraps it with the bookkeeping every subexpression needs. Some nodes are rewritten **in place**
 * during checking, so callers must not count on `e->kind` staying what it was on entry.
 *
 * @param c  Checker.
 * @param e  Expression node; must not be NULL.
 * @return   The expression type; the error type after a diagnostic.
 */

static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:
            /* A literal's own type is the smallest integer that holds it: `i32` when it fits, otherwise
             * `i64`, so an unannotated `let n = 5000000000` inside a generic body is still 64-bit. Only
             * lossless widening is implicit, never implicit truncation; a context that wants a different
             * integer type gives the literal its type through `literalFits` / `adoptContextType`. */
            if (e->u.ival < -2147483648LL || e->u.ival > 2147483647LL) return c->tI64;
            return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_NULL: {
            /* The type was parked on `e->type` by `adoptContextType`. If the context did
             * not say which `?ref T` this stands for, report it rather than guess: extC is
             * explicit instead of inferring, and the context's silence is not an answer. */
            Type *nt = e->type;
            if (nt && nt->kind == TY_REF && nt->nullable) return nt;
            ckError(c, e->line,
                    "`null` is the zero value of a nullable reference, so its type must be"
                    " clear from context -- e.g. `var p: ?ref node = null`, or a parameter"
                    " or return type of `?ref T`",
                    "`null` here does not know which `?ref T` it is");
            return ttError(tt);
        }

        case EX_IDENT: {
            Sym *s = lookup(c, e->u.ident.name);
            if (s && s->modName && !anQualified(e))
                requireQualified(c, e->u.ident.name, s->modName, false, e->line);
            if (!s) {
                /* A **function name used as a value**: `var f: fn(i64) -> i64 = double_it`.
                 *
                 * `lookup` finds bindings -- locals, parameters, globals -- and a function is not one,
                 * so this is where a function name arrives: at the point the language would otherwise
                 * say "undefined name". The expression's type is the function's **own signature**,
                 * built here; no context is consulted, exactly as every other expression has a type
                 * of its own (an annotation on the left of `=` then checks against it as usual).
                 *
                 * Three kinds of function cannot be a `fn` value, and each is refused with its own
                 * reason rather than being allowed to produce C that does not compile; see
                 * `fnValueOf`. */
                FuncDef *fv = findFunc(c, e->u.ident.name);
                if (fv) return fnValueOf(c, e, fv);
                /* A bare variant name, with no payload. After `type st = | ok | bad`,
                 * writing `let s: st = ok` would otherwise report `undefined name `ok``,
                 * which does not show that the qualified form `st.ok` is wanted. A variant
                 * is named by its type, so this gets the same guidance as a bare payload
                 * constructor does. */
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, e->u.ident.name)) owner = td;
                }
                if (owner) {
                    const char *note = arenaPrintf(c->arena,
                            "A variant is named by its type -- write `%s.%s`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), e->u.ident.name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a value -- did you mean `%s.%s`?",
                            e->u.ident.name, DN(owner), DN(owner), e->u.ident.name);
                    return ttError(tt);
                }
                /* `[N]T` makes `N` usable in the body: it is a value supplied by the caller, so the
                 * template records which parameter it is and each instance gets the number. */
                if (c->curFunc && c->curFunc->constParams.len > 0) {
                    for (size_t ci = 0; ci < c->curFunc->constParams.len; ci++) {
                        if (strcmp(*(const char **)vecAt(&c->curFunc->constParams, ci),
                                   e->u.ident.name) != 0) continue;
                        for (size_t pi = 0; pi < c->curFunc->typeParams.len; pi++) {
                            if (strcmp(*(const char **)vecAt(&c->curFunc->typeParams, pi),
                                       e->u.ident.name) != 0) continue;
                            planSetLenParam(e, (int)pi);
                            e->type = c->tI64;
                            return c->tI64;
                        }
                    }
                }
                ckError(c, e->line, "every name must be declared first: locals, parameters, globals,"
                                " and imported module members all count",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* Name resolution is frozen here: codegen prints `cname` directly. This is
             * what tells shadowed names apart (`a` from `a__2`). */
            planSetCName(e, s->cname, RKIND_EXPR);
            /* A moved `@noCopy` binding has given its value away; reading it here would read the
             * bits the destination now owns. Copyable values are copied instead of moved, so this
             * only ever fires for `@noCopy`. */
            if (s->droppedLine) {
                ckError(c, e->line,
                        "an explicit `drop` runs the hook now: the place it names holds nothing, so"
                        " assign a new value before reading it",
                        "`%s` was dropped here and cannot be read", e->u.ident.name);
                return ttError(tt);
            }
            if (s->movedLine) {
                ckError(c, e->line,
                        "a `@noCopy` value has one owner: after the move the old name holds"
                        " nothing. Assign a new value to it, or pass `ref` so both names see the"
                        " same state",
                        "`%s` was moved into `%s` and cannot be read here",
                        e->u.ident.name, s->movedInto ? s->movedInto : "another binding");
                return ttError(tt);
            }
            /* Pin the resolved binding itself onto the node. The solving pass at the end
             * of the run happens after every function body has been checked, when the
             * scopes are already popped, so looking the name up again would either fail or
             * find a different binding that happens to share the name. See `IdentBinding`. */
            {
                IdentBinding *ib = (IdentBinding *)arenaAllocZero(c->arena, sizeof(IdentBinding));
                ib->sym = s;
                e->u.ident.sym = ib;
            }
            /* An `if p != null` test already proved this, so hand out the non-null
             * reference. Then `p.field`, `p.method()`, and passing it to a `ref T`
             * parameter all follow, and not one runtime check has to be generated. */
            Type *sty = s->type;
            if (sty && sty->kind == TY_REF && sty->nullable && isNarrowed(c, s->cname)) {
                Type *nn = ttRef(tt, sty->inner);
                nn->mut = sty->mut;
                return nn;
            }
            return sty;
        }

        case EX_BIN: {
            const char *op0 = e->u.bin.op;

            /* The short circuit of `&&` / `||` is a narrowing point too:
             * `p != null && p.value > 3` may dereference on the right, because reaching
             * the right side means the left side held. */
            if (isLogicOp(op0)) {
                Type *lt = checkValue(c, e->u.bin.left);
                expectBool(c, lt, e->u.bin.left);

                bool whenTrue = false;
                const char *tg = narrowTarget(c, e->u.bin.left, &whenTrue);
                const size_t mark = c->narrow.len;
                if (strcmp(op0, "&&") == 0) {
                    /* Reaching the right side means the left side was true, so every
                     * fact the left side proves holds here. */
                    narrowFactsOf(c, e->u.bin.left);
                } else if (tg && !whenTrue) {
                    /* `a || b`: reaching the right side means the left side was false.
                     * Only the simplest shape, `p == null`, yields "p is not null" when
                     * negated; the full negation of `||` would have to negate a
                     * conjunction, which is not implemented. */
                    pushNarrow(c, tg);
                }

                Type *rt = checkValue(c, e->u.bin.right);
                expectBool(c, rt, e->u.bin.right);
                c->narrow.len = mark;
                return c->tBool;
            }

            /* `p == null` / `p != null`: comparing against null is the only thing a
             * nullable reference is for. This has to run before the `checkValue` calls
             * below, because `null` does not know its own type. */
            if ((strcmp(op0, "==") == 0 || strcmp(op0, "!=") == 0) &&
                (e->u.bin.left->kind == EX_NULL || e->u.bin.right->kind == EX_NULL)) {
                Expr *nv = (e->u.bin.left->kind == EX_NULL) ? e->u.bin.left : e->u.bin.right;
                Expr *ov = (nv == e->u.bin.left) ? e->u.bin.right : e->u.bin.left;
                Type *ot = checkExpr(c, ov);
                if (ttIsError(ot)) return c->tBool;
                if (ot->kind == TY_REF && ot->nullable) {
                    adoptContextType(nv, ot);
                    checkExpr(c, nv);
                    return c->tBool;
                }
                if (ot->kind == TY_REF) {
                    ckError(c, e->line,
                            "only a nullable reference (`?ref T`) can be null -- a plain"
                            " `ref T` is guaranteed non-null, that is what it means",
                            "`%s` is not nullable, so it can never be `null`", typeStr(c, ot));
                    return c->tBool;
                }
                ckError(c, e->line,
                        "`null` is the zero value of a nullable reference, so compare it"
                        " with one: `if p != null { ... }`",
                        "`null` cannot be compared with `%s`", typeStr(c, ot));
                return c->tBool;
            }

            /* `<<` and `>>` treat the left operand as a **receiver**, not a value: in `fin >> x >> line` the
             * first `>>` returns `mut ref fin` and the second continues from the same object, so the
             * value-position rule "a reference is not a value, write `*p`" must not fire on this operand.
             * Other operators behave as before. */
            bool shiftRecv = op0[0] == '<' || op0[0] == '>';
            Type *lt = shiftRecv ? checkExpr(c, e->u.bin.left) : checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

            /* `any` next to an operand of a concrete type means "this must hold that type": the
             * unbox is attached to the operand here, so the operator itself stays an ordinary one
             * and a wrong belief traps at this line instead of silently reinterpreting bytes. */
            if (lt && lt->kind == TY_ANY && rt && rt->kind != TY_ANY) {
                planSetAnyTo(e->u.bin.left, rt);
                lt = rt;
            } else if (rt && rt->kind == TY_ANY && lt && lt->kind != TY_ANY) {
                planSetAnyTo(e->u.bin.right, lt);
                rt = lt;
            }

            if (isCmpOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return c->tBool;

                bool isEqOp = isEqualityOp(op);

                /* `==` / `!=`: builtins compare natively; a struct goes through its
                 * `==` method. The right operand does not have to be the same type as
                 * the left one: like every other overloadable operator, `==` is matched
                 * on the right operand's type exactly, so `vec2 == i64` is a method of
                 * its own and no conversion is invented for it. */
                if (isEqOp) {
                    bool same = ttEquals(lt, rt);
                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);

                    if (same && cmpIsNative(lt)) return c->tBool;

                    /* Two `any` values compare by walking the descriptor at run time: the contents'
                     * type is not known here, so the comparison is the generic one and no method is
                     * involved. Comparing an `any` with a concrete value is deliberately not this
                     * case -- unbox first, so a type mismatch traps at the line that assumed it. */
                    if (same && b && b->kind == TY_ANY) return c->tBool;

                    /* Arrays: `==` is derived by the compiler. The user cannot write an
                     * array type down, so it cannot be given an `==` method. */
                    if (same && lt->kind == TY_ARRAY) {
                        if (!typeSupportsOp(tt, lt->inner, op, lt->inner))
                            ckError(c, e->line,
                                    "An array's `==` is derived by the compiler, "
                                    "so its elements have to be comparable.",
                                    "`%s` cannot be compared: its element type `%s` does not define `==`",
                                    typeStr(c, lt), typeStr(c, ttBase(lt)->inner));
                        return c->tBool;
                    }

                    /* A type parameter: defer the check until the instance is known.
                     * Do not gate the record on `c->curFunc->owner`: that would cover
                     * methods only. A generic free function has no owner, so the
                     * `T: ==` requirement would never be checked. Record the use and
                     * ask once per instance whether this `T` has `==`. */
                    if (same && lt->kind == TY_PARAM) {
                        deferOp(c, e, op);
                        return c->tBool;
                    }

                    if (!sd) {
                        /* Different types with nothing to compare them: the rules below
                         * report it, and they are the ones that know how a numeric
                         * literal adapts to the other side. */
                        if (!same) goto notEq;
                        /* An enum with a payload is the type most likely to hit this: in
                         * C it is a struct, and C structs cannot be compared with `==`, so
                         * the message points the user at `match`. */
                        bool ep = b && b->kind == TY_ENUM && enumHasPayload(b->edef);
                        ckError(c, e->line,
                                ep ? "an enum with a payload is a tagged union -- "
                                     "compare it with `match`, or write a method that does"
                                   : NULL,
                                ep ? "`%s` is an enum with a payload, so it has no `%s`"
                                   : "`%s` does not support `%s`", typeStr(c, lt), op);
                        return c->tBool;
                    }

                    FuncDef *m = findOp(tt, b, op, rt, strcmp(op, "!=") == 0 ? "==" : NULL);
                    if (!m) m = findOpLiteral(c, b, op, strcmp(op, "!=") == 0 ? "==" : NULL, e->u.bin.right);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn ==(self: ref %s, other: %s) -> bool { ... }",
                                  DN(sd), DN(sd), typeStr(c, rt));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==` for `%s`, so it cannot be compared",
                                DN(sd), typeStr(c, rt));
                        return c->tBool;
                    }

                    planSetCallee(e, m);  planSetUsed(m, true);
                    ckNoteInstUse(m, lt);
                    setOpCallArgs(c, e, m);
                    return c->tBool;
                }
            notEq:

                /* Every other comparison is meaningful for numbers only. */
                if (ttIsNumeric(lt) && ttIsNumeric(rt) &&
                    (ttCanWiden(lt, rt) || ttCanWiden(rt, lt))) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;

                /* Ordering on a user type: the same protocol as `==` -- a method whose
                 * name is the operator. Nothing is derived from anything else here:
                 * `!=` is exactly the negation of `==` and so falls back, but `>` does
                 * not come from `<`. A derived answer would have to be explained in
                 * every diagnostic, and it would silently accept a type that defined
                 * only one of the pair. */
                StructDef *osd = structOf(ttBase(lt));
                if (osd) {
                    FuncDef *m = findOp(tt, ttBase(lt), op, rt, NULL);
                    if (!m) m = findOpLiteral(c, ttBase(lt), op, NULL, e->u.bin.right);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn %s(self: ref %s, other: %s) -> bool { ... }",
                                  DN(osd), op, DN(osd), typeStr(c, rt));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `%s` for `%s`, so it cannot be ordered",
                                DN(osd), op, typeStr(c, rt));
                        return c->tBool;
                    }
                    planSetCallee(e, m);
                    planSetUsed(m, true);
                    setOpCallArgs(c, e, m);
                    return c->tBool;
                }

                /* A comparison whose operand is a type parameter: on the template `T` is
                 * opaque, so the answer depends on the instance. Record it and let the
                 * per-instance pass ask -- the same record and the same consumer that
                 * `==` uses. */
                if (lt->kind == TY_PARAM || rt->kind == TY_PARAM) {
                    deferOp(c, e, op);
                    return c->tBool;
                }
                ckError(c, e->line, isEqOp ? "only numbers, `bool`, enums, and structs that define `==` can be compared" : NULL,
                        "cannot compare `%s` with `%s`", typeStr(c, lt), typeStr(c, rt));
                return c->tBool;
            }
            if (isBitOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return ttError(tt);
                /* A **reference receiver** is allowed only for stream operators, and only when its base type
                 * is a struct defining that operator: this is what makes `fin >> x >> line` work -- the first
                 * `>>` returns `mut ref ifstream` and the second continues from the same object. Everything
                 * else still reports "a reference appeared where a number belongs", which is the useful
                 * sentence. */
                bool streamRecv = (op[0] == '<' || op[0] == '>') &&
                                  isRef(lt) && structOf(ttBase(lt)) != NULL;
                if (!streamRecv && (isRef(lt) || isRef(rt)))
                    return refNotANumber(c, e, lt, rt, op);
                /* `<<` and `>>` are shifts for integers and stream operators for a type
                 * that defines them. Which one applies is decided by the *static type of
                 * the left operand*, so this is a branch rather than overload resolution:
                 * a builtin integer shifts (rule three: builtins are never overridden),
                 * a struct with such a method calls it. */
                if (op[0] == '<' || op[0] == '>') {
                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (sd) {
                        FuncDef *m = findOp(c->tt, b, op, rt, NULL);
                        if (!m) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note,
                                      "define it inside `%s`:\n"
                                      "      fn %s(self: ref %s, other: %s) -> %s { ... }",
                                      DN(sd), op, DN(sd), typeStr(c, rt), DN(sd));
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` does not define `%s` for `%s`",
                                    DN(sd), op, typeStr(c, rt));
                            return ttError(tt);
                        }
                        Vec *sp = NULL, *sa = NULL;
                        if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                        planSetCallee(e, m);
                        planSetUsed(m, true);
                        ckNoteInstUse(m, lt);
                        setOpCallArgs(c, e, m);
                        return m->ret ? ttSubstitute(tt, m->ret, sp, sa) : ttVoid(tt);
                    }
                }
                if (!ttIsInteger(lt) || !ttIsInteger(rt)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `%s` to `%s` and `%s`",
                            op, typeStr(c, lt), typeStr(c, rt));
                    return ttError(tt);
                }
            }
            /* The same trap as in the bitwise case: `ttIsNumeric` strips `ref T` down to
             * `T`, so `p + 1` for `p: ref i64` would pass as arithmetic while the generated
             * C performs pointer arithmetic -- silently doing something else entirely. */
            if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
            /* A shift amount that is a **literal** and exceeds the left operand's width is reported at
             * compile time. At run time `extc_shiftCount` traps (`1 << 32` on an i32 is one such case), but
             * this is already visible in the source: writing `i64(1) << 32` or using another count is better
             * than finding out when execution reaches it. */
            if ((strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) &&
                e->u.bin.right->kind == EX_INT && ttIsInteger(lt)) {
                int bits = builtinIntBits(ttBase(lt));
                long long cnt = e->u.bin.right->u.ival;
                if (bits > 0 && (cnt < 0 || cnt >= bits)) {
                    ckError(c, e->line,
                            "The shift count has to stay below the left operand's width (the"
                            " generated C checks it at run time and traps otherwise). Widen the"
                            " left operand -- write `i64(1) << 32` instead of `1 << 32` -- or"
                            " use a count below the width.",
                            "shift count %lld does not fit in `%s` (width %d)",
                            cnt, typeStr(c, lt), bits);
                    return ttError(tt);
                }
            }
            /* The result type comes from the arithmetic rule, literal fitting included;
             * no second copy of that rule here. */
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkValue(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
            /* `~` is meaningful for integers only. */
            if (strcmp(e->u.un.op, "~") == 0) {
                if (!ttIsInteger(ot)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `~` to `%s`", typeStr(c, ot));
                    return ttError(tt);
                }
                return ot;
            }
            if (!ttIsNumeric(ot)) {
                ckError(c, e->line, NULL, "cannot negate `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            return ot;
        }

        case EX_FIELD: {
            /* First decide whether this names an enum variant, as in `Status.warn`, where
             * `Status` is a type name rather than a variable. */
            if (e->u.field.obj->kind == EX_IDENT) {
                const char *tn   = e->u.field.obj->u.ident.name;
                const char *vn   = e->u.field.name;
                if (!lookup(c, tn)) {
                    Type *et = ttFromName(tt, tn);
                    if (et && et->kind == TY_ENUM) {
                        Variant *v = findVariant(et->edef, vn);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, vn);
                            return ttError(tt);
                        }
                        /* Rewrite into an enum value node; codegen consumes it directly. */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        planSetAssocOwner(e, et);      /* the resolved type; instances need it */
                        return et;
                    }
                }
            }

            Type *rawT = checkExpr(c, e->u.field.obj);
            if (rejectNullableDeref(c, rawT, e->u.field.obj, "field access")) return ttError(tt);
            Type *bt = ttBase(rawT);
            StructDef *sd = structOf(bt);
            if (!sd) {
                ckError(c, e->line, NULL, "`%s` is not a struct, so it has no field `%s`",
                        typeStr(c, bt ? bt : e->type), e->u.field.name);
                return ttError(tt);
            }
            FieldDef *fd = findField(sd, e->u.field.name);
            if (!fd) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "fields of %s:", DN(sd));
                for (size_t i = 0; i < sd->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", DN(sd), e->u.field.name);
                return ttError(tt);
            }
            /* `@private` storage belongs to the module that declares it. This is the rule that
             * keeps a container's raw storage out of reach: `s.buf[0..n]` from another module
             * would be a view into pool storage, and a view that can be stored is the hole the
             * pool tier closes by never handing one out. */
            {
                const char *home = c->curFunc ? c->curFunc->modName : NULL;
                if (fd->isPrivate && sd->modName != home) {
                    ckError(c, e->line,
                            "Storage is private to the module that declares it; use the type's"
                            " own methods, which decide what may be lent out and for how long.",
                            "field `%s` of `%s` is private to module `%s`",
                            fd->name, DN(sd), sd->modName ? sd->modName : "this file");
                    return ttError(tt);
                }
            }
            planSetField(e, fd);
            /* A field type may mention type parameters; substitute the receiver's type
             * arguments for them. */
            Type *ftype = (bt->kind == TY_GENERIC)
                          ? ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs)
                          : fd->type;
            /* Path narrowing: inside `if h.p != null { ... }` a read of `h.p` yields the
             * non-null version. `narrowTarget` records the fact only when the root is a
             * local whose address was never taken, which is exactly the condition relied on
             * here. This mirrors the `EX_IDENT` rule (`isNarrowed(c, s->cname)`) above. */
            if (ftype && ftype->kind == TY_REF && ftype->nullable &&
                e->u.field.obj->kind == EX_IDENT && planCName(e->u.field.obj)) {
                const char *rc = planCName(e->u.field.obj);
                size_t kn = strlen(rc) + strlen(e->u.field.name) + 2;
                char *key = (char *)arenaAlloc(c->arena, kn);
                snprintf(key, kn, "%s.%s", rc, e->u.field.name);
                if (isNarrowed(c, key)) {
                    Type *nn = ttRef(tt, ftype->inner);
                    nn->mut = ftype->mut;
                    return nn;
                }
            }
            return ftype;
        }

        case EX_INDEX: {
            Type *ot = checkExpr(c, e->u.index.obj);

            /* A type that defines `[]` gets subscript syntax: `m[k]` becomes the method call it always was,
             * and the whole method machinery -- argument checking, return type, deferred checking of generic
             * instances -- applies as is, with no second copy here. The built-in array / slice / view paths
             * below remain for every other type. `[]` and `[]=` are separate names: only the read side is
             * here; assigning through a type that has only `[]` is reported by the assignment path (writes go
             * through `[]=`). */
            {
                FuncDef *ix = ttIsError(ot) ? NULL : findMethod(ttBase(ot), "[]");
                if (ix) {
                    Vec args;
                    vecInit(&args, c->arena, sizeof(void *));
                    *(Expr **)vecPush(&args) = e->u.index.index;
                    Expr *recv = e->u.index.obj;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "[]";
                    e->u.method.args = args;
                    return checkExpr(c, e);
                }
            }

            Type *it = checkValue(c, e->u.index.index);
            if (ttIsError(ot)) return ttError(tt);

            /* The built-in subscript path dereferences the object, so a nullable reference must first have a
             * non-null proof -- same as field access, `*`, and method receivers (they all call this). Without
             * it, `var p: ?ref [4]i32 = null` followed by `p[1]` would compile with no diagnostic and the
             * generated C would read `(*p).data[1]`. The `[]` method path above has its own check at the
             * receiver, so this one is deliberately placed after it. */
            if (rejectNullableDeref(c, ot, e->u.index.obj, "a subscript")) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, NULL,
                        "cannot index a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            /* Error recovery for the subscript: its failure was already reported where it came from, and
             * `self.tag[i]` is still u8 no matter what went wrong with `i`. Propagating the error here would
             * poison every later use of the element, and a poisoned operand makes later checks return early
             * -- their deferred records would never be written, so codegen would emit the wrong operator. */
            if (ttIsError(it)) return elem;
            if (!ttIsInteger(it)) {
                ckError(c, e->line, "An index must be an integer.",
                        "index must be an integer, found `%s`", typeStr(c, it));
                return ttError(tt);
            }

            /* A fixed array has a compile-time length, so a literal index that is out of
             * range is reported here instead of at runtime: what the compiler can prove, it
             * has to say. The rule matches the three slice bounds checked below, except that
             * a single half-open bound applies -- legal indices are [0, n). Both `b[-1]` and
             * `b[4]` for a `[4]T` are compile errors. */
            if (ob->kind == TY_ARRAY) {
                long long iv = 0;
                if (asIntLit(e->u.index.index, &iv)) {
                    long long n = (long long)ob->asize;
                    if (iv < 0 || iv >= n) {
                        ckError(c, e->line,
                                "The compiler can prove this index is out of range.",
                                "index %lld is not inside `%s` (length %lld)",
                                iv, typeStr(c, ot), n);
                        return ttError(tt);
                    }
                }
            }
            return elem;
        }

        case EX_SLICE: {
            /* `a[lo..hi]` is a view. Either bound may be absent: `lo` when the front is
             * omitted, `hi` when the back is. */
            Type *ot = checkExpr(c, e->u.slice.obj);
            if (ttIsError(ot)) return ttError(tt);

            /* Same rule as the subscript above: slicing a `?ref` view/array takes its address
             * without asking whether it is null (the second half). */
            if (rejectNullableDeref(c, ot, e->u.slice.obj, "a slice")) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, "Only a fixed array or a view can be sliced.",
                        "cannot slice a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }

            /* Slicing a fixed array takes the address of an element, so the base has to be
             * a place. Slicing a view is only pointer arithmetic on a value, which needs no
             * storage -- that is why a string literal can be sliced. */
            if (ob->kind == TY_ARRAY && !isPlace(e->u.slice.obj)) {
                ckError(c, e->line, "Slicing takes the address of an element, so the base must be a place.",
                        "cannot slice a temporary value; `%s` needs a variable, a field or an index",
                        typeStr(c, ot));
                return ttError(tt);
            }

            /* With a fixed-array base the length is a compile-time constant, so an omitted
             * bound is filled in as a literal:
             *     a[2..] -> a[2..15]      a[..] -> a[0..15]
             * Codegen then sees two literal bounds and can emit code with no checks. */
            if (ob->kind == TY_ARRAY) {
                if (!e->u.slice.lo) e->u.slice.lo = intLit(c, 0, e->line);
                if (!e->u.slice.hi) e->u.slice.hi = intLit(c, ob->asize, e->line);
            }

            for (int k = 0; k < 2; k++) {
                Expr *b = k == 0 ? e->u.slice.lo : e->u.slice.hi;
                if (!b) continue;
                Type *bt = checkValue(c, b);
                if (!ttIsError(bt) && !ttIsInteger(bt))
                    ckError(c, b->line, "A slice bound must be an integer.",
                            "slice bound must be an integer, found `%s`", typeStr(c, bt));
            }

            /* An error the compiler can prove is reported here; none of it is left to
             * runtime. Each bound is examined on its own: an out-of-range literal is wrong
             * whatever the other bound says. */
            if (ob->kind == TY_ARRAY) {
                long long n = (long long)ob->asize;
                Expr *lp = e->u.slice.lo, *hp = e->u.slice.hi;
                long long lv = 0, hv = 0;
                bool lOk = asIntLit(lp, &lv);
                bool hOk = asIntLit(hp, &hv);

                /* Each bound is examined on its own: an out-of-range literal is wrong
                 * whatever the other bound says. */
                if (hOk && (hv > n || hv < 0)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice end %lld is not inside `%s` (length %lld)",
                            hv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && (lv < 0 || lv > n)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice start %lld is not inside `%s` (length %lld)",
                            lv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && hOk && hv < lv) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice `%lld..%lld` ends before it starts", lv, hv);
                    return ttError(tt);
                }
            }
            /* A view inherits writability from what it was sliced out of:
             *   `var a` / a `mut ref` parameter          ->  `mut slice<T>`
             *   `let a` / a string literal / a read-only view  ->  `slice<T>`
             * That is how one view type expresses both permissions, without the two slice
             * types Rust needs. */
            return ttViewMut(tt, sliceOf(c, elem),
                             isWritablePlace(c, e->u.slice.obj));
        }

        case EX_ARRAYLIT: {
            /* The type comes from the context (`var a: [3]i32 = [...]`) or is taken from
             * the elements. */
            Type *want = (e->type && e->type->kind == TY_ARRAY) ? e->type : NULL;
            Type *elemT = NULL;

            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                Expr *el = *(Expr **)vecAt(&e->u.arraylit.elems, i);
                if (want) adoptContextType(el, want->inner);
                Type *t = checkValue(c, el);
                if (ttIsError(t)) continue;

                if (!elemT) {
                    elemT = want ? want->inner : t;
                }
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "array element %zu", i);
                checkAssignable(c, elemT, t, el, bufCstr(&what));
            }

            if (!elemT && want) elemT = want->inner;
            if (!elemT || ttIsError(elemT)) {
                ckError(c, e->line,
                        "An empty array literal needs the length and element type from context: "
                        "`var a: [3]i32 = [...]`.",
                        "cannot infer the element type of this array literal");
                return ttError(tt);
            }

            int64_t n = (int64_t)e->u.arraylit.elems.len;
            if (e->u.arraylit.rest) {
                if (!want) {
                    ckError(c, e->line,
                            "`...` fills the rest with zero values, so the length has to "
                            "come from the type.",
                            "`...` needs the array length from context");
                    return ttError(tt);
                }
                n = want->asize;
                if (n < (int64_t)e->u.arraylit.elems.len)
                    ckError(c, e->line, NULL,
                            "too many elements: `%s` holds %lld",
                            typeStr(c, want), (long long)want->asize);
            } else if (want && n != want->asize) {
                ckError(c, e->line,
                        "An array literal must give every element. Use a trailing `...` "
                        "to fill the rest with zero values.",
                        "`%s` needs %lld element(s), got %lld",
                        typeStr(c, want), (long long)want->asize, (long long)n);
                return ttError(tt);
            }

            Type *arr = ttArray(tt, n, elemT);
            if (want && !ttEquals(want, arr))
                ckError(c, e->line, NULL, "array literal does not match `%s`", typeStr(c, want));
            return arr;
        }

        case EX_REF: {
            Expr *op = e->u.ref.operand;
            Type *ot = checkExpr(c, op);
            /* Taking an address records that this binding's address has gone out. An alias
             * may exist, so a write through the binding is not necessarily the only write to
             * that storage.
             * The narrowing facts recorded for paths through it are dropped at the same
             * time: with an alias possible, `h.p` being non-null cannot be relied on. */
            { Sym *rs = placeRoot(c, op); if (rs) { unNarrow(c, rs->cname); rs->addressed = true; } }
            if (ttIsError(ot)) return ttError(tt);

            if (ot->kind == TY_REF) {
                ckError(c, e->line, "it is already a reference -- just pass it as is",
                        "`ref` applied to a value that is already a reference");
                return ot;
            }
            if (!isLvalue(c, op)) {
                /* An element of a container-owned view is a value, not a place: growing the
                 * container resizes or replaces the plate behind the view. Saying so is more
                 * useful than repeating the general rule; a view into storage this frame owns
                 * (`new T[n]`) is a place and never reaches this branch. */
                if (op->kind == EX_INDEX)
                    ckError(c, e->line,
                            "an element of a view has no stable address unless the storage is "
                            "allocated in this frame (`new T[n]`); a container may grow and move "
                            "the storage behind the view",
                            "cannot take a reference to an element of this view");
                else
                    ckError(c, e->line,
                            "only variables, fields, fixed-size array elements and elements of a "
                            "frame-allocated view can be referenced",
                            "cannot take a reference to this expression");
                return ttError(tt);
            }
            /* Which reference comes out depends on whether the place is writable:
             *   a `var` variable / a `mut ref` parameter  ->  `mut ref T`
             *   a `let` variable / a `ref` parameter      ->  `ref T` (read-only)
             *
             * A read-only borrow may always be taken: that is the whole reason borrows
             * exist. While `ref` meant mutable only, taking a reference to a `let` was
             * rejected outright, so even a plain read could not be written. */
            Type *r = ttRef(tt, ot);
            r->mut = isWritablePlace(c, op);
            return r;
        }

        case EX_TRY:
            /* `?` forwards an error at statement level: it expands into "evaluate once,
             * test, return". C has no statement expressions, so it is legal in exactly three
             * positions:
             *     let x = e?   /   x = e?   /   return e?
             * Anywhere else, say `f(e? + 1)`, it is reported clearly instead of producing C
             * that does not compile. */
            ckError(c, e->line,
                    "`?` expands into statements (evaluate once, test, return), so it needs a "
                    "position that is evaluated exactly once and unconditionally.",
                    "`?` cannot appear here: this position may be skipped (the right side of "
                    "`&&` / `||` / `??`, a loop condition, a lambda body). Bind it first: "
                    "`let v = e?`");
            return ttError(tt);

        case EX_ASSOC: {            /* `option<i64>::some(x)`. An associated function is a
                                     * function written inside a `struct` without `self`.
                                     * The type arguments are written out in full rather than
                                     * inferred from the arguments or the context: extC is
                                     * explicit instead of inferring. */
            Type *raw = typeNamed(c->arena, e->u.assoc.typeName);
            raw->targs = e->u.assoc.targs;
            Type *t = ttResolve(tt, c->ctx, raw, e->line, c->curParams);
            if (ttIsError(t)) return ttError(tt);
            planSetAssocOwner(e, t);

            /* Constructing an enum variant goes through this path as well:
             * `option<i64>::some(3)` and `maybe<i64>::nothing`. So `::` has two readings for
             * an enum but one meaning only -- build a value -- and the prelude and existing
             * code keep writing it exactly as they do today. */
            StructDef *esd = structOf(t);
            if (!esd && t->kind == TY_ENUM && t->edef) {
                Variant *v = findVariant(t->edef, e->u.assoc.name);
                if (!v) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "variants of %s:", t->name);
                    for (size_t i = 0; i < t->edef->variants.len; i++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&t->edef->variants, i))->name);
                    ckError(c, e->line, bufCstr(&note),
                            "`%s` has no variant `%s`", t->name, e->u.assoc.name);
                    return ttError(tt);
                }
                Vec evargs = e->u.assoc.args;      /* union: copy it out first */
                e->kind = EX_ENUMVAL;
                e->u.enumval.typeName = t->name;   /* instance name, e.g. `maybe_i64` */
                e->u.enumval.variant  = v->name;
                e->u.enumval.args     = evargs;
                return checkExprInner(c, e);
            }

            StructDef *sd = structOf(t);
            FuncDef *f = NULL;
            if (sd) {
                for (size_t i = 0; i < sd->methods.len; i++) {
                    FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                    if (anIsAssoc(m) && strcmp(m->name, e->u.assoc.name) == 0) { f = m; break; }
                }
            }
            if (!f) {
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "associated functions of %s:", DN(sd));
                    bool any = false;
                    for (size_t i = 0; i < sd->methods.len; i++) {
                        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                        if (!anIsAssoc(m)) continue;
                        bufPrintf(&note, " %s", m->name);
                        any = true;
                    }
                    if (!any) bufPuts(&note, " (none)");
                    bufPuts(&note, "; a function written inside a `struct` without"
                                  " `self` is an associated function");
                } else {
                    bufPuts(&note, "only struct types have associated functions");
                }
                ckError(c, e->line, bufCstr(&note), "no associated function `%s` on `%s`",
                        e->u.assoc.name, e->u.assoc.typeName);
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* code generation keys off this flag, not e->func (NULL here) */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* Signature: `(id: i64, lo: i64, hi: i64, views...)`, where **exactly one** `mut` view is the
                 * output (partitioned over [lo,hi)) and the rest are **read-only** views (every worker gets the
                 * same one, no partitioning, hence no new sendability analysis). */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                /* View arguments are aligned one by one: a `mut` parameter must receive a `mut` argument (it
                 * gets written); a read-only parameter accepts both (using a mutable thing read-only is
                 * safe). */
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Expr *an = *(Expr **)vecAt(&e->u.call.args, 1 + nViews);
                    Expr *ax = *(Expr **)vecAt(&e->u.call.args, 2 + nViews);
                    Type *tn = checkExpr(c, an);
                    Type *tx = checkExpr(c, ax);
                    /* A bare integer literal is an i64 in this position (the argument has no
                     * other context to say otherwise), so `parallel::run(w, out, n, 2)` does not
                     * need the `i64(...)` wrapper. Anything else still has to be i64. */
                    if (!(isI64Type(tn) || (an->kind == EX_INT && literalFits(an, c->tI64))) ||
                        !(isI64Type(tx) || (ax->kind == EX_INT && literalFits(ax, c->tI64)))) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);   /* code generation emits the trampoline based on this flag */
                planSetParTlsArena(wf, true);   /* `new` inside the worker goes to the thread-local arena */
                planSetUsed(wf, true);          /* the trampoline names it, so it has to be emitted */
                {
                    /* The first argument is not an expression but a function name: it is replaced by an i64
                     * literal, and the following arguments proceed as usual. */
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);
            /* Methods and associated functions are recorded per instance: a generic struct's method bodies
             * are shared by all instances, but they should be re-checked and emitted only for the instances
             * that really use them (`slice<u8> ==` must not force `slice<map::bnode<...>>` to define `!=`). */
            if (e->kind == EX_METHOD) ckNoteInstUse(f, e->u.method.recv->type);
            else if (t) ckNoteInstUse(f, t);

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* An associated function has to compute its arena arguments too: it may allocate, and it may
             * return a reference; without this step it is a call with too few arguments and the generated C
             * does not compile. */
            anSetHomeDepth(e, callHomeDepth(c, &e->u.assoc.args, &f->params, e));
            setCallArenaArg(c, e);      /* fix the arena this call ends up passing */
            setCallZoneArg(c, e);       /* pool-creating call: the layer the pool is allocated in */
            /* The reference check is moved to the end of this case, so it runs after all arguments are
             * checked. */

            {
                size_t need = minArgsFor(f, 0);
                if (e->u.assoc.args.len < need || e->u.assoc.args.len > f->params.len) {
                    if (need == f->params.len)
                        ckError(c, e->line, NULL, "`%s::%s` expects %zu argument(s), got %zu",
                                e->u.assoc.typeName, f->name, f->params.len,
                                e->u.assoc.args.len);
                    else if (need == 0)
                        ckError(c, e->line, NULL,
                                "`%s::%s` expects at most %zu argument(s), got %zu",
                                e->u.assoc.typeName, f->name, f->params.len,
                                e->u.assoc.args.len);
                    else
                        ckError(c, e->line, NULL,
                                "`%s::%s` expects %zu to %zu argument(s), got %zu",
                                e->u.assoc.typeName, f->name, need, f->params.len,
                                e->u.assoc.args.len);
                    return ttError(tt);
                }
                fillDefaultArgs(c, &e->u.assoc.args, f, 0);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                Expr **slot = (Expr **)vecAt(&e->u.assoc.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);
                Type *at = checkArgBorrow(c, slot, pt);
                Expr  *a = *slot;
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. ",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* `coroutine<T>` is a uniform handle, a plain 24-byte value, so any place
                 * that takes a value takes it. */
                checkAssignable(c, pt, at, a, "argument");
                /* The same `@value` copy rule as an ordinary call: an associated function is a
                 * call too, so a place handed to a by-value parameter is cloned. */
                if (ckValueCopyNeedsClone(c, pt, a)) planSetArgClone(a, true);
                ckOwningArgMoveOnly(c, pt, a, "argument");
            }
            /* The reference check applies to an associated function as well, once its
             * arguments have been checked. */
            checkCallRefArgs(c, f, &e->u.assoc.args, &f->params, anHomeDepth(e),
                             e->line, e->u.assoc.name);
            return f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);
        }

        case EX_CONV: {
            /* `i32(x)`: an explicit conversion.
             *   - Widening, which is already implicit, may still be written out and generates
             *     no check at all.
             *   - Narrowing and sign changes trap when the value does not fit, with the
             *     source position in the message.
             *   - Integer to float and back follows C (truncation, round to nearest), with a
             *     range check on the float-to-integer direction.
             * A conversion the compiler can prove fits, such as `i32(u8 value)`, generates no
             * check either. */
            /* The one conversion that is not about numbers: a **code pointer**.
             *   `fn(i64) -> i64(p)`   C's `void *` becomes callable
             *   `ref void(f)`         a code pointer becomes an opaque address again
             * It is narrow on purpose. The only source type is `ref void` -- a pointer with **no
             * operations at all** (no `*`, no `[i]`, no `.field`), so the only thing it could ever
             * become is a call target, and the type written out *is* the author's signature, exactly
             * as `effects` is. A general pointer cast (`ref void` -> `ref point`) is deliberately not
             * here: that is the point where a type system stops being one.
             *
             * The null rule is the language's own: `fn` has no null value, so a **nullable** `?ref
             * void` (what `dlsym` returns when it fails) cannot become a `fn` without proving it is
             * not null first -- `p!` is how the author signs for that, and it is checked elsewhere. */
            /* `typeName` is the marker, not `type`: the numeric path below **writes** `type`, and a generic
             * body is checked once per instance -- writing back to `type` would let this branch claim the
             * numeric conversion on the second visit and report "cannot convert `i32` to `i64`". The parser
             * sets exactly one of the two: `i32(x)` sets the name, `fn(...) -> R (x)` sets the resolved
             * type. */
            if (!e->u.conv.typeName && e->u.conv.type) {
                Type *want = ttResolve(tt, c->ctx, e->u.conv.type, e->line, c->curParams);
                e->u.conv.type = want;
                /* `checkExpr`, not `checkValue`: in value position a reference is an error that
                 * tells the author to write `*p`, but **here the reference is the value** -- the
                 * pointer itself is what is being converted. `ref x` and `alloc` are the language's
                 * other two stated exceptions to that rule, for the same reason. */
                Type *src = checkExpr(c, e->u.conv.operand);
                if (ttIsError(src)) return ttError(tt);
                Type *sb = src->kind == TY_REF ? ttBase(src->inner) : NULL;
                bool srcVoid = sb && sb->kind == TY_VOID;
                if (want->kind == TY_FN && srcVoid) {
                    if (src->nullable) {
                        ckError(c, e->line,
                                "`dlsym` returns a nullable pointer, and a `fn` has no null value:"
                                " check it or sign for it first (`p!`), then convert.",
                                "cannot convert `%s` to `%s`: it may be null",
                                typeStr(c, src), typeStr(c, want));
                        return ttError(tt);
                    }
                    return want;
                }
                /* One direction only. Handing a code pointer back out as an opaque address has no
                 * user in this line (the table carries `fn` fields, `dlclose` takes the handle the
                 * loader gave back), and **not** having it is worth something: a code pointer stays
                 * a code pointer, and cannot be laundered into a data pointer through here. The
                 * spelling would also collide with `ref x` (`ref void(f)` reads as `ref (void(f))`),
                 * so a real need would come with its own syntax decision rather than a quiet one. */
                ckError(c, e->line,
                        "This conversion exists for one thing: turning a C code address into a"
                        " callable value -- `fn(i64) -> i64(p)` where `p` is a non-null `ref void`."
                        " Pointers to data change type where they are declared, not here.",
                        "cannot convert `%s` to `%s`", typeStr(c, src), typeStr(c, want));
                return ttError(tt);
            }
            Type *t = ttFromName(tt, e->u.conv.typeName);
            if (!t || t->kind != TY_BUILTIN) {
                ckError(c, e->line, NULL, "`%s` is not a scalar type", e->u.conv.typeName);
                return ttError(tt);
            }
            e->u.conv.type = t;
            /* `checkExpr`, not `checkValue`: this node owns one exception to the value-position rule.
             * `u64(p)` / `i64(p)` is **the address of `p`** -- the one thing the plate model needs
             * (`base <= p < base + size`, one comparison, then trust) and the one thing a reference
             * cannot be asked for any other way, because `*p` would read the pointee and there is no
             * pointer arithmetic to compare with.
             *
             * It is deliberately **one way**: nothing turns a number back into a reference, so a
             * pointer can be checked but never forged, and the only operations on the number are the
             * integer ones (compare, subtract, index). The other half of `checkValue`
             * (`rejectNoCopy`) has nothing to say here: a source that is numeric is not a `@noCopy`
             * value, and a source that is a reference is the case handled right here. */
            Type *src = checkExpr(c, e->u.conv.operand);
            if (ttIsError(src)) return ttError(tt);
            if (src->kind == TY_REF) {
                if (ttIs(t, "u64") || ttIs(t, "i64")) {
                    /* `convCheck = false`: there is nothing to check -- the bits are the same width,
                     * and a pointer's value is whatever the machine put there. */
                    planSetConvCheck(e, false);
                    return t;
                }
                ckError(c, e->line,
                        src->nullable
                          ? "it is a nullable reference (`?ref T`) -- check it first:"
                            " `if p != null { ... *p ... }`"
                          : "write `*p` where the value is needed (`*p + 1`, `let v = *p`, `f(*p)`)",
                        "`%s` is a **reference**, not a value -- dereference it first: `*p`",
                        typeStr(c, src));
                return ttError(tt);
            }
            bool si = ttIsInteger(src), sf = ttIsFloat(src);
            bool ti = ttIsInteger(t),   tf = ttIsFloat(t);
            if (!((si || sf) && (ti || tf))) {
                ckError(c, e->line,
                        "explicit conversions are between numbers: integers and floats",
                        "cannot convert `%s` to `%s`", typeStr(c, src), typeStr(c, t));
                return ttError(tt);
            }
            /* Float to float follows C (round to nearest; out of range becomes an infinity),
             * so it is not checked. An integer widening is lossless and keeps the signedness,
             * so it is not checked either. Integer to float is not checked either: every integer
             * is inside the target float's range, the only loss is precision (round to nearest,
             * as in C). Everything else -- narrowing, a sign change, float to integer -- is
             * checked. */
            bool lossless = (si && ti && ttCanWiden(src, t)) || (sf && tf) || (si && tf);
            planSetConvCheck(e, !lossless);
            return t;
        }

        case EX_NEW: {
            /* `new T`, `new [N]T`, and `new T[n]`:
             *   a single T      ->  `mut ref T`
             *   a fixed `[N]T`  ->  `mut ref [N]T`
             *   `T[n]`          ->  `mut slice<T>`, which is how a buffer is built
             * The memory comes from the arena of the current block and is zeroed: in extC a
             * fresh allocation always reads as zero. */
            Type *w = ttResolve(tt, c->ctx, e->u.new_.type, e->line, c->curParams);
            if (ttIsError(w)) return ttError(tt);
            e->u.new_.type = w;

            if (w->kind == TY_PARAM || ttHasParam(w)) {
                /* `T` has no size while a template is being checked, so the size check is
                 * deferred to instantiation and recorded. That is what makes `new T[cap]`
                 * inside `varArray<T>` writable at all. If the size is still unknown at
                 * instantiation -- a nested generic that never received its argument, say --
                 * the error is reported there. */
                recordNewSizeCheck(c, w, e->line);
            } else if (ttIs(w, "void") || ttIsError(w)) {
                ckError(c, e->line,
                        "`new` needs a concrete type: its size is decided at compile time",
                        "cannot `new` `%s` -- its size is not known here", typeStr(c, w));
                return ttError(tt);
            }

            /* The level is the current block's depth: the same number the generated C uses to pick
             * `__extc_a[k]`. The exception is a function with a home arena (which hands its own allocations to
             * the caller): the site is recorded as `ARENA_HOME`, which reads as 0 under the `refDepth` measure,
             * that is, "the tier outside this frame" -- and that is exactly the tier the escape check lets
             * through, so `return h` in `fn build() -> mut ref node` is legal.
             *
             * This is only a starting point: when the escape check finds the new object stored somewhere that
             * lives longer, it raises `arenaLevel` to that tier, and `refDepth` follows `arenaLevel`, so the two
             * numbers must always agree. The same node may be checked twice, so the assignment happens only
             * the first time and only moves toward the longer-lived direction.
             *
             * The checker decides the level and codegen only translates it. Except for the home arena it
             * follows the block, but a store marked `@overwrite` has to live through every iteration of the
             * loop, so it lives in this frame at level 1. What is read here is the current `needsHome`, which
             * covers direct evidence only; a function that gains a home arena because of some call has it
             * decided later, once the closure is complete, by the check-module pass using
             * `FuncDef.arenaSites`. */
            if (planArenaLevel(e) == 0)
                planSetArenaLevel(e, (c->curFunc && anNeedsHome(c->curFunc)) ? ARENA_HOME
                              : (anReuse(e) ? 1 : (int)c->scopes.len));
            c->allocSites++;
            /* Keep the lexical level in its own field: the branch above may have replaced
             * `arenaLevel` with the `ARENA_HOME` sentinel, while the solver still needs to
             * know which block the site started in. */
            if (anLexicalLevel(e) == 0)
                anSetLexicalLevel(e, anReuse(e) ? 1 : (int)c->scopes.len);
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame". */
            anSetMinAt(e, -1);
        *(Expr **)vecPush(&c->curArenaSites) = e;
            /* `refDepth` follows `arenaLevel`: the two must always be the same number. The
             * only exception is `ARENA_HOME`, whose sentinel is -1 while `refDepth` speaks of
             * 0 as "the level outside this frame", so it maps back to 0 -- the home arena is
             * the scope the caller chose, which is depth 0. */
            {
                int depth = arenaDepthOf(planArenaLevel(e));    /* the one conversion point */
                if (anRefDepth(e) == 0 || anRefDepth(e) > depth) anSetRefDepth(e, depth);
            }

            if (!e->u.new_.count) {
                Type *r = ttRef(tt, w);
                r->mut = true;                  /* freshly allocated storage is writable */
                return r;
            }

            /* `T[n]`: the count must be an integer. An impure count needs a temporary so
             * that it is not evaluated twice. */
            Type *nt = checkValue(c, e->u.new_.count);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, e->u.new_.count->line, NULL,
                        "the element count must be an integer, found `%s`", typeStr(c, nt));
                return ttError(tt);
            }
            if (!repeatablePure(e->u.new_.count)) planSetNeedTemp(e, true);
            /* Freshly allocated memory is writable, so the view is a `mut slice<T>`: the
             * same rule that gives `a[..]` a `mut slice<T>` when `a` is writable. */
            return ttViewMut(tt, sliceOf(c, w), true);
        }

        case EX_GENCALL: {
            /* A generic call. Only the built-in primitives use this shape today:
             * `alloc<i32>(n)` asks the arena of the current frame for room for n values of T
             * and returns a writable reference.
             *
             * It is a primitive rather than a library function because the arena itself has to
             * be generated by the compiler: the library needs it to allocate for itself, so the
             * library cannot provide it. */
            /* `maxOf<i32>(4, 3)`: a free function called with explicit type arguments, the only available
             * spelling when `T` appears only in the return type; otherwise only the builtin-primitive rule
             * (`alloc<T>(n)`) applies. */
            /* `alloc<T>(n) -> mut ref T` is the one allocation primitive on this path: a
             * place for n values, reached through a reference.
             *
             * A buffer whose length is known only at run time -- reading a file of unknown
             * size, or the 64 KB the `reader` asks for -- is spelled `new T[n]`, which gives a
             * `mut slice<T>`. That is the only spelling: `allocSlice<T>(n)` is not a primitive,
             * and using it reports the migration below. */
            bool isAlloc  = strcmp(e->u.gencall.name, "alloc") == 0;
            /* The batch copy takes this path for the same reason `alloc` does: the
             * library needs memory the compiler has to name the type of. */
            bool isCopyInto = isCopyIntoName(e->u.gencall.name);
            /* The buffer primitives are on this path for the same reason: the generator has
             * to write the element type out in C, which the library cannot do. */
            bool isBufPrim = isBufPrimitiveName(e->u.gencall.name);
            /* The weak primitives share this path for the same reason: the generator has to write the
             * target type out in C, and weakUpgrade comes back as a ?ref T. */
            bool isWeakPrim = isWeakPrimitiveName(e->u.gencall.name);
            if (!isAlloc && !isCopyInto && !isBufPrim && !isWeakPrim) {
                FuncDef *tf = findFunc(c, e->u.gencall.name);
                if (tf && tf->typeParams.len == e->u.gencall.targs.len && tf->typeParams.len > 0) {
                    Vec targs;
                    vecInit(&targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < e->u.gencall.targs.len; i++) {
                        Type *t = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, i),
                                            e->line, c->curParams);
                        if (ttIsError(t)) return ttError(tt);
                        *(Type **)vecPush(&targs) = t;
                    }
                    /* The type arguments still mention `T` here: a call to `idOf<T>(...)`
                     * inside a generic body. Nothing is recorded at this point, because the node
                     * is rewritten to EX_CALL and walked again below, and that path notices the
                     * `T` argument and records the deferred check itself. One place instead of
                     * two, so the two cannot drift apart. */
                    FuncDef *inst = funcInstance(c, tf, &targs, e->line);
                    /* `funcInstance` reports the limit it hit and returns NULL (type nesting past
                     * `TYPE_DEPTH_LIMIT`, or more than `FUNC_INST_LIMIT` instances). Dereferencing
                     * that NULL would crash the compiler *while reporting its own limit*
                     * (`z<box<...65 deep...<i64>>>()`). The error is already recorded, so
                     * stop here. */
                    if (!inst) return ttError(tt);
                    planSetUsed(inst, true);
                    planSetUsed(tf, true);
                    /* Rewrite the node into an ordinary call: the shape every path except
                     * `alloc` uses. */
                    Vec args = e->u.gencall.args;
                    Expr *id = exprNew(c->arena, EX_IDENT, e->line);
                    id->u.ident.name = tf->name;
                    e->kind = EX_CALL;
                    e->u.call.callee = id;
                    e->u.call.args   = args;
                    anSetQualified(e, true);      /* do not trigger the qualification check */
                    planSetCallee(e, inst);
                    /* The written type arguments are handed to the EX_CALL path below through a side table:
                     * without it that path would infer from scratch and `f<i64>(x)` would be mere decoration --
                     * usable only where inference happens to succeed, and failing exactly where diagnostics
                     * recommend it (`T` appears only in the return type, or there are no arguments to infer
                     * from). */
                    ExplicitTargs *et = (ExplicitTargs *)arenaAllocZero(c->arena, sizeof *et);
                    et->node  = e;
                    et->targs = targs;
                    *(ExplicitTargs **)vecPush(&c->explicitTargs) = et;
                    /* After the rewrite the node must go through checkExpr again: argument checking, the return
                     * type, and the home arena all live on the EX_CALL path, so returning void here would be
                     * wrong. */
                    return checkExpr(c, e);
                }
                /* A removed primitive has to give a migration hint rather than "not a builtin": this spelling
                 * still appears in examples, tests, and user code. */
                if (strcmp(e->u.gencall.name, "allocSlice") == 0) {
                    ckError(c, e->line,
                            "`new T[n]` says the same thing -- a zeroed `mut slice<T>` -- and is"
                            " the only spelling now. The arena memsets every allocation, which is"
                            " what the zeroing promise always rested on (SPEC section 0.6).",
                            "`allocSlice<T>(n)` was removed; write `new T[n]`");
                    return ttError(tt);
                }
                /* `varArray<i64>(200)`: the constructor of a **generic** type. The type
                 * arguments are written out just as `varArray<i64>::withCap(200)` writes
                 * them -- nothing is inferred -- and the node becomes the associated call
                 * for the rest of the pipeline. */
                if (!lookup(c, e->u.gencall.name)) {
                    Type *base = ttFromName(tt, e->u.gencall.name);
                    StructDef *gsd = structOf(base);
                    if (gsd && gsd->typeParams.len == e->u.gencall.targs.len) {
                        FuncDef *ctor = NULL;
                        for (size_t i = 0; i < gsd->methods.len && !ctor; i++) {
                            FuncDef *m = *(FuncDef **)vecAt(&gsd->methods, i);
                            if (anIsAssoc(m) && strcmp(m->name, "new") == 0) ctor = m;
                        }
                        if (ctor) {
                            /* Every field is read out before the union is written: the two shapes overlap, and
                             * writing `u.assoc.typeName` first would overwrite `u.gencall.targs` and
                             * `u.gencall.args`, leaving the node holding a number where a list belongs. */
                            const char *gname = e->u.gencall.name;
                            Vec gtargs = e->u.gencall.targs;
                            Vec gargs  = e->u.gencall.args;
                            e->kind = EX_ASSOC;
                            e->u.assoc.typeName  = gname;
                            e->u.assoc.targs     = gtargs;
                            e->u.assoc.name      = "new";
                            e->u.assoc.args      = gargs;
                            e->u.assoc.isCall    = true;
                            e->u.assoc.modPrefix = NULL;
                            return checkExpr(c, e);
                        }
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note, "declare one inside `%s`:"
                                         "\n      fn new(...) -> %s { ... }",
                                  DN(gsd), DN(gsd));
                        bufPrintf(&note, "\nor build it field by field: `%s { ... }`"
                                         " (with its type arguments written out)",
                                  DN(gsd));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` has no constructor `new`, so `%s<...>(...)` has nothing to call",
                                DN(gsd), e->u.gencall.name);
                        return ttError(tt);
                    }
                }
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive or generic function",
                        e->u.gencall.name);
                return ttError(tt);
            }
            if (e->u.gencall.targs.len != 1) {
                ckError(c, e->line, NULL, "`%s` needs exactly one type argument, e.g. `%s<i32>(4)`",
                        e->u.gencall.name, e->u.gencall.name);
                return ttError(tt);
            }
            Type *elem = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, 0),
                                   e->line, c->curParams);
            if (ttIsError(elem)) return ttError(tt);
            /* Write the resolved type back: codegen reads the type arguments of the node, not
             * this local variable. */
            *(Type **)vecAt(&e->u.gencall.targs, 0) = elem;
            if (isBufPrim) return checkBufPrim(c, e, elem);
            if (isWeakPrim) return checkWeakPrim(c, e, elem);
            if (isCopyInto) return checkCopyInto(c, e, elem);
            if (e->u.gencall.args.len != 1) {
                ckError(c, e->line, NULL, "`%s` takes one argument (how many elements)",
                        e->u.gencall.name);
                return ttError(tt);
            }
            Expr *n = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *nt = checkValue(c, n);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, n->line, NULL, "the element count must be an integer, found `%s`",
                        typeStr(c, nt));
                return ttError(tt);
            }

            /* This memory lives until the current block ends (arenas are released per block), so its depth is
             * that block's depth: `c->scopes.len`. Here is the seam between the depth model and the arena
             * granularity, and both sides must use the same number: the checker compares block depths to decide
             * whether a reference may live here, and the generated C uses the block depth to pick
             * `__extc_a[k]`, freeing it when the block ends. It must be `c->scopes.len` and not a hard-coded 1:
             * otherwise `p` in `while { p = alloc<i32>(1) }` would point at freed memory on the next
             * iteration. As a result both "return freshly allocated memory" and "store a loop-local allocation
             * outside the loop" are caught by the escape check; to hand memory out, the caller has to provide
             * a buffer or an arena.
             *
             * An `alloc` site gets its level by exactly the same rule as `new`: a function with a home arena
             * gets `ARENA_HOME` (depth 0, "the tier outside this frame"), otherwise the block level. */
            if (c->curFunc && anNeedsHome(c->curFunc)) {
                planSetArenaLevel(e, ARENA_HOME);
                anSetRefDepth(e, 0);
            } else {
                anSetRefDepth(e, c->scopes.len);
                planSetArenaLevel(e, (int)c->scopes.len);
            }
            c->allocSites++;
            if (anLexicalLevel(e) == 0) anSetLexicalLevel(e, (int)c->scopes.len);   /* lexical level */
            /* The initial value is "no constraint has touched this site yet". The 0 that `arenaAllocZero`
             * gives cannot be relied on, because 0 means "must outlive this frame"; `alloc` is an allocation
             * site too and, like `new`, needs this initialization. */
            anSetMinAt(e, -1);
            /* `alloc` has to be registered in `c->curArenaSites` like the `new` in EX_NEW: unregistered, the
             * rewrite traversal that runs after closure never sees it, a home-arena `alloc` stays at the block
             * level and is freed with the block, and a family of false rejections results -- the same program
             * written with `new` passes while the one written with `alloc` is rejected. */
            *(Expr **)vecPush(&c->curArenaSites) = e;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* freshly allocated storage is writable */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b`: use the fallback when there is no value. The meaning is
             * `match a { some(v) => v  _ => b }`, but only one side is ever evaluated: when `a` has a value,
             * `b` is never computed -- another short circuit, like `&&` / `||`. */
            /* The flag must be read **before** the subject is checked: when the subject is itself a call,
             * checking it raises the flag, and reading afterwards would misreport a form like
             * `var a = h() ?? 0` appearing as the first statement. */
            bool priorFx = c->stmtFx != 0;
            Type *mt = checkExpr(c, e->u.coalesce.main);
            if (ttIsError(mt)) { checkExpr(c, e->u.coalesce.fallback); return ttError(tt); }

            bool isOpt = isProtoType(mt, "option", 1);
            bool isRes = isProtoType(mt, "result", 2);
            Type *want = NULL;
            if (isOpt || isRes) {
                want = *(Type **)vecAt(&mt->targs, 0);          /* the payload type T */
            } else if (mt->kind == TY_REF && mt->nullable) {
                want = mt;   /* `?ref T`: the fallback is a reference as well */
            } else {
                /* Position rule: `??` is meaningful only for something that may have no
                 * value. Saying so beats inventing a meaning for it. */
                checkExpr(c, e->u.coalesce.fallback);
                ckError(c, e->line,
                        "`??` means \"if there is nothing, use this instead\", so the left"
                        " side must be an `option`, a `result`, or a `?ref T`",
                        "left of `??` is `%s`, which always has a value", typeStr(c, mt));
                return ttError(tt);
            }

            /* When the subject is not pure (`f() ?? -1`), an ordinary conditional expression cannot be used:
             * the subject would appear twice in the C, so `f()` would run twice. It has to be evaluated into a
             * temporary first, and a temporary's prefix can only be emitted before the whole statement -- so it
             * jumps ahead of earlier side effects in the same statement:
             *     `g() + (h() ?? 0)`   reads as g then h, but h runs first
             * Silently reordering is worse than reporting an error and making the user split the line: the order
             * the user reads must be the order that executes. Only calls count as evidence; the order of `new`,
             * literals, and conversions is not observable, so they are not reported. */
            if (!repeatablePure(e->u.coalesce.main)) {
                checkExpr(c, e->u.coalesce.fallback);
                if (priorFx && !c->noHoist) {
                    ckError(c, e->line,
                            "The subject of `??` is computed into a temporary **before the whole"
                            " statement** (that is what makes it run only once). So an earlier"
                            " side effect in the same statement would run *after* it -- the"
                            " opposite of what the line reads like. Split the statement in two"
                            " (`var t = f() ?? 0` then use `t`).",
                            "`??` here would run **before** the call that comes earlier in this"
                            " statement; split the line so the order you read is the order it runs");
                    return ttError(tt);
                }
                if (c->noHoist) {
                    ckError(c, e->line,
                            "`while` re-evaluates its condition every round, but a temporary"
                            " can only be computed once, before the loop. Bind it inside the"
                            " loop body: `while true { let r = f()  if ... { break } }`",
                            "`??` here cannot be evaluated ahead of time -- the temporary"
                            " would run once instead of every round");
                    return ttError(tt);
                }
                planSetNeedTemp(e, true);        /* codegen evaluates it once, then tests the temp */
                /* All side effects of the subject are hoisted into the prefix in source order together with the
                 * other temporaries, so the subject does not count as "a call left in place"; the fallback stays
                 * in the conditional expression and runs only when the subject has no value, so it does
                 * count. */
                c->stmtFx = priorFx ? 1 : 0;
                if (exprHasCall(c, e->u.coalesce.fallback)) c->stmtFx = 1;
                return want;
            }

            adoptContextType(e->u.coalesce.fallback, want);
            Type *ft = checkInto(c, want, e->u.coalesce.fallback);
            checkAssignable(c, want, ft, e->u.coalesce.fallback, "the right side of `??`");
            e->type = want;
            return want;
        }

        case EX_SIGN: {
            /* `e!` is the user's signature: what the compiler cannot prove, the user vouches for. The
             * vouching **is checked at run time**: a wrong signature traps with a source location instead of
             * undefined behavior; the only way to sign that check away is `@unchecked` on the function. Three
             * uses:
             *   `opt!` (`option<T>`)  -> payload T
             *   `r!` (`result<T,E>`)  -> payload T
             *   `p!` (`?ref T`)       -> `ref T`, "I know it is not null"
             * Every other type is an error: a signature has to be written where it means something. */
            /* This case needs no `priorFx` bookkeeping: codegen emits a helper call around the
             * operand (`unwrapHelper` for `option`/`result`, `extc_refAssert` for `?ref T`), so
             * it is evaluated once, in place, in any expression position -- including a `while`
             * condition, where a statement-prefix temporary cannot live; the operand's own helper
             * call keeps the order. */
            Type *ot = checkExpr(c, e->u.sign.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (isProtoType(ot, "option", 1) || isProtoType(ot, "result", 2)) {
                return *(Type **)vecAt(&ot->targs, 0);      /* the payload type */
            }
            if (ot->kind == TY_REF) {
                if (!ot->nullable) {
                    ckError(c, e->line, "it is already a plain `ref T`, which can never be null",
                            "`%s` is not nullable, so `!` has nothing to assert",
                            typeStr(c, ot));
                    return ttError(tt);
                }
                Type *nn = ttRef(tt, ot->inner);            /* the non-null version */
                nn->mut = ot->mut;
                e->type = nn;
                return nn;
            }
            ckError(c, e->line,
                    "`!` means \"I sign for it\": it turns an `option` / `result` / `?ref T`"
                    " into the value / non-null reference without any check",
                    "`!` needs an `option`, a `result`, or a nullable reference, found `%s`",
                    typeStr(c, ot));
            return ttError(tt);
        }

        case EX_DEREF: {
            /* `*p` is the value, or the place, that `p` points at. Writability comes from the
             * type of `p` (`mut ref`). */
            Type *ot = checkExpr(c, e->u.deref.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (ot->kind != TY_REF) {
                ckError(c, e->line,
                        "`*` means \"the value this reference points at\"; a plain value has"
                        " nothing to dereference",
                        "cannot dereference `%s`, which is not a reference", typeStr(c, ot));
                return ttError(tt);
            }
            if (rejectNullableDeref(c, ot, e->u.deref.operand, "`*`")) return ttError(tt);
            /* `ref void` is C's `void *`: one pointer with no element type, so there is nothing to
             * read. C refuses the same thing, but it refuses it in the generated C, where the error
             * points at a line the author never wrote; the boundary admits `ref void` on purpose
             * (`mmap`, an opaque handle), and this is the other half of that deal. */
            if (ot->inner && ot->inner->kind == TY_VOID) {
                ckError(c, e->line,
                        "A `ref void` is C's `void *` -- a handle extC cannot look inside. Hand it"
                        " back to C, or declare the parameter with the pointee type it really has"
                        " (`ref i64`, `ref point`).",
                        "cannot dereference `%s`: `void` has no size", typeStr(c, ot));
                return ttError(tt);
            }
            return ot->inner;
        }

        case EX_ENUMVAL: {
            /* An instance of a generic enum (`maybe<i64>`) is not in the type table's by-name
             * map: it is produced by instantiation. Constructing it through the EX_ASSOC path
             * therefore records the resolved type on `assocOwner`, which is preferred here. */
            Type *et = planAssocOwner(e) ? planAssocOwner(e) : ttFromName(tt, e->u.enumval.typeName);
            if (!et || et->kind != TY_ENUM || !et->edef) return ttError(tt);
            Variant *v = findVariant(et->edef, e->u.enumval.variant);
            if (!v) return ttError(tt);

            /* A variant without a payload, `status.ok`: it takes no arguments. */
            if (v->types.len == 0) {
                if (e->u.enumval.args.len > 0) {
                    ckError(c, e->line, NULL,
                            "`%s.%s` carries no payload, so it takes no arguments",
                            et->name, v->name);
                    return ttError(tt);
                }
                return et;
            }

            /* Constructing a variant with a payload, `shape.circle(2.0)`: payload values are
             * matched by position. */
            if (e->u.enumval.args.len != v->types.len) {
                ckError(c, e->line, NULL,
                        "`%s.%s` carries %zu value(s), got %zu",
                        et->name, v->name, v->types.len, e->u.enumval.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < v->types.len; i++) {
                Type *pt  = payloadType(tt, et, v, i);
                Expr *arg = *(Expr **)vecAt(&e->u.enumval.args, i);
                Type *at  = checkInto(c, pt, arg);
                checkAssignable(c, pt, at, arg, "payload value");
                ckOwningArgMoveOnly(c, pt, arg, "payload");
                /* A plain name handed to a payload is **transferred**, not copied: the payload
                 * lives inside the constructed value, so it owns what the name held and the name
                 * is dead from here on. The generator marks that move so the name's own drop does
                 * not free the storage a second time; recording it here turns a later read into a
                 * diagnostic instead of a silent read of storage somebody else now owns. */
                if (arg->kind == EX_IDENT && payloadOwnsStorage(c, pt, 0)) {
                    Sym *srcSym = lookup(c, arg->u.ident.name);
                    if (srcSym && srcSym->movedLine == 0) {
                        srcSym->movedLine = e->line;
                        srcSym->movedInto = et->name;
                    }
                }
                /* The payload is stored inside this value, so a reference it carries must not
                 * live for less than the value does. */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }


        case EX_CALL: {
            /* The return type the context expects applies to the inference of **this** call only: it is cleared
             * before the arguments are entered, otherwise `g` in `f(g(x))` would wrongly pick up `f`'s return
             * type. */
            Type *wantRet = c->expectRet;
            c->expectRet = NULL;
            /* A specifier slot of a value f-string: a reserved-name call built by the parser, redirected by the
             * slot value's static type to a `std::text::string` pad function (see `checkFstrPadCall`). */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT &&
                strcmp(e->u.call.callee->u.ident.name, "__extc_fstr_pad") == 0)
                return checkFstrPadCall(c, e);
            /* Two/three-subscript sugar written directly on an `ndarray`: lend the view argument before
             * checking. */
            sugarNdViewArg(c, e);
            /* The `f` in `f(x)` holds a lambda: the node is rewritten **in place** into the method call
             * `f.call(x)` and then handed to the ordinary method path -- the same in-place rewrite the variant
             * branch below performs. */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                if (sy && sy->type && sy->type->kind == TY_STRUCT && sy->type->sdef
                    && anLamSig(sy->type->sdef)) {
                    Expr *recv = e->u.call.callee;      /* read before `u.method` overwrites them */
                    Vec   args = e->u.call.args;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "call";
                    e->u.method.args = args;
                    return checkExprInner(c, e);
                }
            }
            /* The `f` in `f(x)` **holds a value of function type** rather than naming a declaration: the call
             * goes through a code pointer and the signature comes from the type. This branch comes before all
             * the by-name branches below, because they all describe a declaration this call does not have. */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                Type *ct = (sy && sy->type) ? tsub(c, sy->type) : NULL;
                if (ct && ct->kind == TY_FN) return checkCallThroughFn(c, e, ct);
            }
            /* The `f` in `f(x)` is a value whose type mentions a **type parameter** (`F`): which method
             * this is only becomes known per instance, exactly like `f.call(x)` on a type parameter.
             * Rewritten in place into that call so the deferred path decides it once the instance is
             * known; a type that turns out not to be callable then reports "no method `call`". */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                if (sy && sy->type && mentionsParam(tsub(c, sy->type))) {
                    Expr *recv = e->u.call.callee;      /* read before `u.method` overwrites them */
                    Vec   args = e->u.call.args;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "call";
                    e->u.method.args = args;
                    return checkExprInner(c, e);
                }
            }
            /* `o.f(x)` where `f` is a **field** holding a `fn`, not a method. Without this the
             * method path answers "no method `f` on `op`" -- a sentence about the wrong thing, on a
             * program that has the field. The field's type is peeked at rather than checked, so the
             * ordinary method path (which checks the receiver itself) is untouched. */
            if (e->u.call.callee && e->u.call.callee->kind == EX_FIELD) {
                Type *ft2 = peekFieldType(c, e->u.call.callee);
                if (ft2 && ft2->kind == TY_FN) return checkCallThroughFn(c, e, ft2);
            }
            /* Constructing a variant with a payload, `shape.circle(2.0)`: it looks like a field
             * access followed by a call, but `shape` is a type name and not a variable, so it is
             * recognised as an enum construction and rewritten to EX_ENUMVAL. */
            if (e->u.call.callee->kind == EX_FIELD) {
                Expr *fld = e->u.call.callee;
                if (fld->u.field.obj->kind == EX_IDENT && !lookup(c, fld->u.field.obj->u.ident.name)) {
                    Type *et = ttFromName(tt, fld->u.field.obj->u.ident.name);
                    if (et && et->kind == TY_ENUM && et->edef) {
                        Variant *v = findVariant(et->edef, fld->u.field.name);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, fld->u.field.name);
                            return ttError(tt);
                        }
                        Vec args = e->u.call.args;      /* union: copy it out first */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->u.enumval.args     = args;
                        planSetAssocOwner(e, et);
                        return checkExprInner(c, e);    /* the rest is EX_ENUMVAL's job */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
                /* A callee that is not a name: `fs[i](x)`, `(*p)(x)`, `box.f(x)` written as a bare
                 * call. When its type is a `fn(...)` the call goes through that code pointer; any
                 * other callee type has no meaning here. */
                Type *ct = checkExpr(c, e->u.call.callee);
                if (ct && ttBase(ct) && ttBase(ct)->kind == TY_FN)
                    return checkCallThroughFn(c, e, ttBase(ct));
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            /* `flush()` pushes out the buffer that `println` writes through.
             *
             * It is needed because `std::io`'s `writeBytes` writes straight to the file
             * descriptor with `write(2)` and no buffering, while `println` goes through printf,
             * which buffers. Mixing the two loses the order: the prompt has not been printed yet
             * and the program is already waiting for input. */
            if (strcmp(name, "flush") == 0) {
                if (e->u.call.args.len != 0) {
                    ckError(c, e->line, "`flush()` takes no arguments.",
                            "`flush` takes no arguments");
                }
                return ttVoid(tt);
            }

            /* Files are the program's own business; the compiler keeps no descriptor table -- `std::fs` closes
             * files with `close(2)` declared as `extern!`, and the handle's `open` flag makes a second close a
             * no-op; what the compiler can do is prove a leak when it can. */
            /* `io::print` / `io::println` are the **same** builtins under the module's name:
             * `std::io` re-exports them, and a value printable through one spelling must be
             * printable through the other. The name is answered here, before the library's own
             * two functions are looked up, so both spellings share one printer. */
            if (isPrintBuiltinName(name)) {
                /* `print` / `println` are builtins dispatched by name; they go through the **same buffer** as
                 * `io::cout` (codegen's `genPrint` emits `extc_printf` / `extc_cout_put`, both writing the cout
                 * buffer), so interleaved writes keep source order and need no `flush`. Structural types still
                 * go to the descriptor printer, which writes that same buffer. */

                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr **slot = (Expr **)vecAt(&e->u.call.args, i);
                    Expr *a = *slot;
                    Type *at = checkPrintArg(c, a); /* takes a value, so it dereferences */
                    /* `string` is a struct with an inline buffer, and descriptor printing would spill it field
                     * by field. It is itself a run of bytes, so handing it to `asSlice()` takes the `slice<u8>`
                     * text path. */
                    if (at && at->kind == TY_STRUCT && at->sdef && at->sdef->name &&
                        strstr(at->sdef->name, "string")) {
                        /* `view()` is the convention (`asSlice()` is the older spelling). */
                        const char *lend = findMethod(at, "view") ? "view" : "asSlice";
                        Expr *m = exprMethodNew(c->arena, a, lend, a->line);
                        *slot = m;
                        a = m;
                        at = checkPrintArg(c, a);
                    }
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            /* A diagnostic has to echo the name as written in the source (`open`), not the
             * mangled `lib$open`. Without `srcName` the message would read "write
             * `lib::lib$open`", which is nonsense. */
            const char *shownName = e->u.call.callee->u.ident.srcName
                                    ? e->u.call.callee->u.ident.srcName : name;
            FuncDef *f = findFunc(c, name);
            /* A function of another module written without qualification: `lib.extc` has `open`, and this
             * source writes `open()`.
             *
             * A name without `$` is a source-level name (the loader rewrites only qualified names into flat
             * ones), so the right thing to say here is "a qualified name is required", not "there is no such
             * function". The criterion is whether the name contains `$`: that character is not legal in an extC
             * identifier, so its presence means the loader rewrote the name, and only then is the function
             * really missing. */
            if (!f && name && !strchr(name, '$') && !anQualified(e)) {
                for (size_t i = 0; i < c->m->funcs.len; i++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&c->m->funcs, i);
                    const char *d = cand->name ? strchr(cand->name, '$') : NULL;
                    if (d && cand->modName && !cand->isPrivate && strcmp(d + 1, shownName) == 0) {
                        requireQualified(c, shownName, cand->modName, false, e->line);
                        return ttError(tt);
                    }
                }
            }
            if (f && !f->reserved && f->modName && !anQualified(e))
                requireQualified(c, shownName, f->modName, false, e->line);
            if (!f) {
                /* The bare enum constructor. The error must point the way rather than only say "there is no such
                 * function": after `type shape = | circle(f64) | ...`, writing `circle(2.0)` has to report that
                 * `circle` is a variant of `shape` and point at `shape.circle(...)`, not a bare "the builtins are
                 * only print/println" with no correct spelling. No type inference is done here: writing the type
                 * out in full is deliberate, and the message only states the correct spelling instead of filling
                 * it in for the user. Only payload-carrying variants are redirected this way; a payload-less
                 * `status.ok` is a value by itself and takes the other path. */
                /* `ifstream("input.txt")`: a call whose name is a **type** is that type's constructor, that is,
                 * the associated function `new`. It is rewritten into an associated call node, so argument
                 * checking, arena arguments, and the generated C are identical to
                 * `ifstream::new("input.txt")`, with no second implementation to drift (the constructor's one
                 * path).
                 *
                 * Only when the name is not shadowed by a local binding: otherwise a variable shadowed by a type
                 * name would be reported as a missing constructor, which says the wrong thing about a problem
                 * that does not exist. */
                if (!lookup(c, name)) {
                    Type *ct = ttFromName(tt, name);
                    if (ct && !ttIsError(ct)) {
                        StructDef *csd = structOf(ct);
                        FuncDef *ctor = NULL;
                        if (csd) {
                            for (size_t i = 0; i < csd->methods.len && !ctor; i++) {
                                FuncDef *m = *(FuncDef **)vecAt(&csd->methods, i);
                                if (anIsAssoc(m) && strcmp(m->name, "new") == 0) ctor = m;
                            }
                        }
                        if (ctor) {
                            Vec cargs = e->u.call.args;      /* union: copy it out first */
                            e->kind = EX_ASSOC;
                            e->u.assoc.typeName  = name;
                            e->u.assoc.name      = "new";
                            e->u.assoc.args      = cargs;
                            e->u.assoc.isCall    = true;
                            e->u.assoc.modPrefix = NULL;
                            /* Fields used by the old kind are still in the union, so every one of them has to be
                             * written: missing `targs` leaves the node holding values left over from this very
                             * call, and the parser then reports "`named` expects 0 type arguments, got 1" -- for
                             * a type nobody wrote type arguments for (the node's leftovers). */
                            vecInit(&e->u.assoc.targs, c->arena, sizeof(void *));
                            return checkExprInner(c, e);
                        }
                        if (csd) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "declare one inside `%s`:", DN(csd));
                            bufPrintf(&note, "\n      fn new(...) -> %s { ... }", DN(csd));
                            bufPrintf(&note, "\nor build it field by field: `%s { ... }`",
                                      DN(csd));
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no constructor `new`, so `%s(...)` has nothing to call",
                                    DN(csd), name);
                            return ttError(tt);
                        }
                    }
                }
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, name)) owner = td;
                }
                if (owner) {
                    /* The `note` argument of `ckError` is passed through verbatim: unlike the
                     * format it takes no varargs, so a value has to be rendered with
                     * `arenaPrintf` first. Passing a raw `%s` printed the literal text. */
                    const char *note = arenaPrintf(c->arena,
                            "A payload variant is named by its type -- write `%s.%s(...)`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a function -- did you mean `%s.%s(...)`?",
                            name, DN(owner), DN(owner), name);
                    return ttError(tt);
                }
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* code generation keys off this flag, not e->func (NULL here) */
                    e->type = f->ret;
                    return f->ret;
                }
                if (isViewOfDecl(f)) {
                    /* `extc_viewOf(p, n)`: no runtime check of any kind -- whoever calls it owns the
                     * pointer (the plate checks it with `holds`, the boundary signs for it in a
                     * declaration). The shapes are still checked: a byte pointer and a length. */
                    if (e->u.call.args.len != 2) {
                        ckError(c, e->line, NULL, "`extc_viewOf` takes 2 arguments, got %zu",
                                e->u.call.args.len);
                        return ttError(tt);
                    }
                    Expr *ap = *(Expr **)vecAt(&e->u.call.args, 0);
                    Expr *an = *(Expr **)vecAt(&e->u.call.args, 1);
                    Type *tp = checkExpr(c, ap);   /* the reference **is** the value here */
                    Type *tn = checkExpr(c, an);
                    if (!ttIsError(tp) && !(tp->kind == TY_REF &&
                                            ttBase(tp->inner) && ttBase(tp->inner)->kind == TY_BUILTIN))
                        ckError(c, ap->line,
                                "It re-types a pointer without checking anything, so it insists on"
                                " the one shape it can mean: `ref u8`.",
                                "`extc_viewOf` wants a `ref u8`, found `%s`", typeStr(c, tp));
                    if (!ttIsError(tn) && !ttIsInteger(tn))
                        ckError(c, an->line, NULL,
                                "`extc_viewOf` wants an integer length, found `%s`", typeStr(c, tn));
                    planSetViewOf(e, true);              /* code generation keys off this flag (no body, hence no func) */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                /* `vecAt` does no bounds check: `parallel::run()` with zero arguments would dereference the NULL
                 * base of an empty vector and crash the compiler. The shape is reported first here; with one
                 * argument the diagnostic path is already reached, with none it was not. */
                if (e->u.call.args.len == 0) {
                    ckError(c, e->line,
                            "`parallel::run(worker, views..., n)` takes the worker function's"
                            " **name** first (v1 has no first-class function values).",
                            "`parallel::run` needs at least the worker function's name");
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* Signature: `(id: i64, lo: i64, hi: i64, views...)`, where **exactly one** `mut` view is the
                 * output (partitioned over [lo,hi)) and the rest are **read-only** views (every worker gets the
                 * same one, no partitioning). */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Expr *an = *(Expr **)vecAt(&e->u.call.args, 1 + nViews);
                    Expr *ax = *(Expr **)vecAt(&e->u.call.args, 2 + nViews);
                    Type *tn = checkExpr(c, an);
                    Type *tx = checkExpr(c, ax);
                    /* A bare integer literal is an i64 in this position (the argument has no
                     * other context to say otherwise), so `parallel::run(w, out, n, 2)` does not
                     * need the `i64(...)` wrapper. Anything else still has to be i64. */
                    if (!(isI64Type(tn) || (an->kind == EX_INT && literalFits(an, c->tI64))) ||
                        !(isI64Type(tx) || (ax->kind == EX_INT && literalFits(ax, c->tI64)))) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);
                planSetParTlsArena(wf, true);
                planSetUsed(wf, true);
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);
            /* Method callers are recorded per instance too (free functions have no owner, and the helper skips
             * them itself). */
            if (e->kind == EX_METHOD)
                ckNoteInstUse(f, e->u.method.recv->type);

            /* A generic free function: `T` can be inferred from an argument only, because an
             * instance of a type is decided by the type while a call to a free function has
             * nothing but its arguments. So the arguments are checked first, then `T` is
             * inferred, then the instance is created. When inference fails because `T` appears
             * only in the return type, the message tells the user to write the arguments
             * explicitly: `f<i32>(...)`. */
            if (f->typeParams.len > 0) {
                {
                    size_t need = minArgsFor(f, 0);
                    if (e->u.call.args.len < need || e->u.call.args.len > f->params.len) {
                        if (need == f->params.len)
                            ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                                    name, f->params.len, e->u.call.args.len);
                        else if (need == 0)
                            ckError(c, e->line, NULL,
                                    "`%s` expects at most %zu argument(s), got %zu",
                                    name, f->params.len, e->u.call.args.len);
                        else
                            ckError(c, e->line, NULL,
                                    "`%s` expects %zu to %zu argument(s), got %zu",
                                    name, need, f->params.len, e->u.call.args.len);
                        return f->ret ? f->ret : ttVoid(tt);
                    }
                    fillDefaultArgs(c, &e->u.call.args, f, 0);
                }
                /* Lend container arguments before inference: inference must read the node types that will
                 * actually be checked. */
                for (size_t i = 0; i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i);
                    lendViewArg(c, p->type, (Expr **)vecAt(&e->u.call.args, i));
                }
                Vec targs;
                vecInit(&targs, c->arena, sizeof(void *));
                Vec *written = NULL;
                for (size_t k = 0; k < c->explicitTargs.len && !written; k++) {
                    ExplicitTargs *cand = *(ExplicitTargs **)vecAt(&c->explicitTargs, k);
                    if (cand->node == e) written = &cand->targs;
                }
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    /* Seeded with the written `<...>`; the arguments still have to agree with it
                     * (unification reports a mismatch). `NULL` means "infer this one". */
                    Type *ex = (written && i < written->len)
                                   ? *(Type **)vecAt(written, i) : NULL;
                    *(Type **)vecPush(&targs) = ex;
                }
                /* When a type parameter appears only in the return type (`dup<A, B>(x, y) -> pair<A, B>`), the
                 * return type the context expects is the only witness: `let p: pair<i64, u8> = dup(3, u8(4))`.
                 * The expectation is only a **hint**: the arguments are inferred first and it fills in only what
                 * they cannot determine (merged in the last step), so `dup(u8(2), u8(5))` at an expected
                 * `pair<i16, u16>` still gets `u8` from the arguments, and the outer `checkAssignable` reports the
                 * clear "expected ..." mismatch instead of this code changing its story or reporting "cannot
                 * infer". Only expectations without type parameters are accepted (an expectation inside a
                 * template body is still `T`, which says nothing). */
                Vec hintTargs;
                vecInit(&hintTargs, c->arena, sizeof(void *));
                for (size_t i = 0; i < f->typeParams.len; i++)
                    *(Type **)vecPush(&hintTargs) = NULL;
                if (wantRet && !mentionsParam(wantRet) && f->ret)
                    (void)unifyTParams(c->tt, &f->typeParams, &hintTargs, f->ret, wantRet);
                /* Literal arguments are set aside: a literal has no type of its own before the
                 * context decides one, so it must not *decide* a type parameter that another
                 * argument can decide. `seq::count(xs, 5)` with `xs: slice<i64>` infers `T = i64`
                 * from `xs` and then fits the `5`; `m<i64>(3)` fits the `3` against the written
                 * argument. Only a literal that leaves no other witness keeps the old behaviour
                 * (it is unified at its default type, which is how `f(5)` infers i32). */
                Vec argTypes;
                vecInit(&argTypes, c->arena, sizeof(void *));
                Vec lateArgs;
                vecInit(&lateArgs, c->arena, sizeof(size_t));
                for (size_t i = 0; i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i);
                    Expr  *a = *(Expr **)vecAt(&e->u.call.args, i);
                    adoptContextType(a, p->type);
                    Type *at = checkExpr(c, a);
                    if (ttIsError(at)) return ttError(tt);
                    /* A `ref T` parameter: when the argument is written `ref x`, `at` is a
                     * reference, so the pointee types are unified. */
                    Type *want = p->type;
                    if (want->kind == TY_REF && at->kind == TY_REF) { want = want->inner; at = at->inner; }
                    *(Type **)vecPush(&argTypes) = at;
                    if (ttHasParam(want) &&
                        (a->kind == EX_INT || a->kind == EX_FLOAT || a->kind == EX_BOOL)) {
                        *(size_t *)vecPush(&lateArgs) = i;
                        continue;
                    }
                    if (!unifyTParams(c->tt, &f->typeParams, &targs, want, at)) {
                        ckError(c, e->line,
                                "A generic function's type parameters are inferred from its arguments."
                                " Write them out at the call to supply one that no argument mentions:"
                                " `f<i32>(...)` seeds the inference.",
                                "cannot infer type parameter(s) of `%s` from the arguments", name);
                        return f->ret ? f->ret : ttVoid(tt);
                    }
                }
                /* Type parameters the arguments cannot determine are filled in from the return type the context
                 * expects (a hint is possible only when the return type mentions it, see `hintTargs` above). This
                 * is placed before the literal arguments: the two literals in `dup(7, 9)` must be checked against
                 * the hinted types, otherwise they would both default to i32. */
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    if (*(Type **)vecAt(&targs, i)) continue;
                    *(Type **)vecAt(&targs, i) = *(Type **)vecAt(&hintTargs, i);
                }
                for (size_t k = 0; k < lateArgs.len; k++) {
                    size_t i = *(size_t *)vecAt(&lateArgs, k);
                    Param *p = *(Param **)vecAt(&f->params, i);
                    Expr  *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = *(Type **)vecAt(&argTypes, i);
                    Type *want = p->type;
                    if (want->kind == TY_REF && at->kind == TY_REF) { want = want->inner; at = at->inner; }
                    if (unifyTParams(c->tt, &f->typeParams, &targs, want, at)) continue;
                    Type *fitted = ttSubstitute(c->tt, want, &f->typeParams, &targs);
                    if (literalFits(a, fitted)) continue;
                    ckError(c, e->line,
                            "A generic function's type parameters are inferred from its arguments."
                            " Write them out at the call to supply one that no argument mentions:"
                            " `f<i32>(...)` seeds the inference.",
                            "cannot infer type parameter(s) of `%s` from the arguments", name);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    if (*(Type **)vecAt(&targs, i)) continue;
                    ckError(c, e->line,
                            "A generic function's type parameters are inferred from its arguments."
                            " Write them out at the call to supply one that no argument mentions:"
                            " `f<i32>(...)` seeds the inference.",
                            "cannot infer type parameter `%s` of `%s`",
                            *(const char **)vecAt(&f->typeParams, i), name);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                /* Calling a generic function from inside a generic body: the type arguments
                 * still mention `T`, so no correct instance can be built at this moment. That is
                 * not an error either.
                 *
                 * The technique matches `RefCheck` and `OpCheck`: while the template is checked,
                 * an instance whose arguments are the parameters themselves is created
                 * (`idOf_T`), which is self-consistent and treats `T` as opaque, so the rest of
                 * the template body still type-checks. The call is recorded, and when the
                 * instance is checked again, the callee is redirected to the concrete instance
                 * (`idOf_i32`). */
                FuncDef *inst = funcInstance(c, f, &targs, e->line);
                /* Same NULL contract as at EX_GENCALL above: the error has already been recorded, so stop right
                 * here. */
                if (!inst) return ttError(tt);
                planSetUsed(inst, true);
                planSetCallee(e, inst);
                f = inst;                     /* every check below uses the instance */

                bool hasParamTarg = false;
                for (size_t i = 0; i < targs.len; i++) {
                    Type *a = *(Type **)vecAt(&targs, i);
                    if (a && ttHasParam(a)) { hasParamTarg = true; break; }
                }
                if (hasParamTarg && c->curFunc) {
                    CallCheck *cc = (CallCheck *)arenaAllocZero(c->arena, sizeof(CallCheck));
                    FuncDef *tmplOfF = planTemplate(f);
                    cc->node = e;
                    cc->tmpl = tmplOfF ? tmplOfF : f;   /* the template, not the instance */
                    cc->func = c->curFunc;   /* which template body this belongs to */
                    vecInit(&cc->targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < targs.len; i++)
                        *(Type **)vecPush(&cc->targs) = *(Type **)vecAt(&targs, i);
                    *(CallCheck **)vecPush(&c->callChecks) = cc;
                }
            }

            {
                size_t need = minArgsFor(f, 0);
                if (e->u.call.args.len < need || e->u.call.args.len > f->params.len) {
                    if (need == f->params.len)
                        ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                                name, f->params.len, e->u.call.args.len);
                    else if (need == 0)
                        ckError(c, e->line, NULL, "`%s` expects at most %zu argument(s), got %zu",
                                name, f->params.len, e->u.call.args.len);
                    else
                        ckError(c, e->line, NULL, "`%s` expects %zu to %zu argument(s), got %zu",
                                name, need, f->params.len, e->u.call.args.len);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                fillDefaultArgs(c, &e->u.call.args, f, 0);
            }
            /* A read-only view parameter accepts a container argument of the same element: first lend `va` / `a`
             * in place into real `va.asSlice()` / `a[..]` nodes, so that inference and checking afterwards see
             * that node. */
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                Expr **slot = (Expr **)vecAt(&e->u.call.args, i);
                lendViewArg(c, p->type, slot);
            }
            /* The arena for this call comes from the shallowest `mut ref` argument. */
            anSetHomeDepth(e, callHomeDepth(c, &e->u.call.args, &f->params, e));
            setCallArenaArg(c, e);      /* decide which arena the call finally passes */
            setCallZoneArg(c, e);       /* pool-creating call: the layer the pool is allocated in */
            /* The reference check has to run after the arguments are checked: until then
             * `a->type` is not filled in, `typeContainsRef` answers "no" for everything, and a
             * violation is silently accepted. The method path hit the same trap. The check itself
             * is at the end of this case. */
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr **slot = (Expr **)vecAt(&e->u.call.args, i);
                Type *at = checkArgBorrow(c, slot, p->type);
                Expr  *a  = *slot;

                /* When the parameter is a `ref T`, the call site has to spell `ref` out, so
                 * that a reader sees at a glance that a reference is passed and not a copy. An
                 * argument that already is a reference can be passed as it stands. */
                if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, p->type));
                    continue;
                }
                /* `coroutine<T>` is a uniform handle, a plain 24-byte value, so any place
                 * that takes a value takes it. */
                checkAssignable(c, p->type, at, a, "argument");
                /* A by-value argument of an `@value` type owns its storage: a place has to be
                 * copied through `clone`, or two values would share one storage and the callee's
                 * parameter would release what the caller still holds. */
                if (ckValueCopyNeedsClone(c, p->type, a)) planSetArgClone(a, true);
                ckOwningArgMoveOnly(c, p->type, a, "argument");
            }
            /* Every call site is checked, not only the ones with a home arena: what matters is
             * that the callee may store into a container the argument points at, which has
             * nothing to do with the home arena.
             * The position matters as well: after the arguments are checked, because
             * `targetDepth` reads their types. */
            checkCallRefArgs(c, f, &e->u.call.args, &f->params, anHomeDepth(e), e->line, name);
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_EXT: {
            /* `ext` hands a task to a **domain**, and a domain is a lexical block provided by the library
             * (`sched::run { ... }`, `par::pool(n) { ... }`). Outside a domain there is no receiver, so this is a
             * compile error -- the design point is that it fails here rather than at run time. */
            if (c->domainDepth == 0) {
                ckError(c, e->line,
                        "A domain is a lexical block that says where the concurrency runs and when"
                        " it must all be finished; `ext` hands the task to the nearest one.",
                        "there is no domain here: `ext` is only legal inside `sched::run { … }` or"
                        " `par::pool(n) { … }`");
                return ttError(tt);
            }
            /* Inside a domain: the operand must be a call to a function that **can suspend**, i.e. a
             * coroutine. A plain function has no frame to stop in, so "starting" it would silently
             * be an ordinary call -- exactly the kind of quiet difference this design refuses. */
            Expr *in = e->u.ext_.call;
            if (!in || in->kind != EX_CALL) {
                ckError(c, e->line,
                        "`ext` starts one task, and a task is a call: it needs arguments and a frame"
                        " to hold its progress.",
                        "`ext` takes a call, found something else");
                return ttError(tt);
            }
            Type *inner = checkExpr(c, in);
            (void)inner;
            if (!planCallee(in) || !planIsCoro(planCallee(in))) {
                ckError(c, in->line,
                        "A task must be able to **stop in the middle**: only a coroutine has a frame to"
                        " hold where it stopped and what its locals were. A plain function would just"
                        " run to completion right here, so `ext` on one would be a silent ordinary"
                        " call.",
                        "`ext` needs a coroutine function, and `%s` is a plain function",
                        planCallee(in) ? planCallee(in)->name : "this");
                return ttError(tt);
            }
            planSetExtDom(e, c->curDom);          /* code generation registers the task on this domain */
            planSetIsExtTarget(planCallee(in), true);   /* => emit the domain adapter for it only; other outputs unaffected */
            /* The value is **deliberately void**: `let h = ext f(x)` is rejected by the type system rather than
             * by a special rule; the handle is delivered with the driver loop. */
            e->type = ttVoid(tt);
            return e->type;
        }
        case EX_LAMBDA: return checkLambda(c, e);
        case EX_DYN: {
            /* `dyn Trait(x)` as a **value**: its type is `dyn Trait`, and the payload must
             * implement the trait -- the implementation is what the table points at. The payload's
             * type is kept on the node: codegen needs its C spelling for the temporary and for
             * `sizeof`, and by then this node's own type is `dyn Trait`. */
            Type *payT = checkExprInner(c, e->u.dynv.payload);
            /* The payload node needs a type too: codegen emits a temporary from it, and a node without a type
             * becomes `(int){0}`. */
            if (payT) e->u.dynv.payload->type = payT;
            e->u.dynv.payloadType = payT;
            /* A `dyn` value is backed by a pool, so the enclosing function is marked as pool-making -- the same
             * existing flag the dispatch site uses, and it must be set here too: without it the pool runtime is
             * not emitted and the generated C calls the undeclared `extc_dyn_put`. */
            if (c->curFunc) planSetMakesPool(c->curFunc, true);
            /* The place the site sits in is where the pool belongs: the handle lives because the pool lives, so
             * the pool may not have a shorter lifetime than the value. The rule is the same set pool-making calls
             * use (`setCallZoneArg`): library functions are transparent to the place and leave the choice to the
             * caller, otherwise the current block. On escape, `promoteInto2` raises this tier toward the
             * longer-lived side. */
            setCallZoneArg(c, e);
            dynTraitOf(c, e->u.dynv.traitName, payT, e->line);
            Type *dt = typeNamed(c->arena, e->u.dynv.traitName);
            dt->kind = TY_DYN;
            e->type = dt;
            return dt;
        }

        case EX_METHOD: {
            /* Specifier of an f-string slot: the parser desugars it into this reserved name, and the check picks
             * the pad primitive by the value's static type */
            if (e->u.method.name && strcmp(e->u.method.name, "__extc_pad") == 0)
                return checkPadCall(c, e);
            /* `o.f(x)` where `f` is a **field** holding a `fn`, not a method. The parser reads the
             * shape `receiver.name(args)` as a method call (only a bare `f(x)` becomes EX_CALL), so
             * the field case has to be recognised here -- otherwise the answer is "no method `f` on
             * `op`", a sentence about the wrong thing, on a program that has the field.
             *
             * The field's type is peeked at, never checked: if this turns out to be an ordinary
             * method call (the overwhelmingly common case) the receiver's own checks must run once,
             * and they run on the path below. The node is then rewritten in place into a call
             * through the value, which is what `o.f(x)` means. */
            if (e->u.method.recv->kind == EX_IDENT && lookup(c, e->u.method.recv->u.ident.name)) {
                /* The callee node must be a **real** field access allocated in the arena, not a stack temporary:
                 * it becomes the callee of the rewritten call and is printed by codegen as `(o.f)(x)`; pointing
                 * the call at the *receiver* would print `o`. */
                Expr *fldE = exprNew(c->arena, EX_FIELD, e->line);
                fldE->u.field.obj  = e->u.method.recv;
                fldE->u.field.name = e->u.method.name;
                Type *ft2 = peekFieldType(c, fldE);
                if (ft2 && ft2->kind == TY_FN) {
                    Vec args = e->u.method.args;
                    e->kind = EX_CALL;
                    e->u.call.callee = fldE;
                    e->u.call.args   = args;
                    return checkCallThroughFn(c, e, ft2);
                }
            }

            /* Constructing a variant with a payload can look like this as well: the syntax of
             * `shape.circle(2.0)` is that of a method call, `receiver.name(args)`, and the only
             * difference is that `shape` is a type name rather than a variable. So it is
             * recognised here first, exactly as `status.ok` is recognised on the EX_FIELD path;
             * the receiver's name decides. */
            if (e->u.method.recv->kind == EX_IDENT && !lookup(c, e->u.method.recv->u.ident.name)) {
                Type *et = ttFromName(tt, e->u.method.recv->u.ident.name);
                if (et && et->kind == TY_ENUM && et->edef) {
                    Variant *v = findVariant(et->edef, e->u.method.name);
                    if (!v) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note, "variants of %s:", et->name);
                        for (size_t i = 0; i < et->edef->variants.len; i++)
                            bufPrintf(&note, " %s",
                                      (*(Variant **)vecAt(&et->edef->variants, i))->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` has no variant `%s`", et->name, e->u.method.name);
                        return ttError(tt);
                    }
                    Vec args = e->u.method.args;
                    e->kind = EX_ENUMVAL;
                    e->u.enumval.typeName = et->name;
                    e->u.enumval.variant  = v->name;
                    e->u.enumval.args     = args;
                    planSetAssocOwner(e, et);
                    return checkExprInner(c, e);
                }
            }

            /* `f.call(x)` where `f` holds a `fn(...)` pointer: `.call` is the spelling a closure
             * uses, so it is accepted here too and means the same thing -- a call through the
             * pointer. Recognised *before* the receiver is checked, so the call path owns the one
             * and only check of it. */
            if (e->u.method.name && strcmp(e->u.method.name, "call") == 0) {
                Type *ct = peekCallableType(c, e->u.method.recv);
                /* A *concrete* `fn(...)` means a call through the pointer. A type parameter may
                 * instantiate to a closure, whose `call` is a real method: rewriting that here would
                 * turn it back into a call expression and the two rewrites would chase each other. */
                if (ct && ct->kind == TY_FN) {
                    Expr *recv = e->u.method.recv;      /* read before `u.call` overwrites them */
                    Vec   args = e->u.method.args;
                    e->kind = EX_CALL;
                    e->u.call.callee = recv;
                    e->u.call.args   = args;
                    return checkExprInner(c, e);
                }
            }
            /* Methods live inside a `struct` body only, so they are found by the type of the
             * receiver. */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            /* An integer literal as a method receiver: the `21` in `21.doubled()` has no type of its own yet, so
             * if the default width (i32) has no such method, try the widths it fits into (the same rule
             * `findOpLiteral` applies to operator overloads). Whichever width matches, the literal takes that
             * type. */
            if (!f && e->u.method.recv->kind == EX_INT && ttIsInteger(rb)) {
                static const char *methodRecvCands[] = {"i64", "u32", "i16", "u16", "i8", "u8"};
                for (size_t ci = 0; ci < sizeof methodRecvCands / sizeof methodRecvCands[0]; ci++) {
                    Type *cand = ttFromName(tt, methodRecvCands[ci]);
                    if (!cand || ttIsError(cand)) continue;
                    FuncDef *m = findMethod(cand, e->u.method.name);
                    if (!m) continue;
                    f = m;
                    rb = cand;
                    recvT = cand;
                    e->u.method.recv->type = cand;
                    break;
                }
            }
            if (f && f->isPrivate) {
                const char *home = c->curFunc ? c->curFunc->modName : NULL;
                if (f->modName != home) {
                    ckError(c, e->line,
                            "`@private` members are reachable only from the module that declares"
                            " them; if this is meant to be public, drop the annotation.",
                            "`%s` is private to module `%s`", e->u.method.name,
                            f->modName ? f->modName : "this file");
                    return ttError(tt);
                }
            }
            if (!f && ttBase(recvT) && ttBase(recvT)->kind == TY_ANY) {
                /* A method call on a dynamically typed value: which `m` runs is decided by the
                 * value's own descriptor when the program runs, so no `FuncDef` is resolved here.
                 * Three consequences worth spelling out:
                 *
                 *   - the result is `any`: the callee's return type cannot be named from here, and
                 *     the caller unboxes it like any other dynamic value (an implicit conversion
                 *     that traps if the belief is wrong);
                 *   - the only compile-time check is the **monotonic** one -- the name has to exist
                 *     somewhere in the program. Checking signatures here would let a later change
                 *     (one more type with that name, one more overload) break a program that
                 *     already compiles, which is exactly what this feature must not do;
                 *   - arguments are not supported yet: boxing them is a separate step, and an
                 *     unimplemented shape should say so rather than silently ignore the argument. */
                if (e->u.method.args.len != 0) {
                    ckError(c, e->line,
                            "A dynamic call looks the method up by name at run time; passing"
                            " arguments through that lookup is not implemented yet.",
                            "a method call on `any` cannot take arguments yet");
                    return ttError(tt);
                }
                bool known = false;
                for (size_t i = 0; i < c->m->structs.len; i++) {
                    StructDef *cand = *(StructDef **)vecAt(&c->m->structs, i);
                    for (size_t k = 0; k < cand->methods.len; k++) {
                        FuncDef *md = *(FuncDef **)vecAt(&cand->methods, k);
                        if (md->params.len == 1 && strcmp(md->name, e->u.method.name) == 0) {
                            /* Keep **every** candidate alive: reachability prunes methods nobody
                             * calls by name, and a dynamic site is not a static one. Missing this
                             * leaves the dispatcher calling a function that was never generated. */
                            planSetUsed(md, true);
                            known = true;
                        }
                    }
                }
                if (!known) {
                    ckError(c, e->line,
                            "A dynamic call resolves the name against the types in the program, so"
                            " some type has to declare that method.",
                            "no type in this program defines `%s` with no arguments",
                            e->u.method.name);
                    return ttError(tt);
                }
                /* The boxed result needs an arena that outlives the call site. */
                planSetDynCall(e, true);
                planSetArenaLevel(e, ARENA_HOME);
                *(Expr **)vecPush(&c->curArenaSites) = e;
                e->type = tt->tAny;
                return e->type;
            }
            if (!f) {
                /* The `d` in `d.m(...)` is a `dyn Trait` value: the method lives in the **trait's** declaration,
                 * not in any type's method set -- the concrete type is only known at run time. This is placed
                 * here (after the receiver has been checked), so every receiver shape works (`d`, `h.d`, ...) and
                 * the receiver node already carries a type for codegen; reading its type without checking the
                 * receiver would scramble the order of this pass. */
                if (ttBase(recvT) && ttBase(recvT)->kind == TY_DYN) {
                    /* The receiver may be reached through a reference (`r.tag()` with
                     * `r: ref dyn Tag`). `ttBase` unwraps it here, so record the fact for codegen:
                     * it has to dereference the C expression when it hands the handle to
                     * `extc_dyn_slot`. */
                    planSetDynRecvViaRef(e, (recvT && recvT->kind == TY_REF));
                    const char *traitName = ttBase(recvT)->name;
                    TraitDef *tr = NULL;
                    for (size_t i = 0; i < c->m->traits.len && !tr; i++) {
                        TraitDef *cand = *(TraitDef **)vecAt(&c->m->traits, i);
                        if (cand->name && strcmp(cand->name, traitName) == 0) tr = cand;
                    }
                    FuncDef *want = NULL;
                    for (size_t i = 0; tr && i < tr->methods.len && !want; i++) {
                        FuncDef *cand = *(FuncDef **)vecAt(&tr->methods, i);
                        if (strcmp(cand->name, e->u.method.name) == 0) want = cand;
                    }
                    /* Object safety: a `dyn` call goes through the trait's uniform table, and the table hands the
                     * receiver to the thunk, so a method with **no receiver** has no slot to sit in.
                     * `want->params.len == 0` is rejected here, the same rule as returning `Self`; that way the
                     * diagnostic can name the trait method instead of leaving the call to codegen written around a
                     * `params[0]` receiver (the table's slot decides it). */
                    if (want && want->params.len == 0) {
                        ckError(c, e->line,
                                "A `dyn` value dispatches through the trait's uniform table, and that"
                                " table hands a receiver to every entry. A method with no receiver has"
                                " no place in it -- make it a free function, or give it a receiver.",
                                "`%s` has no receiver, so it cannot be called through `dyn`",
                                e->u.method.name);
                        return ttError(tt);
                    }
                    if (!want) {
                        ckError(c, e->line,
                                "A `dyn` value dispatches through the trait it names, so the method"
                                " has to be one the trait declares.",
                                "trait `%s` has no method `%s`", traitName, e->u.method.name);
                        return ttError(tt);
                    }
                    /* Arguments come against the trait's declaration; a parameter mentioning `Self`
                     * is skipped -- the implementing type is deliberately unknown here, and object
                     * safety keeps such methods out of the table. */
                    size_t wantArgs = want->params.len > 0 ? want->params.len - 1 : 0;
                    if (e->u.method.args.len != wantArgs) {
                        ckError(c, e->line,
                                "The trait's declaration says how many arguments the method takes.",
                                "`%s` takes %zu argument(s), but %zu were given",
                                e->u.method.name, wantArgs, e->u.method.args.len);
                        return ttError(tt);
                    }
                    for (size_t i = 0; i < e->u.method.args.len; i++) {
                        Expr *a = *(Expr **)vecAt(&e->u.method.args, i);
                        Param *wp = *(Param **)vecAt(&want->params, i + 1);
                        /* The parameter type given by the trait declaration is the context: literal arguments
                         * (`d.bump(10)`) take their type from it, and `checkInto` handles the "only implicit
                         * widening" half of the check. */
                        Type *wantT = mentionsParam(wp->type) ? NULL : wp->type;
                        if (wantT) adoptContextType(a, wantT);
                        Type *at = wantT ? checkInto(c, wantT, a) : checkExprInner(c, a);
                        /* Same rule as an ordinary call: literal arguments may land in the declared type
                         * (`d.bump(10)`), and everything else only allows lossless conversion -- rather than
                         * demanding bit-for-bit equal types. */
                        if (!mentionsParam(wp->type))
                            (void)checkAssignable(c, ttBase(wp->type), ttBase(at), a, "argument");
                    }
                    if (c->curFunc) planSetMakesPool(c->curFunc, true);
                    e->u.method.recv->type = ttBase(recvT);
                    planSetCallee(e, want);
                    e->dynTrait = traitName;
                    e->type = want->ret ? want->ret : ttVoid(tt);
                    return e->type;
                }
                /* A method call on a type parameter: `T` is opaque inside a template, and which method this is
                 * only becomes known per instance. Record it and hand back the error type (the error type exists
                 * precisely so that errors do not cascade; `checkAssignable` lets it through), and let the
                 * instance pass decide and report. Without this, a generic container could do nothing with a
                 * value of its element type -- `hashMap<K, V>` is stuck exactly here: hashing a `K` means calling
                 * `hash()` on a type parameter. */
                if (c->curFunc && mentionsParam(recvT)) {
                    /* The arguments are checked here even though the callee is not known yet: what
                     * they mean does not depend on the instance, and the rest of the pass walks this
                     * tree afterwards (unused parameters, escapes, the arena sites), so their C names
                     * have to exist. Checking them again per instance is the same double work the
                     * deferred operators already cost. */
                    for (size_t i = 0; i < e->u.method.args.len; i++)
                        (void)checkExprInner(c, *(Expr **)vecAt(&e->u.method.args, i));
                    MethodCheck *mc = (MethodCheck *)arenaAllocZero(c->arena, sizeof(MethodCheck));
                    mc->node  = e;
                    mc->owner = c->curFunc->owner;
                    mc->name  = e->u.method.name;
                    mc->nargs = e->u.method.args.len;
                    mc->func  = c->curFunc;
                    *(MethodCheck **)vecPush(&c->methodChecks) = mc;
                    return ttError(tt);
                }
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                /* An `impl` block in a module that was not imported is the usual reason a
                 * method is "missing": say which module declares it (stdlib/INDEX). */
                const char *provider = modulesMethodHint(e->u.method.name);
                if (sd) {
                    bufPrintf(&note, "methods of %s:", DN(sd));
                    if (sd->methods.len == 0) bufPuts(&note, " (none)");
                    for (size_t i = 0; i < sd->methods.len; i++)
                        bufPrintf(&note, " %s",
                                  (*(FuncDef **)vecAt(&sd->methods, i))->name);
                    bufPuts(&note, "; declare it inside its `struct`, or attach it with an"
                                  " `impl` block");
                } else {
                    bufPuts(&note, "only struct values and builtin scalars have methods");
                }
                if (provider)
                    bufPrintf(&note, "; method `%s` is declared by module `%s` -- add `use %s`",
                              e->u.method.name, provider, provider);
                ckError(c, e->line, bufCstr(&note), "no method `%s` on `%s`",
                        e->u.method.name, typeStr(c, rb ? rb : recvT));
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* code generation keys off this flag, not e->func (NULL here) */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* Signature: `(id: i64, lo: i64, hi: i64, views...)`, where **exactly one** `mut` view is the
                 * output (partitioned over [lo,hi)) and the rest are **read-only** views (every worker gets the
                 * same one, no partitioning). */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Expr *an = *(Expr **)vecAt(&e->u.call.args, 1 + nViews);
                    Expr *ax = *(Expr **)vecAt(&e->u.call.args, 2 + nViews);
                    Type *tn = checkExpr(c, an);
                    Type *tx = checkExpr(c, ax);
                    /* A bare integer literal is an i64 in this position (the argument has no
                     * other context to say otherwise), so `parallel::run(w, out, n, 2)` does not
                     * need the `i64(...)` wrapper. Anything else still has to be i64. */
                    if (!(isI64Type(tn) || (an->kind == EX_INT && literalFits(an, c->tI64))) ||
                        !(isI64Type(tx) || (ax->kind == EX_INT && literalFits(ax, c->tI64)))) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);
                planSetParTlsArena(wf, true);
                planSetUsed(wf, true);
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);
            /* Method callers are recorded per instance too (free functions have no owner, and the helper skips
             * them itself). */
            if (e->kind == EX_METHOD)
                ckNoteInstUse(f, e->u.method.recv->type);
            /* `dyn Trait(x).m(...)`: **object safety**.
             *
             * Not every method can be dispatched through the table; the rule is enforced at the **use site** of
             * `dyn`, not at the trait declaration: such a method is perfectly fine as a static call, it simply
             * has no slot to sit in. The decision is about the **trait's** signature, so the trait is looked up by
             * name among the module's declarations (the same lookup that assigns `impl`s). */
            if (e->dynTrait) {
                /* A `dyn` value is backed by a pool, so the enclosing function makes a pool. This uses the
                 * **existing** flag: codegen enters a place for the body and emits the pool runtime accordingly;
                 * the decision must be made in the checker, because "emit `zoneEnter`" is settled before the body
                 * is generated, and setting the flag halfway through body generation is too late. */
                if (c->curFunc) planSetMakesPool(c->curFunc, true);
                TraitDef *tr = dynTraitOf(c, e->dynTrait, e->u.method.recv->type, e->line);
                FuncDef *want = NULL;
                for (size_t ti = 0; tr && ti < tr->methods.len && !want; ti++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&tr->methods, ti);
                    if (strcmp(cand->name, e->u.method.name) == 0) want = cand;
                }
                /* Object safety. These three are FATAL on purpose: a `dyn` call dispatches
                 * through a uniform table whose every entry receives a `self`, so a trait method
                 * with no receiver (an empty `f->params`) has no slot to sit in. */
                bool objSafeBad = false;
                if (want && !funcIsMethod(want)) {
                    ckError(c, e->line,
                            "A table holds method pointers, so the trait method it names must take"
                            " a receiver; an associated function is still callable statically.",
                            "`%s::%s` has no `self`, so it cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                } else if (want && want->typeParams.len > 1) {
                    ckError(c, e->line,
                            "A generic method has one implementation per instantiation, so there is"
                            " no single slot for it in a table.",
                            "`%s::%s` is generic and cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                } else if (want && want->ret && mentionsParam(want->ret)) {
                    ckError(c, e->line,
                            "A method returning `Self` would have to name the implementing type,"
                            " which a table slot cannot do.",
                            "`%s::%s` returns `Self` and cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                }
                if (objSafeBad) return ttError(tt);
            }

            /* A method that takes `self: mut ref T` needs a writable receiver. This is the other
             * half of making signatures tell the truth: `x.bump()` alone does not show whether
             * `x` will be modified, `self: mut ref` in the signature does, and this is where that
             * promise is enforced. */
            if (f->params.len > 0) {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type->kind == TY_REF && selfP->type->mut &&
                    requireMutable(c, e->u.method.recv, e->line, "call a method that writes"))
                    return ttError(tt);
            }

            /* The receiver is the shallowest `mut ref` argument, since `self` comes first, so
             * what the callee allocates follows the arena the receiver lives in. */
            if (f->params.len > 0) {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type && selfP->type->kind == TY_REF && selfP->type->mut) {
                    int d = slotDepth(c, e->u.method.recv);
                    /* The `self: mut ref` branch does not go through `callHomeDepth`, so the escape decision has to
                     * be made here too. */
                    const char *rrn = placeRootName(e->u.method.recv);
                    if (d != 0 && rrn && isEscapeeName(c, rrn)) d = -1;
                    anSetHomeDepth(e, (d == 0) ? -1 : d);
                    /* This receiver branch depends on the escape information as well, and that
                     * information is not final while the body is being checked: it is complete
                     * only once the closure of the call graph is closed. So the site is recorded
                     * and recomputed at the end.
                     * The shape that needs it:
                     *     fn fill(out) { var l: list  l.pushOne(7)  out.take(l) }
                     * Here `l` is published by a method call afterwards, so the receiver has to be
                     * treated as escaping. */
                    /* Recorded for a receiver at any depth, `d == 0` included: the arena is
                     * settled then, but the **zone** is not (it waits for the `makesPool`
                     * closure), and the final pass needs the site to promote it. */
                    if (rrn && c->eSites.arena) {
                        EArenaSite *rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                        rec->call = e;
                        rec->argRoot[0]  = (char *)rrn;      /* the receiver is argument 0 */
                        rec->argDepth[0] = (d == 0) ? 0 : d;
                        rec->n = 1;
                        *(EArenaSite **)vecPush(&c->eSites) = rec;
                    }
                } else {
                    anSetHomeDepth(e, callHomeDepth(c, &e->u.method.args, &f->params, e));
                }
                /* Resolve which arena the call finally passes; both numbers are decided here. */
                setCallArenaArg(c, e);
                setCallZoneArg(c, e);       /* pool-creating call: the layer the pool is allocated in */
                /* Whichever way the argument was a local or a parameter, the depth of the thing pushed into the
                 * container must be recorded. */
                /* A method call has to be checked like every other call site: the contents flow does not depend on
                 * the home arena.
                 *
                 * The receiver counts as argument 0. The callee's `params[0]` is `self`, while `e->u.method.args`
                 * does not contain the receiver, so the two lengths differ and the `params->len != args->len`
                 * guard at the top of `checkCallRefArgs` would return early without a word -- and the reference
                 * rules would then not apply to method calls at all. Prepending the receiver below fixes
                 * that. */
                (void)0;   /* the check is moved below, after the arguments; see there */
            }

            /* When the receiver is a generic instance, `T` in the method signature must be substituted with the
             * type arguments; and when the **method itself** is generic too (`fn cast<U>(self: ref box, u: U)`),
             * its own parameters are inferred from the arguments just like a free generic function
             * (`unifyTParams`, the same call as on the `EX_CALL` path; the method's own parameters included). */
            StructDef *msd = structOf(rb);
            Vec *sp = NULL, *sa = NULL;
            Vec  mp, ma;
            vecInit(&mp, c->arena, sizeof(const char *));
            vecInit(&ma, c->arena, sizeof(Type *));
            if (rb && rb->kind == TY_GENERIC && msd) {
                sp = &msd->typeParams;
                sa = &rb->targs;
            }
            if (f->typeParams.len) {
                Vec mt;
                vecInit(&mt, c->arena, sizeof(Type *));
                for (size_t i = 0; i < f->typeParams.len; i++)
                    *(Type **)vecPush(&mt) = NULL;
                for (size_t i = 0; i < e->u.method.args.len && i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i + 1);
                    Expr  *arg = *(Expr **)vecAt(&e->u.method.args, i);
                    Type  *want = ttSubstitute(tt, p->type, sp, sa);
                    /* Container arguments are lent as read-only views before inference (the `va` in
                     * `s.append(va)`). */
                    if (lendViewArg(c, want, (Expr **)vecAt(&e->u.method.args, i)))
                        arg = *(Expr **)vecAt(&e->u.method.args, i);
                    Type  *got  = checkExprInner(c, arg);
                    (void)unifyTParams(tt, &f->typeParams, &mt, want, got);
                }
                bool missing = false;
                for (size_t i = 0; i < mt.len; i++) {
                    if (*(Type **)vecAt(&mt, i)) continue;
                    ckError(c, e->line,
                            "A generic method's type parameters are inferred from its arguments;"
                            " give an argument whose type mentions the parameter.",
                            "cannot infer type parameter `%s` of `%s`",
                            *(const char **)vecAt(&f->typeParams, i), e->u.method.name);
                    missing = true;
                    break;
                }
                if (missing) return ttError(tt);
                /* The method's own parameters come **after** the receiver's, so the parallel lists
                 * the substitutions below use cover both. */
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    *(const char **)vecPush(&mp) = *(const char **)vecAt(&f->typeParams, i);
                    *(Type **)vecPush(&ma) = *(Type **)vecAt(&mt, i);
                }
                if (sp) {
                    for (size_t i = 0; i < sp->len; i++) {
                        *(const char **)vecPush(&mp) = *(const char **)vecAt(sp, i);
                        *(Type **)vecPush(&ma) = *(Type **)vecAt(sa, i);
                    }
                }
                sp = &mp;
                sa = &ma;
            }

            size_t want = f->params.len - 1;
            Type *rt = f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);

            {
                size_t need = minArgsFor(f, 1);
                if (e->u.method.args.len < need || e->u.method.args.len > want) {
                    if (need == want)
                        ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                                e->u.method.name, want, e->u.method.args.len);
                    else if (need == 0)
                        ckError(c, e->line, NULL, "`%s` expects at most %zu argument(s), got %zu",
                                e->u.method.name, want, e->u.method.args.len);
                    else
                        ckError(c, e->line, NULL, "`%s` expects %zu to %zu argument(s), got %zu",
                                e->u.method.name, need, want, e->u.method.args.len);
                    return rt;
                }
                fillDefaultArgs(c, &e->u.method.args, f, 1);
            }
            for (size_t i = 0; i < want; i++) {
                Param *p = *(Param **)vecAt(&f->params, i + 1);
                Expr  *a = *(Expr **)vecAt(&e->u.method.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);

                /* A view parameter (`append(s: slice<u8>)`) accepts a container argument of the same element, and
                 * also accepts "expression results" such as a value f-string. */
                Expr **slot = (Expr **)vecAt(&e->u.method.args, i);
                lendViewArg(c, pt, slot);
                Type *at = checkArgBorrow(c, slot, pt);
                a = *slot;
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* `coroutine<T>` is a uniform handle, a plain 24-byte value, so any place
                 * that takes a value takes it. */
                checkAssignable(c, pt, at, a, "argument");
                /* A method argument follows the same `@value` rule as an ordinary call: the
                 * callee's parameter owns what it is given and drops it on the way out. */
                if (ckValueCopyNeedsClone(c, pt, a)) planSetArgClone(a, true);
                ckOwningArgMoveOnly(c, pt, a, "argument");
            }
            /* The rule is checked only after the arguments have been checked: before that `a->type` is not filled
             * in yet and `typeContainsRef` says "no" to everything, so violations would slip through silently.
             * The receiver is prepended as argument 0, because the callee's `params[0]` is `self`. */
            {
                Vec margs; vecInit(&margs, c->arena, sizeof(Expr *));
                *(Expr **)vecPush(&margs) = e->u.method.recv;
                for (size_t ai = 0; ai < e->u.method.args.len; ai++)
                    *(Expr **)vecPush(&margs) = *(Expr **)vecAt(&e->u.method.args, ai);
                checkCallRefArgs(c, f, &margs, &f->params, anHomeDepth(e),
                                 e->line, e->u.method.name);
            }
            return rt;
        }

        case EX_STRUCTLIT: {
            /* The type comes from the name written in the literal or from the context; a generic
             * instance can only come from the context.
             *
             * A written name goes through `ttResolve` with the current context, exactly the way a
             * type annotation does, so a name the module system mangles (`box` -> `box$box`)
             * resolves as written. The message for a name that really is unknown comes from
             * `ttResolve`, which also reports the ambiguous case (`pair` exported by two
             * modules). The literal's own fields are then looked up in the resolved struct. */
            Type *st = e->type;
            if (e->u.lit.name &&
                !(st && structLitNameMatches(structOf(st), e->u.lit.name))) {
                st = ttResolve(tt, c->ctx, typeNamed(c->arena, e->u.lit.name), e->line, c->curParams);
                if (!st || ttIsError(st)) return ttError(tt);
            }
            StructDef *sd = structOf(st);
            if (!sd) {
                if (e->u.lit.name)
                    ckError(c, e->line, NULL, "`%s` is not a struct", e->u.lit.name);
                else
                    ckError(c, e->line,
                            "a bare `{}` works only where the context pins the type down: "
                            "`var x: T = {}` / `return {}` / `f({})`",
                            "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            if (st->kind == TY_STRUCT && sd->typeParams.len > 0) {
                ckError(c, e->line, "a generic needs explicit type arguments, e.g. `Pair<i32, i32> { ... }`",
                        "`%s` is generic; type arguments cannot be inferred here", DN(sd));
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "fields of %s:", DN(sd));
                    for (size_t k = 0; k < sd->fields.len; k++)
                        bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, k))->name);
                    ckError(c, fi->value->line, bufCstr(&note),
                            "struct `%s` has no field `%s`", DN(sd), fi->name);
                    continue;
                }
                /* Writing a private field is as much a breach as reading one: a literal that can
                 * set a container's storage from outside lets anyone plant a fabricated pointer
                 * there. Assignment goes through the field path, which is checked; a literal does
                 * not, so it needs its own check. */
                {
                    const char *home = c->curFunc ? c->curFunc->modName : NULL;
                    if (fd->isPrivate && sd->modName != home) {
                        ckError(c, fi->value->line,
                                "Construct the value through the type's own constructor: storage"
                                " is the type's business.",
                                "field `%s` of `%s` is private to module `%s`", fd->name, DN(sd),
                                sd->modName ? sd->modName : "this file");
                        continue;
                    }
                }
                Type *want = fd->type;
                if (st->kind == TY_GENERIC)
                    want = ttSubstitute(tt, want, &sd->typeParams, &st->targs);

                adoptContextType(fi->value, want);
                Type *vt = checkInto(c, fd->type, fi->value);
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "field `%s`", fi->name);
                checkAssignable(c, want, vt, fi->value, bufCstr(&what));
                /* A field of an `@value` type owns its storage: a literal that copies a place
                 * into it would leave two owners, so the copy is built through `clone`. */
                if (ckValueCopyNeedsClone(c, want, fi->value)) planSetArgClone(fi->value, true);
                ckOwningArgMoveOnly(c, want, fi->value, "field");
                /* A plain name written into a field of an owning type is **transferred**, the
                 * same rule a payload follows: the literal owns the storage, the name is dead
                 * from here on. The generator marks the move so the name's own drop does not
                 * free the storage twice. */
                if (!planArgClone(fi->value) && fi->value->kind == EX_IDENT && payloadOwnsStorage(c, want, 0)) {
                    Sym *srcSym = lookup(c, fi->value->u.ident.name);
                    if (srcSym && srcSym->movedLine == 0) {
                        srcSym->movedLine = e->line;
                        srcSym->movedInto = sd->name;
                    }
                }
            }

            /* Omitted fields are filled with the zero value, but `ref` has no zero value: leaving it alone would
             * emit `.field = 0`, that is, a null reference, while the language says `ref` is never null. */
            for (size_t i = 0; i < sd->fields.len; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
                bool given = false;
                for (size_t k = 0; k < e->u.lit.inits.len && !given; k++)
                    given = strcmp((*(FieldInit **)vecAt(&e->u.lit.inits, k))->name, fd->name) == 0;
                if (given) continue;

                Type *ft = fd->type;
                if (st->kind == TY_GENERIC)
                    ft = ttSubstitute(tt, ft, &sd->typeParams, &st->targs);
                if (typeLacksZeroValue(tt, ft))
                    ckError(c, e->line,
                            typeContainsFn(tt, ft)
                                ? "A `fn` field is a code pointer, and this language has no null"
                                  " one: give it explicitly, or declare the field as"
                                  " `option<fn …>`."
                                : "`ref` has no default value (it is a non-nullable reference)",
                            "field `%s` must be given explicitly", fd->name);
            }
            return st;
        }
    }
    return ttError(tt);
}

/* Declared here because the two walks below recurse through it. */
static bool funcMayPrint(Checker *c, FuncDef *f);

static bool stmtMayPrint(Checker *c, Stmt *s);

/**
 * @brief Tell whether evaluating an expression prints, directly or through a call.
 *
 * @param c  Checker.
 * @param e  Expression to look at; NULL counts as "does not print".
 * @return   True when a `print` / `println` call is reachable from here.
 */
static bool exprMayPrint(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return true;
        if (strcmp(n, "flush") == 0) return false;      /* flushing is not printing */
    }
    switch (e->kind) {
    /* The payload of `dyn Trait(x)` is a subexpression (`dynv.payload`), and every walker has to look into it
     * -- the walkers all end in `default:`, so `-Wswitch` will not point out the missing branch. */
    case EX_DYN: return exprMayPrint(c, e->u.dynv.payload);
    case EX_CALL:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        if (exprMayPrint(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_ASSOC:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    case EX_BIN:   return exprMayPrint(c, e->u.bin.left) || exprMayPrint(c, e->u.bin.right);
    case EX_UN:    return exprMayPrint(c, e->u.un.operand);
    case EX_FIELD: return exprMayPrint(c, e->u.field.obj);
    case EX_INDEX: return exprMayPrint(c, e->u.index.obj) || exprMayPrint(c, e->u.index.index);
    case EX_SLICE: return exprMayPrint(c, e->u.slice.obj) || exprMayPrint(c, e->u.slice.lo) ||
                          exprMayPrint(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprMayPrint(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprMayPrint(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF:   return exprMayPrint(c, e->u.ref.operand);
    case EX_EXT: return exprMayPrint(c, e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprMayPrint(c, e->u.deref.operand);
    case EX_SIGN:  return exprMayPrint(c, e->u.sign.operand);
    case EX_CONV:  return exprMayPrint(c, e->u.conv.operand);
    case EX_TRY:   return exprMayPrint(c, e->u.try_.operand);
    case EX_NEW:   return exprMayPrint(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE:
        return exprMayPrint(c, e->u.coalesce.main) || exprMayPrint(c, e->u.coalesce.fallback);
    default: return false;
    }
}

/**
 * @brief Tell whether executing a statement prints, directly or through a call.
 *
 * @param c  Checker.
 * @param s  Statement to look at; NULL counts as "does not print".
 * @return   True when a `print` / `println` call is reachable from here.
 */
static bool stmtMayPrint(Checker *c, Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMayPrint(c, s->u.yield_.value);
    case ST_TRAP:   return true;      /* it writes a line to stderr itself */
    case ST_DROP:   return false;     /* the hook is a plain method; a hook that prints is a call */
    case ST_VAR:    return exprMayPrint(c, s->u.var.init);
    case ST_ASSIGN: return exprMayPrint(c, s->u.assign.target) || exprMayPrint(c, s->u.assign.value);
    case ST_EXPR:   return exprMayPrint(c, s->u.expr.expr);
    case ST_RETURN: return exprMayPrint(c, s->u.ret.value);
    case ST_IF:     return exprMayPrint(c, s->u.ifs.cond) ||
                           stmtMayPrint(c, s->u.ifs.thenBody) || stmtMayPrint(c, s->u.ifs.elseBody);
    /* `d.run { … }`: the receiver is an expression, the block is a body -- walk both. */
    case ST_DOMAIN: return exprMayPrint(c, s->u.domain_.callee) || stmtMayPrint(c, s->u.domain_.body);
    case ST_WHILE:  return exprMayPrint(c, s->u.whiles.cond) || stmtMayPrint(c, s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtMayPrint(c, *(Stmt **)vecAt(&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH:
        if (exprMayPrint(c, s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtMayPrint(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    default: return false;
    }
}

/**
 * @brief Tell whether a function prints, directly or through the calls it makes, with a cycle guard.
 *
 * `FuncDef.callees` is only filled in once a body has been checked, so it cannot be used while a body is
 * being checked; this walks the callee AST instead, which needs its own cycle guard. An unresolvable
 * callee and a function inside a recursive cycle both count as printing, the safe direction for the
 * "can this be reordered" question.
 *
 * @param c  Checker.
 * @param f  Function to look at; NULL means the callee could not be resolved.
 * @return   True when a `print` / `println` call is reachable from here.
 */
static bool funcMayPrint(Checker *c, FuncDef *f) {
    if (!f) return true;                       /* unresolved: assume it prints */
    if (anMayPrintState(f) == 1) return true;
    if (anMayPrintState(f) == 2) return false;
    if (anMayPrintState(f) == 3) return true;    /* a cycle: assume it prints */
    anSetMayPrintState(f, 3);
    bool r = stmtMayPrint(c, f->body);
    anSetMayPrintState(f, r ? 1 : 2);
    return r;
}

/**
 * @brief Tell whether calling a function has an observable side effect.
 *
 * Printing, taking a `mut ref` or `mut` view parameter (and so possibly writing an argument), itself
 * reaching such a call, or being unresolvable at all, all count as effectful. A pure getter does not:
 * swapping two getters is unobservable, and judging by "contains a call" would reject programs written
 * with ordinary accessors.
 *
 * @param c  Checker.
 * @param f  Callee; NULL means it could not be resolved.
 * @return   True when reordering this call would change what the program does.
 */
static bool callIsEffectful(Checker *c, FuncDef *f) {
    if (!f) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->type) continue;
        if (p->type->kind == TY_REF && p->type->mut) return true;      /* may write the argument */
        if (p->type->kind == TY_GENERIC && p->type->mut) return true;  /* a `mut` view */
    }
    return funcMayPrint(c, f);
}

/**
 * @brief Tell whether an expression contains a **call** at all, no matter what the effect summary says
 *        (a syntactic question).
 *
 * `exprHasCall` answers the semantic question ("can evaluating twice be observed"), which the `??`
 * ordering rule needs. This one answers a syntactic question, and is deliberately blunt: it guards the
 * only place the compiler has to name an expression twice -- a compound assignment whose operator is a
 * user-defined method (`x += y` becomes `x = add(&x, y)`). There "the summary says it is pure" is not
 * reason enough to duplicate a call: a function body that reads a global but never writes it is pure by
 * the summary, and duplicating it is still wrong. There is one rule with no exceptions: if the target
 * contains a call, reject, and the message tells the user to split the line.
 *
 * @param e  Expression to look at; NULL counts as "has no call".
 * @return   True when a call appears at any depth.
 */
static bool exprHasAnyCall(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* The payload of `dyn Trait(x)` is a subexpression (`dynv.payload`), and every walker has to look into it
     * -- the walkers all end in `default:`, so `-Wswitch` will not point out the missing branch. */
    case EX_DYN: return exprHasAnyCall(e->u.dynv.payload);
    case EX_CALL: case EX_METHOD: case EX_ASSOC: case EX_GENCALL: return true;
    case EX_BIN:   return exprHasAnyCall(e->u.bin.left) || exprHasAnyCall(e->u.bin.right);
    case EX_UN:    return exprHasAnyCall(e->u.un.operand);
    case EX_REF:   return exprHasAnyCall(e->u.ref.operand);
    case EX_EXT: return exprHasAnyCall(e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprHasAnyCall(e->u.deref.operand);
    case EX_SIGN:  return exprHasAnyCall(e->u.sign.operand);
    case EX_CONV:  return exprHasAnyCall(e->u.conv.operand);
    case EX_TRY:   return exprHasAnyCall(e->u.try_.operand);
    case EX_FIELD: return exprHasAnyCall(e->u.field.obj);
    case EX_INDEX: return exprHasAnyCall(e->u.index.obj) || exprHasAnyCall(e->u.index.index);
    case EX_SLICE: return exprHasAnyCall(e->u.slice.obj) || exprHasAnyCall(e->u.slice.lo) ||
                          exprHasAnyCall(e->u.slice.hi);
    case EX_NEW:   return exprHasAnyCall(e->u.new_.count);
    case EX_COALESCE:
        return exprHasAnyCall(e->u.coalesce.main) || exprHasAnyCall(e->u.coalesce.fallback);
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasAnyCall(*(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasAnyCall(*(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasAnyCall((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprHasAnyCall((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    default: return false;      /* literals, bindings, `null` */
    }
}

/**
 * @brief Tell whether an expression contains a call that is **observable when reordered** (the semantic
 *        question).
 *
 * Every switch branch returns a value and `default:` gives `false`, so the walk always has a definite
 * answer: a call with a known absence of side effects (`pure(x)`) is reported as absent, so "there is
 * still a call ahead that was not moved out" is decided by the expression rather than by an uncertain
 * result.
 *
 * @param c  Checker.
 * @param e  Expression to look at; NULL counts as "has no call".
 * @return   True when an effectful call appears at any depth.
 */
static bool exprHasCall(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* The payload of `dyn Trait(x)` is a subexpression (`dynv.payload`), and every walker has to look into it
     * -- the walkers all end in `default:`, so `-Wswitch` will not point out the missing branch. */
    case EX_DYN: return exprHasCall(c, e->u.dynv.payload);
    case EX_CALL: case EX_METHOD: case EX_ASSOC:
        if (callIsEffectful(c, planCallee(e))) return true;
        return false;
    case EX_BIN:      return exprHasCall(c, e->u.bin.left) || exprHasCall(c, e->u.bin.right);
    case EX_UN:       return exprHasCall(c, e->u.un.operand);
    case EX_FIELD:    return exprHasCall(c, e->u.field.obj);
    case EX_INDEX:    return exprHasCall(c, e->u.index.obj) || exprHasCall(c, e->u.index.index);
    case EX_SLICE:    return exprHasCall(c, e->u.slice.obj) || exprHasCall(c, e->u.slice.lo) ||
                             exprHasCall(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasCall(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprHasCall(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF: case EX_DEREF: case EX_SIGN: case EX_CONV: case EX_TRY:
        return exprHasCall(c, e->kind == EX_REF ? e->u.ref.operand :
                              e->kind == EX_DEREF ? e->u.deref.operand :
                              e->kind == EX_SIGN ? e->u.sign.operand :
                              e->kind == EX_CONV ? e->u.conv.operand : e->u.try_.operand);
    case EX_EXT:      return exprHasCall(c, e->u.ext_.call);   /* `ext f(x)`: the spawned call */
    case EX_NEW:      return exprHasCall(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE: return exprHasCall(c, e->u.coalesce.main) || exprHasCall(c, e->u.coalesce.fallback);
    default:          return false;      /* literals, bindings, and `null` */
    }
}

/**
 * @brief Spend one level of the expression-depth budget.
 *
 * A left-associative chain of `n` operands is an AST `n` levels deep, and the passes that follow the
 * checker (the escape side, the data flow, the level pass, the code generator) descend the same tree.
 * Bounding the walk here therefore keeps all of them inside the C stack, and the diagnostic names the
 * constructor the user can act on: splitting the chain.
 *
 * @param c  Checker.
 * @param e  Expression about to be checked, for the diagnostic's line.
 * @return   `true` when `EXPR_DEPTH_LIMIT` is exceeded (the diagnostic has been reported and this
 *           level has been given back); `false` otherwise.
 */
static bool exprTooDeep(Checker *c, Expr *e) {
    if (++c->exprDepth <= EXPR_DEPTH_LIMIT) return false;
    c->exprDepth--;
    ckError(c, e->line,
            "An expression tree this deep comes from a machine, and every pass over it is"
            " recursive: the checker, the escape analysis and the code generator would each"
            " overflow the C stack. Split the chain with intermediate bindings.",
            "expression too deep to check (the compiler's limit is %d levels)", EXPR_DEPTH_LIMIT);
    return true;
}

/**
 * @brief Check an expression and write its type back onto the node, without dereferencing references.
 *
 * This is the entry point the rest of the checker uses; the result is also left on `e->type`. When the
 * expression contains a call, the statement-effect flag is raised so `??` can decide whether a temporary
 * has to be taken first. The flag is raised by the expression being evaluated, not by a `??` subject:
 * its temporary is hoisted together with the other temporaries in source order, so their relative order
 * does not change.
 *
 * @param c  Checker.
 * @param e  Expression; NULL yields the error type directly.
 * @return   The expression's type; the error type when it cannot be determined, so callers never see an
 *           empty type.
 */
Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    if (exprTooDeep(c, e)) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    c->exprDepth--;
    /* `!` is deliberately not included here: its operand is an argument to an unwrap helper, the calls inside
     * it execute in place, and the flag is raised as usual. */
    if (exprHasCall(c, e) &&
        !(e->kind == EX_COALESCE && planNeedTemp(e))) c->stmtFx = 1;
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

