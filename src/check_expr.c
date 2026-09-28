/* Expression checking: the type of every expression, and the checks that only the
 * shape of an expression can decide.
 *
 * This is the recursive core of the checker. It resolves names to bindings, rewrites a
 * few nodes in place (a variant access becomes an enum value, a generic call becomes a
 * plain call), and decides the arena level of every allocation site. The lifetime and
 * borrow rules themselves live in check_escape.c.
 */

#include "modules.h"   /* modulesMethodHint: "no method" says which module to import */
#include <stdlib.h>
#include "check_internal.h"

/* The inclusive range of a **builtin integer** type, by name.
 *
 * Used by the constant-fold check below: two integer literals are folded by the C compiler, and C's
 * constant folding is exactly where gcc rejects the result (`int32_t n = (2000000000 + 2000000000);`
 * is an error under `-Werror`, even though `-fwrapv` defines the runtime behaviour). */
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
 * `parallel::run(worker, out, n, threads)` 是 stdlib/std/parallel.extc 里用 `@builtin` 声明的内建：
 * 语言没有一等函数值，所以"把 worker 放到线程上跑"这件事必须由编译器生成一个 trampoline 去调它。
 *
 * ③a 的语义（与库文件头一致）：区间 [0,n) 分块，worker 拿到自己的 [lo,hi)；**`out` 按 [lo,hi) 切成
 * 互不重叠的一段**交给每个 worker ⇒ 读只共享、写必分区 ⇒ 不需要新的可发送性分析。
 *
 * ③b（本文件的语法白名单，保守但可靠）：worker 体内只许算符、字面量、下标、它自己的参数与局部；
 * 任何调用（含用户函数与 stdlib）、`new`、以及既非参数又非局部的名字（模块级 `var` 就在这一档）
 * 一律拒绝。传递性（worker 调用户辅助函数）与线程本地 arena 的路由是 ③c。 */
static bool isParRunDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "parallel$run") == 0;
}
/* `sys::domain::single()`: the domain layer's one entry point. Recognised the way
 * `parallel::run` is -- mangled builtin name plus its shape -- not by module path, so a user function
 * named `single` is untouched (it is not marked `@builtin`). */
static bool isDomSingleDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "domain$single") == 0 &&
           f->ret && f->ret->kind == TY_STRUCT && f->ret->name &&
           strcmp(f->ret->name, "domain") == 0;
}
static bool isI64Type(Type *t) {
    return t && t->kind == TY_BUILTIN && t->name && strcmp(t->name, "i64") == 0;
}
typedef struct { Vec names; const char *why; int depth; Checker *c; } ParQ;
static bool parQHas(ParQ *q, const char *n);
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth);
static bool parLocalStmt(void *ctx, Stmt *s);
static bool parBodyExpr(void *ctx, Expr *e);
static bool parBodyStmt(void *ctx, Stmt *s);

/* 收局部名（`let`/`var`）。 */
static bool parLocalStmt(void *ctx, Stmt *s) {
    ParQ *q = (ParQ *)ctx;
    if (s->kind == ST_VAR && s->u.var.name) *(const char **)vecPush(&q->names) = s->u.var.name;
    AstVisit v = { NULL, parLocalStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
static bool parBodyStmt(void *ctx, Stmt *s) {
    AstVisit v = { parBodyExpr, parBodyStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
static bool parBodyExpr(void *ctx, Expr *e) {
    ParQ *q = (ParQ *)ctx;
    if (e->kind == EX_IDENT && !parQHas(q, e->u.ident.name)) {
        if (!q->why) q->why = "it names something that is neither a parameter nor a local of the worker";
        return false;
    }
    /* `new` 现在放行（③c 完成）：worker 链上每个函数都带 parTlsArena ⇒ arenaRefAt 给
     * `(*extc_tls_arena)`；而 worker 体内分配会让它多一个隐藏的 home 尾参，trampoline 补传的也是
     * 同一个线程本地 arena。安全性来自签名：只能写 out、只能返回 i64 ⇒ 分配逃不出去。 */
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        FuncDef *g = e->func;
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

#define PAR_DEPTH_LIMIT 16      /* 递归深度上限：宁可拒绝，也不让检查自己爆栈（记忆化留给以后） */
static bool parQHas(ParQ *q, const char *n) {
    for (size_t i = 0; i < q->names.len; i++)
        if (strcmp(*(const char **)vecAt(&q->names, i), n) == 0) return true;
    return false;
}
/* 一个函数能不能出现在 worker 里（含**传递**）：
 *   · 语法上只碰自己的参数与局部（模块级 `var` 既非参数也非局部 ⇒ 自然落在这一档 ⇒ 不用另做全局分析）；
 *   · 不 `new`（线程本地 arena 的路由是 ③c）；
 *   · 调用只能是：签过 `Thread=0` 的 extern，或另一个同样通过这一关的顶层函数。
 * 递归带深度上限；超限按"不安全"处理（保守方向）。 */
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth) {
    ParQ q;
    if (depth > PAR_DEPTH_LIMIT) return "the call chain is too deep to check (conservative refusal)";
    if (!f->body) return NULL;                       /* extern/内建：由调用点的规则管 */
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
    f->parTlsArena = true;        /* 这条链上的每个人：`new` 都走线程本地 arena（③c） */
    return NULL;
}

static bool exprHasCall(Checker *c, Expr *e);   /* defined at the end of this file */
static bool exprHasAnyCall(Expr *e);            /* the syntactic question; see its comment */

/* Record that an operator applied to a type parameter has to be re-checked per instance.
 *
 * On a template `T` is opaque, so whether `T` supports an operator is only answerable once
 * the type argument is known. Every overloadable operator goes through this one record --
 * `==` was the first family, and every other overloadable operator joined it -- and the
 * consumer (runOpCheck) re-asks the predicate the concrete-type path uses. The
 * record is deliberately not written in three places: the three would drift, and the drift
 * would show up as one operator accepting what another rejects.
 *
 * Params:
 *   c  - checker
 *   e  - the EX_BIN node carrying the operator
 *   op - the operator, which decides what the instance has to provide
 *
 * Notes:
 *   - Outside a function body there is nothing to defer to: a global initializer is
 *     required to be a constant, and the comparison there is reported on its own terms.
 */
static void deferOp(Checker *c, Expr *e, const char *op) {
    e->needOp = true;
    if (!c->curFunc) return;
    OpCheck *oc = (OpCheck *)arenaAllocZero(c->arena, sizeof(OpCheck));
    oc->node  = e;
    oc->op    = op;
    oc->func  = c->curFunc;
    oc->owner = c->curFunc->owner;   /* NULL for a free function */
    *(OpCheck **)vecPush(&c->opChecks) = oc;
}

/* The two hidden arguments of an overloadable operator, resolved exactly as `EX_METHOD` does.
 *
 * An operator IS a method call: the left operand is the receiver and the right one is the
 * single argument, and the callee can allocate (`usesHome`) or create a pool (`makesPool`)
 * just like any method. So the call site owes it the same hidden arguments, and the decision
 * of *which* arena/zone belongs to the same two functions the method path uses.
 *
 * The arena half is still open (PLAN #83: an operator that allocates and returns the value
 * needs the escape-site bookkeeping as well), but the ZONE half cannot wait: `push`/`append`
 * on an SSO string create the pool on the way out of the inline buffer, which makes the
 * string-reading operators (`io >> string`, `ifstream >> string`) pool-creating callees.
 * Without this the declaration carries `int64_t __extc_home_zone` and the call site passes
 * two arguments -- the generated C does not compile at all.
 *
 * `setCallZoneArg` fixes the provisional level (the current block in a program, "pass my own
 * zone down" inside a library module); `callHomeDepth` is then given the two operands so the
 * escape site is recorded -- the final pass lowers the level again when an operand escapes
 * this frame (`makesPool` is a closure, so it is still false here, exactly as on the method
 * path: the record is unconditional and the final pass filters on the flag).
 *
 * Params:
 *   c - checker
 *   e - the EX_BIN node carrying the operator
 *   m - the operator method the checker resolved for this node
 */
static void setOpCallArgs(Checker *c, Expr *e, FuncDef *m) {
    setCallZoneArg(c, e);
    if (!m || !c->eSites.arena) return;
    /* Only a `mut ref` operand can be published into, so only then is there an escape site
     * to record: `==` / `<` take both operands by `ref` and record nothing. */
    bool anyMut = false;
    for (size_t i = 0; i < m->params.len && i < 2; i++) {
        Param *p = *(Param **)vecAt(&m->params, i);
        if (p->type && p->type->kind == TY_REF && p->type->mut) anyMut = true;
    }
    if (!anyMut) return;
    /* An operator has exactly two parameters and two operands, in the same order
     * (`checkOperatorSig` is what keeps it that way), so the operands can be handed to the
     * method path's own recorder without a second copy of the depth/escape rules. */
    Vec args;
    vecInit(&args, c->arena, sizeof(Expr *));
    *(Expr **)vecPush(&args) = e->u.bin.left;
    *(Expr **)vecPush(&args) = e->u.bin.right;
    (void)callHomeDepth(c, &args, &m->params, e);
}

/* Compute the result type of a binary arithmetic operator.
 *
 * Params:
 *   c  - checker
 *   e  - the EX_BIN node being checked, for its operator and source position
 *   lt - type of the left operand
 *   rt - type of the right operand
 *
 * Returns:
 *   The common type of the two operands, or the error type after a diagnostic.
 *
 * Notes:
 *   - A literal operand adapts to the other side, so `u32 + 1` has type `u32`.
 *   - `%` is restricted to integers here.
 *   - The result rule lives only in this function on purpose: the bitwise operators use
 *     it too, and a second copy would be free to drift away from it.
 *   - Reference-typed operands never reach this function; the caller reports them first.
 */
static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;

    /* A user-defined arithmetic operator, tried before the numeric rule so that a struct
     * operand reaches its own method rather than "arithmetic operators only accept
     * numeric types". Same protocol as `==`: a method whose name is the operator, its
     * signature already validated where it was defined (checkOperatorSig).
     *
     * The two operand types have to match exactly. extC never converts implicitly at an
     * overloaded operator, so `vec2 * f64` is not a candidate here -- that
     * wants a named method of its own, and inventing the conversion would make the
     * meaning of `*` depend on the overload set. */
    /* Heterogeneous operands are allowed here: an operator may be defined once per
     * right-operand type (`vec2 * f64`), and it is matched on that type exactly. */
    if (isArithOp(op)) {
        StructDef *sd = structOf(ttBase(lt));
        if (sd) {
            FuncDef *m = findOp(c->tt, ttBase(lt), op, rt, NULL);
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
            e->func = m;
            m->used = true;      /* record that this method is used */
            setOpCallArgs(c, e, m);   /* the hidden zone argument of an operator call */
            return lt;
        }
        /* Defer when **either** operand is a type parameter, not just the left one. Deferring only
         * on the left made `s + c.value()` an error the moment a coroutine was driven from inside a
         * generic function: `value()` returns the template's `T`, which arrives on the **right**
         * (`cannot apply `+` to `i64` and `T`, tools/attack.py B8). The instance decides what the
         * operands really are; the result is pinned to the parameter either way. */
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

    /* Two integer **literals** and an arithmetic operator: the fold happens in C, and C's constant
     * folding is where gcc rejects it -- `int32_t n = (2000000000 + 2000000000);` is
     * "integer overflow in expression" and an error under `-Werror`, even though `-fwrapv` defines
     * what the runtime would do. The author's decision is to **report it here**, where the language
     * can say what to write (`i64(...)`), because wrapping would spread boundary conditions through
     * the whole language (tools/attack.py W15/W16 -- found by "can extC do `let n = 1 + 2` now?"). */
    if ((strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0) &&
        e->u.bin.left->kind == EX_INT && e->u.bin.right->kind == EX_INT &&
        ttEquals(lt, rt) && ttIsInteger(lt)) {
        long long lo; unsigned long long hi;
        if (intLitRange(lt, &lo, &hi)) {
            long long a = e->u.bin.left->u.ival;
            long long b = e->u.bin.right->u.ival;
            bool over = false;
            long long r = 0;
            /* Detect first, compute second: `a + b` may itself overflow `long long`, which would be
             * undefined. A result outside `long long` is outside every one of these types anyway. */
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
            /* The upper bound is unsigned (`u64` does not fit in `long long`), so only cast a
             * **non-negative** result: casting `-1` produced a huge unsigned value and made every
             * negative constant look like an overflow (caught by examples/generic-free-fn.extc,
             * whose `return 0 - 1` is perfectly fine). */
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

    /* A literal adapts to the other operand by value: in `u32 + 1` the `1` is a u32. */
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
/* Resolve the operator of a compound assignment (`x += y`).
 *
 * The statement is kept as it was written and the operation is checked here, by the very
 * function a written-out `x + y` goes through (`checkArith`): the arithmetic rule, literal
 * fitting, reference rejection and a user-defined operator are all the same question, and
 * asking it twice in two places is how the two answers drift apart. The result is the
 * `EX_BIN` that answered it, which codegen needs when the operator turned out to be a
 * method.
 *
 * Params:
 *   c      - checker
 *   op     - the compound operator as written (`"+="`); its first character is the binary
 *            one, because all five are one character long
 *   target - the left side, already checked
 *   value  - the right side, already checked
 *   tgt    - the type of the target
 *   val    - the type of the value
 *   out    - receives the resolved `EX_BIN`
 *
 * Returns:
 *   The type of `target op value`, or an error type when the operator does not apply.
 *
 * Notes:
 *   - A user-defined operator takes its receiver as a reference, so the generated call has
 *     to name the target a second time (`x = add(&x, y)`). That is only harmless when the
 *     target is a place that can be evaluated twice without doing anything, so a target
 *     containing a call is refused with the instruction to split the line. Builtin
 *     operators do not have this problem: C's `x += y` evaluates the target once.
 */
Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                      Type *tgt, Type *val, Expr **out) {
    Expr *bin = exprNew(c->arena, EX_BIN, target->line);
    bin->u.bin.op    = arenaStrndup(c->arena, op, 1);   /* `+=` asks about `+` */
    bin->u.bin.left  = target;
    bin->u.bin.right = value;
    Type *rt = checkArith(c, bin, tgt, val);
    if (out) *out = bin;
    if (ttIsError(rt)) return rt;
    if (bin->func && exprHasAnyCall(target)) {
        ckError(c, target->line,
                "the statement is rewritten as `x = add(&x, y)`, so the target has to be a"
                " place that can be named twice without side effects",
                "the target has a call in it -- split it into two lines: `var t = ...`"
                " then `t %s ...`", op);
        return ttError(c->tt);
    }
    return rt;
}


/* Report whether `op` is one of the bitwise operators `&`, `|`, `^`, `<<`, `>>`.
 *
 * The result type of these operators comes from `checkArith`, so the rule is not copied
 * here and the two cannot drift apart.
 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

/* Report whether a type is a reference, nullable or not. */
static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/* Report an arithmetic or comparison operator applied to a reference.
 *
 * In C this would silently become pointer arithmetic, which is not what the user wrote.
 * Reporting an error is preferred over an answer that looks right and moves a pointer.
 *
 * Params:
 *   c  - checker
 *   e  - the EX_BIN node being checked, for its source position
 *   lt - type of the left operand
 *   rt - type of the right operand
 *   op - the operator spelling, for the message
 *
 * Returns:
 *   The error type, so the caller can return it directly.
 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

/* Check one expression and return its type.
 *
 * Params:
 *   c - checker
 *   e - the expression node
 *
 * Returns:
 *   The type of the expression, or the error type once a diagnostic was reported.
 *
 * Notes:
 *   - This is the recursive core; `checkExpr` wraps it with the per-statement
 *     bookkeeping that every subexpression needs.
 *   - Some nodes are rewritten in place while they are checked, so the caller must not
 *     rely on `e->kind` staying what it was on entry.
 */
/* `poolSlice<T>(rid, n) -> mut slice<T>` . `poolResize<T>(rid, s, n) -> mut slice<T>` .
 * `poolGive<T>(rid, s) -> bool`: the typed door into a **pool's own plate**
 * (POOLS.md 2.1 "a pool has two faces", PLAN #85).
 *
 * The `...Raw` spellings of the first two are the same call minus the zeroing of the bytes
 * they hand out, for a column whose every slot is written before it is read (`map`'s four
 * stores, `vector`/`string`'s buffer, `hashMap`'s `keys`/`slot`/`bucketOfDense`). Zeroing
 * such a column is pure extra work: it writes bytes that are about to be overwritten and
 * commits pages that may never be used -- measured on `bench/stl` as -17.5% RSS for `map`,
 * -10.8% for `string`, -8.0% for `hashMap`, with byte-identical output. What must keep the
 * zeroing: `hashMap`'s `tag` (an empty bucket IS zero), and `pool`'s own `ent` (its `pFree`
 * reads `low == 0` as free). `tests/pool/run.sh` pins the whole question with two canaries
 * (poisoned blocks must not change a program's output; memcheck must not report more
 * uninitialized reads than the recorded baseline).
 *
 * Why this cannot be an `extern!` in the library: `extern!` may not return `slice<T>` (it
 * would become two C parameters), and extC has no pointer casts, so the library cannot turn
 * a block of bytes into a typed view. The generator can, so the generator does -- the same
 * reason `alloc` is a primitive rather than a library function.
 *
 * `poolResize` is the one `grow` uses, and the reason is the peak: take + copy + give holds
 * the old and the new block at the same time (measured 1.9x the live size), while resizing
 * in place stays near it.
 *
 * No arena level is recorded on purpose. This memory is not the arena's: the pool owns it,
 * and it goes back when the block is returned (`poolGive`) or when the pool is released. The
 * escape rules therefore have nothing to say about it here -- "a reference may not outlive
 * the place" is enforced for pool memory by the container's own discipline (POOLS.md 5.4:
 * only handles leave). */
static Type *checkPoolPrim(Checker *c, Expr *e, Type *elem) {
    TypeTable *tt = c->tt;
    const char *nm = e->u.gencall.name;
    /* `copyInto<T>(dst: mut slice<T>, src: slice<T>, n) -> i64`: one checked `memmove`.
     * It shares this door because the generator has to name the element type, exactly like the
     * pool primitives -- and there is no other way to write a bulk move in the language (a
     * loop pays the bounds check per element; the containers would also pay their stale-pool
     * guard per element, which measured 1 cycle per byte in bench/app scenario C). */
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
    bool isTake    = strcmp(nm, "poolSlice") == 0 || strcmp(nm, "poolSliceRaw") == 0;
    bool isResize  = strcmp(nm, "poolResize") == 0 || strcmp(nm, "poolResizeRaw") == 0;
    size_t want    = isResize ? 3 : 2;
    if (e->u.gencall.args.len != want) {
        ckError(c, e->line, NULL, "`%s` takes %d arguments (the pool, %s)", nm, (int)want,
                isTake   ? "and how many elements"
              : isResize ? "the block, and how many elements it should hold"
                         : "and the block to give back");
        return ttError(tt);
    }
    Expr *ridE = *(Expr **)vecAt(&e->u.gencall.args, 0);
    Type *ridT = checkValue(c, ridE);
    if (!ttIsError(ridT) && !ttIsInteger(ridT)) {
        ckError(c, ridE->line, NULL, "the pool is named by its integer id, found `%s`",
                typeStr(c, ridT));
        return ttError(tt);
    }
    Expr *argE = *(Expr **)vecAt(&e->u.gencall.args, 1);
    Type *argT = checkValue(c, argE);
    if (isTake) {
        if (!ttIsError(argT) && !ttIsInteger(argT)) {
            ckError(c, argE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, argT));
            return ttError(tt);
        }
    } else if (isResize) {
        if (!ttIsError(argT)) {
            Type *se = viewElemOf(argT);
            if (!se || !ttEquals(se, elem)) {
                ckError(c, argE->line, NULL, "`%s` resizes an existing block: `%s`, not `%s`",
                        nm, typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, argT));
                return ttError(tt);
            }
        }
    }
    if (isTake || isResize) {
        Expr *nE = isResize ? *(Expr **)vecAt(&e->u.gencall.args, 2) : argE;
        Type *nT = isResize ? checkValue(c, nE) : argT;
        if (!ttIsError(nT) && !ttIsInteger(nT)) {
            ckError(c, nE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, nT));
            return ttError(tt);
        }
        /* `T` has no size while a template is checked, exactly as in `new T[n]`, so the size
         * check is deferred to instantiation and recorded there. */
        if (elem->kind == TY_PARAM || ttHasParam(elem)) recordNewSizeCheck(c, elem, e->line);
        /* The count appears twice in the emitted C (once for the bytes, once for `.len`), so
         * an impure count is computed into a temporary first -- the same rule as `new T[n]`. */
        if (!repeatablePure(nE)) e->needTemp = true;
        return ttViewMut(tt, sliceOf(c, elem), true);
    }
    if (!ttIsError(argT)) {
        Type *se = viewElemOf(argT);
        if (!se || !ttEquals(se, elem)) {
            ckError(c, argE->line, NULL,
                    "`poolGive` needs the block itself: `%s`, not `%s`",
                    typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, argT));
            return ttError(tt);
        }
    }
    return c->tBool;
}

/* Resolve the trait a `dyn` form names, and insist the payload implements it.
 *
 * One helper for both users -- `dyn Trait(x)` (a value) and `dyn Trait(x).m(...)` (the immediate
 * form) -- because it is one rule, and the `impl Trait for T` records phase 1 kept are what answer
 * it. Returns the trait, or NULL after reporting. */
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
    /* The uniform tables are emitted only for a trait that is actually dispatched through -- this
     * is also what keeps an object-unsafe method (no receiver, generic, or returning `Self`) from
     * having to appear in one. */
    tr->usedDyn = true;
    c->m->usesDyn = true;
    Type *pt = ttBase(payT);
    bool impl = false;
    for (size_t i = 0; i < c->m->impls.len && !impl; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&c->m->impls, i);
        /* `impl<T> Tag for pair<T>` covers **every** instance of `pair`, so the declaration being
         * the same is what counts -- comparing the types themselves never matches `pair<i64>`
         * against `pair<T>`, which is what rejected `dyn Tag(p)` with "`pair_i64` does not
         * implement `Tag`" (tools/attack.py F1). */
        Type *it = im->target ? ttBase(im->target) : NULL;
        impl = im->trait == tr && it && pt &&
               (ttEquals(it, pt) || (it->sdef && it->sdef == pt->sdef));
        /* A **generic** impl (`impl<T> Tag for pair<T>`) is matched by declaration, and its methods
         * are shared by every instance -- so mark them used here: that is what makes the per-instance
         * emission loop (`codegen.c`, "method definitions of the instances") write `pair_i64_tag`.
         * Without it the `dyn` table pointed at a function nobody emitted
         * (`extc_vt$Tag$pair_i64` undeclared, tools/attack.py F1). */
        if (impl && it && it->sdef && it->kind == TY_GENERIC)
            for (size_t k = 0; k < im->methods.len; k++) {
                FuncDef *mf = *(FuncDef **)vecAt(&im->methods, k);
                mf->used = true;
                mf->dynTable = true;   /* its body is named by the vt thunk, from outside */
            }
    }
    if (pt && pt->sdef && !impl)
        ckError(c, line, "A `dyn` value names a trait its payload implements, because the"
                " implementation is what the table points at.",
                "`%s` does not implement `%s`", pt->name, traitName);
    return tr;
}


/* ---------------------------------------------------------------- lambda literals --------------
 *
 * docs/topics/LAMBDA.md, the author's v2 decisions: syntax B (`fn(x: i64) -> i64 { ... }`),
 * monomorphized parameters -- so no function pointers enter the language -- and **writable captures
 * spelled out** in `[mut ref x]`, because a read is captured by value and needs no entry.
 *
 * The order below is the whole design:
 *
 *   1. the body is checked **in place**, in a scope that holds the lambda's parameters, so every
 *      name in it is resolved by the ordinary rules -- shadowing included, which a name-only
 *      pre-pass would get wrong;
 *   2. what the body then refers to from *outside* that scope is exactly the capture set: it is
 *      read off the resolved `Sym`s, not guessed from the text;
 *   3. each capture becomes a field of a generated struct -- by value (a copy, so the closure sees
 *      the value it was made with) or, for `mut ref x`, a reference to the enclosing variable;
 *   4. the body becomes that struct's `call` method, and each captured name is emitted as
 *      `self.<field>` (`(*self.<field>)` for a writable one). That last step needs no AST surgery:
 *      codegen prints an identifier through its `cname`, so the rewrite is a `cname` assignment.
 *
 * `f(x)` where `f` holds such a value is a call through a value, which v1 has no syntax for; the
 * call site rewrites itself in place into the method call `f.call(x)` -- the same rewriting the
 * enum-construction branch of EX_CALL already does.
 */
typedef struct {
    Checker *c;
    size_t   nOuter;      /* scopes below this index belong to the enclosing functions */
    Vec      caps;        /* Sym*: the capture set, in declaration order */
    Vec      written;     /* Sym*: the captured variables the body writes */
    bool     usedNew;
    bool     sawReturn;
} LamWalk;

static void lamAddUnique(Vec *v, Sym *s) {
    if (!s) return;
    for (size_t i = 0; i < v->len; i++)
        if (*(Sym **)vecAt(v, i) == s) return;
    *(Sym **)vecPush(v) = s;
}

/* Is this binding declared below the lambda's own scope? Membership in the scope stack is checked
 * by pointer: `Sym.depth` is the *arena* depth and answers a different question. */
static bool lamIsOuter(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->nOuter && i < w->c->scopes.len; i++) {
        Scope *sc = *(Scope **)vecAt(&w->c->scopes, i);
        for (size_t j = 0; j < sc->syms.len; j++)
            if (*(Sym **)vecAt(&sc->syms, j) == s) return true;
    }
    return false;
}

static bool lamWritten(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->written.len; i++)
        if (*(Sym **)vecAt(&w->written, i) == s) return true;
    return false;
}

/* The root binding of an assignment target: `x[3].f = ...` writes through `x`. */
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

static bool lamCapListed(Expr *lam, const char *name) {
    for (size_t i = 0; i < lam->u.lambda.captures.len; i++)
        if (strcmp((*(LamCap **)vecAt(&lam->u.lambda.captures, i))->name, name) == 0) return true;
    return false;
}

/* Step 4: rewrite the captured names to the environment's fields. */
typedef struct { Checker *c; LamWalk *w; Vec *fields; } LamRw;   /* fields: const char*, per cap */
static bool lamRwExpr(void *ctx, Expr *e);
static bool lamRwStmt(void *ctx, Stmt *s);

static bool lamRwExpr(void *ctx, Expr *e) {
    LamRw *r = (LamRw *)ctx;
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        for (size_t i = 0; i < r->w->caps.len; i++) {
            if (*(Sym **)vecAt(&r->w->caps, i) != sy) continue;
            const char *fn = *(const char **)vecAt(r->fields, i);
            /* A writable capture is a reference: `(*self->c0)` is an lvalue, so a read and a write
             * through it both come out right, and the node's type never changes. */
            e->u.ident.cname = arenaPrintf(r->c->arena,
                                           lamWritten(r->w, sy) ? "(*self->%s)" : "self->%s", fn);
            break;
        }
    }
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkExprChildren(e, &v);
}
static bool lamRwStmt(void *ctx, Stmt *s) {
    LamRw *r = (LamRw *)ctx;
    if (!s) return true;
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkStmtChildren(s, &v);
}

static Type *checkExprInner(Checker *c, Expr *e);   /* the struct-literal path is re-entered below */

static Type *checkLambda(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

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
    vecInit(&sd->typeParams, c->arena, sizeof(const char *));
    vecInit(&sd->fields, c->arena, sizeof(FieldDef *));
    vecInit(&sd->methods, c->arena, sizeof(FuncDef *));
    Type *lt = arenaAllocZero(c->arena, sizeof *lt);
    lt->kind = TY_STRUCT;
    lt->name = lname;
    lt->sdef = sd;
    vecInit(&lt->targs, c->arena, sizeof(Type *));

    /* The `call` method exists **before** the body is checked: a `return` inside a lambda belongs to
     * the lambda, and the checker reads the return type off `curFunc` (check_stmt.c). With no `->`
     * written, `ret` stays NULL while the body is checked and is filled in from the first `return`
     * below -- the same freedom a named function has in the syntax. */
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
    /* A declared function's signature is resolved before its body is checked; a synthesized one has
     * to go through the same step or its parameter types stay the parser's raw nodes -- measured as
     * "cannot apply `+` to `i64` and `i64`". */
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
        sd->lamSig = bufCstr(&sig);
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
    /* With no `->` written the result type is decided by the first `return <value>` in the body; the
     * ST_RETURN case fills it in (check_stmt.c). */
    cf->lamInferRet = (cf->ret == NULL);
    checkStmt(c, e->u.lambda.body);
    cf->lamInferRet = false;
    c->curFunc = saveCur;
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

    /* Every writable capture is written down in the source (the author's decision) -- which is also
     * what the escape and arena passes read: every writable alias is visible in the program text. */
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
            fd->type = sy->type;             /* a copy: the closure sees the value it was made with */
        }
        *(FieldDef **)vecPush(&sd->fields) = fd;
        *(const char **)vecPush(&fields) = fd->name;

        Expr *id = exprNew(c->arena, EX_IDENT, e->line);
        id->u.ident.name = sy->name;
        id->u.ident.cname = sy->cname;
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
    cf->lamChecked = true;                   /* checked above, in the scope it was written in */
    *(FuncDef **)vecPush(&sd->methods) = cf;
    /* The struct has to be reachable from the module: codegen collects struct types and the methods
     * to emit from `m->structs` (codegen.c). */
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

static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:
            /* The literal's own type is the smallest integer that holds it. It used to be i32
             * unconditionally, so a value that does not fit was truncated silently wherever no
             * context supplied a type -- an unannotated `let n = 5000000000` inside a generic body
             * became 32-bit (PLAN #92). SPEC §3.4 says only lossless widening is implicit and
             * there is no implicit truncation; contexts that want another integer type still type
             * the literal themselves through `literalFits` / `adoptContextType`. */
            if (e->u.ival < -2147483648LL || e->u.ival > 2147483647LL) return c->tI64;
            return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_NULL: {
            /* The type was parked on `e->type` by `adoptContextType`. If the context did
             * not say which `?ref T` this stands for, report it rather than guess: extC is
             * explicit instead of inferring. */
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
            if (s && s->modName && !e->qualified)
                requireQualified(c, e->u.ident.name, s->modName, false, e->line);
            if (!s) {
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
                ckError(c, e->line, "every name must be declared first (extC has no globals yet)",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* Name resolution is frozen here: codegen prints `cname` directly. This is
             * what tells shadowed names apart (`a` from `a__2`). */
            e->u.ident.cname = s->cname;
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

            /* `<<` and `>>` take their left operand as a **receiver**, not as a value:
             * the first `>>` of `fin >> x >> line` returns `mut ref fin`, and the second
             * one continues from that very object, so the value-position rule ("a
             * reference is not a value, write `*p`") must not fire on this operand
             * (docs/DECISIONS.md 88). Every other operator keeps the rule. */
            bool shiftRecv = op0[0] == '<' || op0[0] == '>';
            Type *lt = shiftRecv ? checkExpr(c, e->u.bin.left) : checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

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

                    e->func = m;  m->used = true;   /* record that this method is used */
                    setOpCallArgs(c, e, m);         /* the hidden zone argument of an operator call */
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
                    e->func = m;
                    m->used = true;      /* record that this method is used */
                    setOpCallArgs(c, e, m);   /* the hidden zone argument of an operator call */
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
                /* A **reference receiver** is allowed for the stream operators only, and
                 * only when its base type is a struct that defines the operator: that is
                 * what makes `fin >> x >> line` work, because the first `>>` returns
                 * `mut ref ifstream` and the second one continues from that same object
                 * (docs/DECISIONS.md 88). Everything else still reports "reference where a
                 * number belongs", which is the sentence that helps. */
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
                        e->func = m;
                        m->used = true;
                        setOpCallArgs(c, e, m);   /* the hidden zone argument of an operator call */
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
                        e->assocOwner = et;      /* the resolved type; instances need it */
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
             * pool tier closes by never handing one out (T4 / ruling #89). */
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
            e->field = fd;
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
                e->u.field.obj->kind == EX_IDENT && e->u.field.obj->u.ident.cname) {
                const char *rc = e->u.field.obj->u.ident.cname;
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

            /* A type that defines `[]` gets the subscript syntax: `m[k]` becomes the method call it
             * is, and the whole method machinery -- argument checks, return type, the deferred
             * check for a generic instance (#57) -- applies without a second copy of it here. The
             * built-in array / slice / view path below is what remains for every other type. `[]`
             * and `[]=` are independent names: this is the read side only, an assignment through a
             * `[]`-only type is reported by the assignment path (`[]=` is where writing goes). */
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

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, NULL,
                        "cannot index a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            /* Error recovery for the index: its own failure was reported where it came from, and
             * `self.tag[i]` is still a `u8` whatever happened to `i`. Propagating the error here
             * poisoned every later use of the element, and a poisoned operand makes the checks
             * that follow return early -- so their deferral records were never written and code
             * generation emitted the wrong operator. That is how a deferred method call (#57)
             * cost the comparison below it its `==` method: `var i = k.hash() & mask` made `i`
             * an error type, `self.keys[i] == k` was waved through as "native", and the C
             * compiler was the first to notice, on two structs (PLAN #79). */
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
             * may exist now, so a write through the binding can no longer be treated as the
             * only write to that storage.
             * The narrowing facts recorded for paths through it are dropped at the same
             * time: `h.p` being non-null is no longer reliable. */
            { Sym *rs = placeRoot(c, op); if (rs) { unNarrow(c, rs->cname); rs->addressed = true; } }
            if (ttIsError(ot)) return ttError(tt);

            if (ot->kind == TY_REF) {
                ckError(c, e->line, "it is already a reference -- just pass it as is",
                        "`ref` applied to a value that is already a reference");
                return ot;
            }
            if (!isLvalue(op)) {
                ckError(c, e->line, "only variables and fields can be referenced",
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
                    "`?` has to expand into statements (evaluate once, test, return), "
                    "so it only fits where a whole statement can be rewritten.",
                    "`?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?`");
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
            e->assocOwner = t;

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
                    if (m->isAssoc && strcmp(m->name, e->u.assoc.name) == 0) { f = m; break; }
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
                        if (!m->isAssoc) continue;
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
                    e->domNew = true;          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
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
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区 ⇒ 不需要
                 * 新的可发送性分析）。 */
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
                /* 视图实参逐个对齐类型：`mut` 形参必须收到 `mut` 实参（它要被写）；只读形参两种都收
                 * （可变的东西只读地用是安全的）。 */
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
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                e->parWorker = wf;
                wf->isParWorker = true;   /* codegen 据此生成 trampoline */
                wf->parTlsArena = true;   /* worker 里的 new 走线程本地 arena（③c） */
                wf->used = true;          /* the trampoline names it, so it must be emitted */
                {
                    /* 第一个实参不是表达式，是函数名：换成一个 i64 字面量，后面的实参照常走。 */
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* An associated function needs its arena argument computed too: it may
             * allocate, or it may return a reference. Omitting this generated a call with one
             * argument too few, so `Type::make()` did not compile. */
            e->homeDepth = callHomeDepth(c, &e->u.assoc.args, &f->params, e);
            setCallArenaArg(c, e);      /* resolve which arena the call finally passes */
            setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
            /* The reference checking is moved to the end of this case, after the arguments
             * have been checked. */

            if (e->u.assoc.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s::%s` expects %zu argument(s), got %zu",
                        e->u.assoc.typeName, f->name, f->params.len, e->u.assoc.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                Expr  *a = *(Expr **)vecAt(&e->u.assoc.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);
                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. ",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* A coroutine value cannot cross a function boundary yet: the argument's type is
                 * the `coroutine<T>` marker, while codegen materialises the coroutine's own frame.
                 * Loud beats a C type mismatch. The uniform representation (a boxed handle) lands
                 * with slice C, which is what a scheduler needs anyway. */
                /* Legal now: `coroutine<T>` is the unified handle, a plain 24-byte value. */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, pt, at, a, "argument");
                }
            }
            /* The reference check applies to an associated function as well, once its
             * arguments have been checked. */
            checkCallRefArgs(c, f, &e->u.assoc.args, &f->params, e->homeDepth,
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
            Type *t = ttFromName(tt, e->u.conv.typeName);
            if (!t || t->kind != TY_BUILTIN) {
                ckError(c, e->line, NULL, "`%s` is not a scalar type", e->u.conv.typeName);
                return ttError(tt);
            }
            e->u.conv.type = t;
            Type *src = checkValue(c, e->u.conv.operand);
            if (ttIsError(src)) return ttError(tt);
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
             * so it is not checked either. Everything else -- narrowing, a sign change, float
             * to integer -- is checked. */
            bool lossless = (si && ti && ttCanWiden(src, t)) || (sf && tf);
            e->convCheck = !lossless;
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

            /* Depth is the depth of the current block: the same number the generated C uses
             * to pick `__extc_a[k]`.
             *
             * The exception is a function with a home arena, one that hands its own
             * allocations to the caller. What it allocates then lives as long as the scope the
             * caller chose, so the site is recorded as `ARENA_HOME`. In `refDepth` terms that
             * reads as 0, the level outside this frame, which is the level the escape check
             * lets through; this is what makes `return h` in
             * `fn build() -> mut ref node` legal.
             *
             * This level is the starting point, not the final one. When the escape check finds
             * the new object stored somewhere longer-lived, it promotes `arenaLevel` to that
             * level (`promoteInto` in check_escape.c), and `refDepth` follows `arenaLevel`,
             * because the two must always be the same number.
             *
             * The same node may be checked twice, so the level is only assigned on the first
             * visit and only ever moved towards a longer life. */
            /* The checker decides the level and codegen only translates it. A function with
             * a home arena gives `ARENA_HOME`; everything else goes by block, except that
             * storage marked `@overwrite` has to survive every round of the loop, so it lives
             * in this frame, at level 1.
             *
             * `needsHome` is read as it stands at this moment, which covers the direct
             * evidence only. A function that gains a home arena because of a call it makes is
             * decided again by `checkModule`, using `FuncDef.arenaSites`, once the closure is
             * complete. */
            if (e->arenaLevel == 0)
                e->arenaLevel = (c->curFunc && c->curFunc->needsHome) ? ARENA_HOME
                              : (e->reuse ? 1 : (int)c->scopes.len);
            /* Keep the lexical level in its own field: the branch above may have replaced
             * `arenaLevel` with the `ARENA_HOME` sentinel, while the solver still needs to
             * know which block the site started in. */
            if (e->lexicalLevel == 0)
                e->lexicalLevel = e->reuse ? 1 : (int)c->scopes.len;
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame". */
            e->minAt = -1;
        *(Expr **)vecPush(&c->curArenaSites) = e;
            /* `refDepth` follows `arenaLevel`: the two must always be the same number. The
             * only exception is `ARENA_HOME`, whose sentinel is -1 while `refDepth` speaks of
             * 0 as "the level outside this frame", so it maps back to 0 -- the home arena is
             * the scope the caller chose, which is depth 0. */
            {
                int depth = arenaDepthOf(e->arenaLevel);    /* the one conversion point */
                if (e->refDepth == 0 || e->refDepth > depth) e->refDepth = depth;
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
            if (!repeatablePure(e->u.new_.count)) e->needTemp = true;
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
            /* `maxOf<i32>(4, 3)`: a free function called with explicit type arguments, which
             * is the only spelling available when `T` appears in the return type alone.
             * Otherwise the old rule stands: only the built-in primitives arrive through this
             * path (`alloc<T>(n)`). */
            /* `alloc<T>(n) -> mut ref T` is the one allocation primitive on this path: a
             * place for n values, reached through a reference.
             *
             * A buffer whose length is known only at run time -- reading a file of unknown
             * size, or the 64 KB the `reader` asks for -- is spelled `new T[n]`, which gives a
             * `mut slice<T>`. There used to be a second primitive for exactly that,
             * `allocSlice<T>(n)`, removed on 2026-09-24: it meant the same thing as `new T[n]`
             * (same view, same level rules, same zeroed memory, plus a second `memset` the
             * arena had already done), so it was a synonym with a weaker story (decision 81). */
            bool isAlloc  = strcmp(e->u.gencall.name, "alloc") == 0;
            /* The pool primitives share this path for the same reason `alloc` does: the
             * library needs memory the compiler has to name the type of. */
            bool isPoolPrim = strcmp(e->u.gencall.name, "poolSlice") == 0
                           || strcmp(e->u.gencall.name, "poolSliceRaw") == 0
                           || strcmp(e->u.gencall.name, "poolResize") == 0
                           || strcmp(e->u.gencall.name, "poolResizeRaw") == 0
                           || strcmp(e->u.gencall.name, "poolGive") == 0
                           || strcmp(e->u.gencall.name, "copyInto") == 0;
            if (!isAlloc && !isPoolPrim) {
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
                    inst->used = true;
                    tf->used = true;
                    /* Rewrite the node into an ordinary call: the shape every path except
                     * `alloc` uses. */
                    Vec args = e->u.gencall.args;
                    Expr *id = exprNew(c->arena, EX_IDENT, e->line);
                    id->u.ident.name = tf->name;
                    e->kind = EX_CALL;
                    e->u.call.callee = id;
                    e->u.call.args   = args;
                    e->qualified     = true;      /* do not trigger the qualification check */
                    e->func = inst;
                    /* Hand the written type arguments to the EX_CALL path below through a side
                     * table: without it that path inferred from scratch, so `f<i64>(x)` was pure
                     * decoration -- it worked only where inference happened to succeed anyway
                     * (`maxOf<i32>(4, 3)`) and failed exactly where the diagnostic recommends it
                     * (`T` in the return type alone, or no argument to infer from:
                     * tools/attack.py B10/B3). */
                    ExplicitTargs *et = (ExplicitTargs *)arenaAllocZero(c->arena, sizeof *et);
                    et->node  = e;
                    et->targs = targs;
                    *(ExplicitTargs **)vecPush(&c->explicitTargs) = et;
                    /* After the rewrite, walk the node again: the argument checks, the
                     * return type, and the home arena all live on the EX_CALL path, so
                     * returning void here instead is wrong. That mistake was made once. */
                    return checkExpr(c, e);
                }
                /* A removed primitive deserves the migration, not "not a built-in": the
                 * old spelling is in examples, in tests, and in code written last week. */
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
                            if (m->isAssoc && strcmp(m->name, "new") == 0) ctor = m;
                        }
                        if (ctor) {
                            /* Every field is read out **before** the union is written: the
                             * two shapes overlap, so writing `u.assoc.typeName` first would
                             * have clobbered `u.gencall.targs` and `u.gencall.args` and the
                             * node would carry a number where a list belongs. The crash was
                             * `obligExpr` following 0x40000000f. */
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
            if (isPoolPrim) return checkPoolPrim(c, e, elem);
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

            /* This memory lives until the end of the current block, because arenas are
             * released per block, so its depth is the depth of that block: `c->scopes.len`.
             *
             * This line is the seam between the depth model and the arena granularity, and both
             * sides have to use the same number: the checker compares block depths to decide
             * whether a reference may be stored here, and the generated C uses the block depth
             * to pick `__extc_a[k]`, which is released when the block ends.
             * Hard-coding 1, as an earlier version did, makes `p` in
             * `while { p = alloc<i32>(1) }` point at freed memory on the next iteration --
             * measured, and exactly the dangling case this has to prevent.
             * As a result, returning freshly allocated memory and storing loop-local allocations
             * outside the loop are both stopped by the escape check. To hand the memory out, the
             * caller has to supply the buffer or the arena. */
            /* The checker decides the level of an `alloc` site, and it has to use exactly the
             * rule `new` uses. The EX_NEW case above reads: a function with a home arena gives
             * `ARENA_HOME`, so the object goes into the arena the caller chose and outlives this
             * frame; otherwise the level is the block level. An earlier version always used the
             * block level, so an `alloc` in a function with a home arena counted as frame-local
             * and returning it was rejected as `1 > 0`, while the same program written with `new`
             * passed -- and the error text told the user to write `new`.
             * Being symmetric with `new` removes that whole family of false rejections: a home
             * arena gives `ARENA_HOME` and depth 0, "the level outside this frame". */
            if (c->curFunc && c->curFunc->needsHome) {
                e->arenaLevel = ARENA_HOME;
                e->refDepth   = 0;
            } else {
                e->refDepth   = c->scopes.len;
                e->arenaLevel = (int)c->scopes.len;
            }
            if (e->lexicalLevel == 0) e->lexicalLevel = (int)c->scopes.len;   /* lexical level */
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame".
             * `alloc` is an allocation site too, so it needs this initialisation as much as
             * `new` does. Without the line, the `inner` of `examples/alloc-in-block` was taken
             * to outlive the frame and was emitted as `__extc_home` although it has no home
             * arena, so the generated C did not compile. */
            e->minAt = -1;
            /* `alloc` has to be registered as an allocation site, the same way `new` is
             * registered in the EX_NEW case above with `vecPush(&c->curArenaSites)`. Without
             * the registration, the rewrite pass that runs after the closure cannot see it, so
             * an `alloc` in a function with a home arena stayed at the block level and was
             * released when the block ended. That produced a family of false rejections:
             * `fn f() -> mut ref i32 { return alloc<i32>(1) }` was rejected while the same
             * program written with `new` passed, and the error text told the user to write
             * `new`. Once registered, a home arena rewrites the site to `ARENA_HOME`, fully
             * symmetric with `new`. */
            *(Expr **)vecPush(&c->curArenaSites) = e;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* freshly allocated storage is writable */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b`: fall back when there is nothing. The meaning is
             * `match a { some(v) => v  _ => b }`, but only one side is evaluated: when a has a
             * value, b is never computed. That is the short circuit of `&&` / `||` again. */
            /* The flag has to be read before the subject is checked, because a subject that is
             * itself a call raises it while it is checked. Reading it afterwards wrongly
             * reported `var a = h() ?? 0` when that is the first statement. */
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

            /* When the subject is not free of side effects, as in `f() ?? -1`, a plain
             * conditional expression will not do: the subject would appear twice in the C and
             * `f()` would run twice. It has to be evaluated into a temporary first, which needs
             * the ability to emit a prefix in front of the enclosing statement. Most positions
             * have it; two do not. */
            /* An impure subject is computed into a temporary, and that prefix is emitted in
             * front of the whole statement, so it would jump ahead of an earlier side effect in
             * the same statement:
             *     `g() + (h() ?? 0)`   reads g first and h second, but h would run first
             * Reordering silently is worse than reporting it and asking for two lines: the order
             * the user reads has to be the order that runs.
             * Only calls count as evidence here: the order of `new`, of literals, and of
             * conversions is unobservable, so those are not reported. */
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
                e->needTemp = true;        /* codegen evaluates it once, then tests the temp */
                /* Every side effect of the subject is hoisted into the prefix, in source order
                 * relative to the other temporaries, so the subject does not count as a call
                 * left in place. Counting it reported two `f() ?? -1` in one `println` that are
                 * in fact fine, which the corpus caught immediately.
                 * The fallback stays inside the conditional expression and runs only when the
                 * subject has no value, so it does count. */
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
            /* `e!` is the user's signature: what the compiler cannot prove, the user takes
             * responsibility for, and not one check is generated. It has three uses:
             *   `opt!` (`option<T>`)  -> the payload T
             *   `r!` (`result<T,E>`)  -> the payload T; on failure the behaviour is whatever the
             *                            user signed for, and the undefined behaviour is theirs
             *   `p!` (`?ref T`)       -> `ref T`, the escape hatch for "I know it is not null"
             * Anything else is an error: a signature has to be written where it means something. */
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
            return ot->inner;
        }

        case EX_ENUMVAL: {
            /* An instance of a generic enum (`maybe<i64>`) is not in the type table's by-name
             * map: it is produced by instantiation. Constructing it through the EX_ASSOC path
             * therefore records the resolved type on `assocOwner`, which is preferred here. */
            Type *et = e->assocOwner ? e->assocOwner : ttFromName(tt, e->u.enumval.typeName);
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
                /* The payload is stored inside this value, so a reference it carries must not
                 * live for less than the value does. */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }

        case EX_CALL: {
            /* `f(x)` where `f` holds a lambda. v1 has no call through a value, so the node rewrites
             * itself **in place** into the method call `f.call(x)` and the ordinary method path takes
             * it from there -- the same in-place rewriting the variant branch below does. */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                if (sy && sy->type && sy->type->kind == TY_STRUCT && sy->type->sdef
                    && sy->type->sdef->lamSig) {
                    Expr *recv = e->u.call.callee;      /* read before `u.method` overwrites them */
                    Vec   args = e->u.call.args;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "call";
                    e->u.method.args = args;
                    return checkExprInner(c, e);
                }
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
                        e->assocOwner = et;
                        return checkExprInner(c, e);    /* the rest is EX_ENUMVAL's job */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
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

            /* `ownFd`/`closeFd` used to live here: the block owned the descriptor and
             * closed it at the exit. Files are the program's business now (decision 79), so
             * the compiler keeps no table of them -- `std::fs` closes with `close(2)`, which
             * it declares as an `extern!`, and the handle's `open` flag makes a second close
             * a no-op. What the compiler still does is *prove* a leak when it can: see the
             * "opened and never closed" check in `check_top.c`. */
            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                /* `print` / `println` are **deprecated** (2026-09-26). They are builtins
                 * dispatched by name, and they write through C's stdio - a second buffer
                 * that `io::cout`'s cannot be ordered against. Measured: a program whose
                 * last statement was `io::cout << "x"` printed nothing at all, and one that
                 * mixed the two printed the `println` side first. `io::cout` is the one
                 * console path, and it is a stream over an `fd`, so it can grow a file, a
                 * pipe or a buffer later; a builtin name cannot.
                 *
                 * The library migrated first - its `<<(f64)` was the last user inside the
                 * compiler's own sources - so this warning is always about the program's own
                 * call sites. It is a warning and not an error because the corpus still has
                 * hundreds of them; the note is the migration, and the migration is
                 * mechanical: `println(a, b)` becomes `io::cout << a << b << "\n"`. */
                ckWarn(c, e->line,
                       "`io::cout` is the one console path -- `use std::io`, then"
                       " `io::cout << x` (`println(x)` is `io::cout << x << \"\\n\"`)",
                       "`%s` is deprecated", name);
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkPrintArg(c, a); /* takes a value, so it dereferences */
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
            /* A function of another module written without qualification: `lib.extc` holds
             * `open`, and the source here says `open()`.
             *
             * A name without `$` is a source-level name, because the loader only rewrites
             * qualified names into flat ones. For such a name the message to give is "this needs
             * a qualified name", not "no such function", which would point the user the wrong
             * way. The test is simply whether the name contains `$`: the character is not legal
             * in an extC identifier, so when it appears the name must be one the loader mangled,
             * and only then is the function really missing. */
            if (!f && name && !strchr(name, '$') && !e->qualified) {
                for (size_t i = 0; i < c->m->funcs.len; i++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&c->m->funcs, i);
                    const char *d = cand->name ? strchr(cand->name, '$') : NULL;
                    if (d && cand->modName && !cand->isPrivate && strcmp(d + 1, shownName) == 0) {
                        requireQualified(c, shownName, cand->modName, false, e->line);
                        return ttError(tt);
                    }
                }
            }
            if (f && !f->reserved && f->modName && !e->qualified)
                requireQualified(c, shownName, f->modName, false, e->line);
            if (!f) {
                /* A bare enum constructor. The error has to point the way instead of saying
                 * only "no such function".
                 *
                 * After `type shape = | circle(f64) | ...`, writing `circle(2.0)` used to report
                 * `call to undefined function `circle`` plus "built-ins available:
                 * print/println", from which nobody could tell that this is a variant and has to
                 * be written with its type name.
                 *
                 * No type inference happens here: writing the type out in full for an associated
                 * call is deliberate, because extC does not guess a type from context. The
                 * message states the correct spelling; it does not fill it in.
                 * Only a variant with a payload is pointed at. A payload-free `status.ok` is a
                 * value in its own right and takes the other path. */
                /* `ifstream("input.txt")`: a call whose name is a **type** is that
                 * type's constructor -- the associated function `new`. It is rewritten
                 * into the associated-call node, so the argument checking, the arena
                 * argument and the generated C are the very same ones as for
                 * `ifstream::new("input.txt")`; there is no second implementation to
                 * drift away from this one.
                 *
                 * Only when no local binding of that name is in scope: a variable
                 * shadowing a type name would otherwise be reported as a missing
                 * constructor, which is the wrong sentence about the wrong thing. */
                if (!lookup(c, name)) {
                    Type *ct = ttFromName(tt, name);
                    if (ct && !ttIsError(ct)) {
                        StructDef *csd = structOf(ct);
                        FuncDef *ctor = NULL;
                        if (csd) {
                            for (size_t i = 0; i < csd->methods.len && !ctor; i++) {
                                FuncDef *m = *(FuncDef **)vecAt(&csd->methods, i);
                                if (m->isAssoc && strcmp(m->name, "new") == 0) ctor = m;
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
                            /* The fields the old kind used are still in the union, so
                             * every one of them has to be written: leaving `targs` alone
                             * made the node carry the call's leftovers, and the resolver
                             * reported "`named` expects 0 type argument(s), got 1" about a
                             * type nobody had written type arguments for. */
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
                    e->domNew = true;          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
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
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区）。 */
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
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                e->parWorker = wf;
                wf->isParWorker = true;
                wf->parTlsArena = true;
                wf->used = true;
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */

            /* A generic free function: `T` can be inferred from an argument only, because an
             * instance of a type is decided by the type while a call to a free function has
             * nothing but its arguments. So the arguments are checked first, then `T` is
             * inferred, then the instance is created. When inference fails because `T` appears
             * only in the return type, the message tells the user to write the arguments
             * explicitly: `f<i32>(...)`. */
            if (f->typeParams.len > 0) {
                if (e->u.call.args.len != f->params.len) {
                    ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                            name, f->params.len, e->u.call.args.len);
                    return f->ret ? f->ret : ttVoid(tt);
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
                    if (!unifyTParams(c->tt, &f->typeParams, &targs, want, at)) {
                        ckError(c, e->line,
                                "A generic function's type parameters are inferred from its arguments."
                                " Write them out at the call to supply one that no argument mentions:"
                                " `f<i32>(...)` seeds the inference.",
                                "cannot infer type parameter(s) of `%s` from the arguments", name);
                        return f->ret ? f->ret : ttVoid(tt);
                    }
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
                 * instance is checked again, `e->func` is redirected to the concrete instance
                 * (`idOf_i32`). */
                FuncDef *inst = funcInstance(c, f, &targs, e->line);
                inst->used = true;
                e->func = inst;
                f = inst;                     /* every check below uses the instance */

                bool hasParamTarg = false;
                for (size_t i = 0; i < targs.len; i++) {
                    Type *a = *(Type **)vecAt(&targs, i);
                    if (a && ttHasParam(a)) { hasParamTarg = true; break; }
                }
                if (hasParamTarg && c->curFunc) {
                    CallCheck *cc = (CallCheck *)arenaAllocZero(c->arena, sizeof(CallCheck));
                    cc->node = e;
                    cc->tmpl = f->tmpl ? f->tmpl : f;   /* the template, not the instance */
                    cc->func = c->curFunc;   /* which template body this belongs to */
                    vecInit(&cc->targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < targs.len; i++)
                        *(Type **)vecPush(&cc->targs) = *(Type **)vecAt(&targs, i);
                    *(CallCheck **)vecPush(&c->callChecks) = cc;
                }
            }

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            /* The arena for this call comes from the shallowest `mut ref` argument. */
            e->homeDepth = callHomeDepth(c, &e->u.call.args, &f->params, e);
            setCallArenaArg(c, e);      /* decide which arena the call finally passes */
            setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
            /* The reference check has to run after the arguments are checked: until then
             * `a->type` is not filled in, `typeContainsRef` answers "no" for everything, and a
             * violation is silently accepted. The method path hit the same trap. The check itself
             * is at the end of this case. */
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkInto(c, p->type, a);

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
                /* A coroutine value cannot cross a function boundary yet: the argument's type is
                 * the `coroutine<T>` marker, while codegen materialises the coroutine's own frame.
                 * Loud beats a C type mismatch. The uniform representation (a boxed handle) lands
                 * with slice C, which is what a scheduler needs anyway. */
                /* Legal now: `coroutine<T>` is the unified handle, a plain 24-byte value. */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, p->type, at, a, "argument");
                }
            }
            /* Every call site is checked, not only the ones with a home arena: what matters is
             * that the callee may store into a container the argument points at, which has
             * nothing to do with the home arena.
             * The position matters as well: after the arguments are checked, because
             * `exprRefDepth` reads their types. */
            checkCallRefArgs(c, f, &e->u.call.args, &f->params, e->homeDepth, e->line, name);
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_EXT: {
            /* `ext` hands a task to a **domain**, and a domain is a lexical block from the library
             * (`sched::run { … }`, `par::pool(n) { … }`). Outside one there is nobody to hand it to,
             * so this is a compile error -- the point of the design is that it fails here and not at
             * run time (docs/topics/CONCURRENCY.md, `ext` 与调度域). */
            if (c->domainDepth == 0) {
                ckError(c, e->line,
                        "A domain is a lexical block that says where the concurrency runs and when"
                        " it must all be finished; `ext` hands the task to the nearest one.",
                        "there is no domain here: `ext` is only legal inside `sched::run { … }` or"
                        " `par::pool(n) { … }`");
                return ttError(tt);
            }
            /* Inside a domain the shape is accepted; the spawn itself (building the task and
             * handing it over) is the next step. Loud, never a silent call. */
            ckError(c, e->line,
                    "The domain rule is in place; building the task and handing it to the domain is"
                    " the next step (docs/topics/CONCURRENCY.md, `ext` 与调度域).",
                    "`ext` inside a domain is not implemented yet");
            return ttError(tt);
        }
        case EX_LAMBDA: return checkLambda(c, e);
        case EX_DYN: {
            /* `dyn Trait(x)` as a **value**: its type is `dyn Trait`, and the payload must
             * implement the trait -- the implementation is what the table points at. The payload's
             * type is kept on the node: codegen needs its C spelling for the temporary and for
             * `sizeof`, and by then this node's own type is `dyn Trait`. */
            Type *payT = checkExprInner(c, e->u.dynv.payload);
            /* The payload's own node needs its type too: codegen emits the temporary from it, and a
             * node left without one came out as `(int){0}` (found by compiling the generated C). */
            if (payT) e->u.dynv.payload->type = payT;
            e->u.dynv.payloadType = payT;
            /* A `dyn` value is pool-backed, so the enclosing function creates a pool -- the same
             * existing flag the dispatch site sets, and it must be set here too: without it the pool
             * runtime was never emitted and the generated C called `extc_dyn_put` undeclared. */
            if (c->curFunc) c->curFunc->makesPool = true;
            dynTraitOf(c, e->u.dynv.traitName, payT, e->line);
            Type *dt = typeNamed(c->arena, e->u.dynv.traitName);
            dt->kind = TY_DYN;
            e->type = dt;
            return dt;
        }

        case EX_METHOD: {
            /* Constructing a variant with a payload can look like this as well: the syntax of
             * `shape.circle(2.0)` is that of a method call, `receiver.name(args)`, and the only
             * difference is that `shape` is a type name rather than a variable. So it is
             * recognised here first, exactly as `status.ok` is recognised on the EX_FIELD path. */
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
                    e->assocOwner = et;
                    return checkExprInner(c, e);
                }
            }

            /* Methods live inside a `struct` body only, so they are found by the type of the
             * receiver. */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
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
            if (!f) {
                /* `d.m(...)` where `d` is a `dyn Trait` value: the method lives in the **trait's**
                 * declaration, not in any type's method set -- the concrete type is not known until
                 * runtime (DYN.md stage 3). Placed here, after the receiver has been checked, so
                 * every receiver shape works (`d`, `h.d`, …) and the receiver's node already carries
                 * its type for codegen. Reading the receiver's type without checking it was the
                 * first version, and it reordered this pass badly enough to break an unrelated
                 * stdlib case. */
                if (ttBase(recvT) && ttBase(recvT)->kind == TY_DYN) {
                    /* The receiver may be reached through a reference (`r.tag()` with
                     * `r: ref dyn Tag`). `ttBase` unwraps it here, so record the fact for codegen:
                     * it has to dereference the C expression when it hands the handle to
                     * `extc_dyn_slot`. */
                    e->dynRecvViaRef = (recvT && recvT->kind == TY_REF);
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
                    /* Object safety, the half that was missing: a `dyn` call goes through the trait's
                     * uniform table and hands the receiver to the thunk, so a method **without a
                     * receiver** has no slot to sit in. `want->params.len == 0` used to reach code
                     * generation, which indexed `params[0]` and crashed the compiler
                     * (tools/attack.py dyn D2: `trait Bad { fn nope() -> i64 }` + `d.nope()` --
                     * SIGSEGV in genMethodCall, codegen.c:1803). The `Self`-returning case was
                     * already refused; this is the same rule. */
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
                        Type *at = checkExprInner(c, a);
                        Param *wp = *(Param **)vecAt(&want->params, i + 1);
                        if (!mentionsParam(wp->type) && !ttEquals(ttBase(wp->type), ttBase(at)))
                            ckError(c, a->line,
                                    "Argument types must match the trait's declaration.",
                                    "argument %zu expects `%s`, found `%s`", i + 1,
                                    typeStr(c, ttBase(wp->type)), typeStr(c, ttBase(at)));
                    }
                    if (c->curFunc) c->curFunc->makesPool = true;
                    e->u.method.recv->type = ttBase(recvT);
                    e->func = want;
                    e->dynTrait = traitName;
                    e->type = want->ret ? want->ret : ttVoid(tt);
                    return e->type;
                }
                /* A method call on a type parameter (`#57`): `T` is opaque on the template, so
                 * which method this means is only known per instance. Record it and hand back
                 * the error type -- that type exists so errors do not cascade, and
                 * `checkAssignable` waves it through -- then the instance pass decides and
                 * reports (`runMethodCheck` in check_top.c). Without this a generic container
                 * could not do anything at all with a value of its element type, which is
                 * exactly what blocked `hashMap<K, V>`: hashing a `K` means calling `hash()` on
                 * a type parameter. */
                if (c->curFunc && mentionsParam(recvT)) {
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
                    e->domNew = true;          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
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
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区）。 */
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
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                e->parWorker = wf;
                wf->isParWorker = true;
                wf->parTlsArena = true;
                wf->used = true;
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */
            /* `dyn Trait(x).m(...)`: **object safety** (DYN.md stage 1).
             *
             * Not every method can be dispatched through a table, and the rule is enforced here --
             * at the `dyn` use site -- rather than on the trait declaration: such a method is
             * perfectly fine as a static call, it simply has no slot to sit in. The judgment is
             * about the **trait's** signature, so the trait is looked up by name from the module's
             * declarations (the same lookup the `impl` attachment uses). */
            if (e->dynTrait) {
                /* A `dyn` value is pool-backed, so the enclosing function creates a pool. That is
                 * the **existing** flag which makes codegen enter a place for the body and emit the
                 * pool runtime -- and it has to be set here, in the checker, because the decision to
                 * emit `zoneEnter` is taken before the body is generated (setting the codegen flag
                 * mid-body was too late: the program trapped with "a `dyn` value needs a place to
                 * live in"). */
                if (c->curFunc) c->curFunc->makesPool = true;
                TraitDef *tr = dynTraitOf(c, e->dynTrait, e->u.method.recv->type, e->line);
                FuncDef *want = NULL;
                for (size_t ti = 0; tr && ti < tr->methods.len && !want; ti++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&tr->methods, ti);
                    if (strcmp(cand->name, e->u.method.name) == 0) want = cand;
                }
                /* Object safety. These three are FATAL on purpose: a trait method with no
                 * receiver makes `f->params` empty, and the receiver checks below read
                 * `params[0]`. Reporting and carrying on used to segfault the compiler
                 * (`dyn Tag(b).zero()` with a `fn zero()` in the trait: exit 139, no
                 * diagnostic at all). */
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
                    int d = placeDepth(c, e->u.method.recv);
                    /* The `self: mut ref` branch does not go through `callHomeDepth`, so the
                     * escape decision has to be applied here as well. The first version missed it,
                     * and ASan caught the resulting use-after-free. */
                    const char *rrn = placeRootName(e->u.method.recv);
                    if (d != 0 && rrn && isEscapeeName(c, rrn)) d = -1;
                    e->homeDepth = (d == 0) ? -1 : d;
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
                    e->homeDepth = callHomeDepth(c, &e->u.method.args, &f->params, e);
                }
                /* Resolve which arena the call finally passes; both numbers are decided here. */
                setCallArenaArg(c, e);
                setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
                /* Whether the argument is a local or a parameter, the depth of what is pushed
                 * into a container has to be recorded. The first version did this in the `else`
                 * branch only, so the `self: mut ref` path -- which is the one `v.push(...)`
                 * takes -- never ran it at all. Only debugging showed why. */
                /* A method call is checked like any other call site: content flow does not
                 * depend on the home arena.
                 *
                 * The receiver has to count as argument 0. The callee's `params[0]` is `self`,
                 * while `e->u.method.args` does not contain the receiver, so the two lengths
                 * differ and the `params->len != args->len` guard at the top of
                 * `checkCallRefArgs` returns early without a word. The reference rule therefore
                 * never applied to a method call at all -- a latent defect found by inspection.
                 * Prepending the receiver below fixes that. */
                (void)0;   /* the check is moved below, after the arguments; see there */
            }

            /* When the receiver is a generic instance, the `T` in the method signature has to
             * be replaced with the type arguments -- and when the **method itself** is generic
             * (`fn cast<U>(self: ref box, u: U)`), its own parameters are inferred from the
             * arguments exactly like a free generic function's (`unifyTParams`, the same call the
             * `EX_CALL` path makes). Before this, a method's own parameters were neither inferred
             * nor substituted, so every such call failed with "argument expects `U`, found `i64`"
             * -- even when the signature never mentioned `U`. */
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

            if (e->u.method.args.len != want) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        e->u.method.name, want, e->u.method.args.len);
                return rt;
            }
            for (size_t i = 0; i < want; i++) {
                Param *p = *(Param **)vecAt(&f->params, i + 1);
                Expr  *a = *(Expr **)vecAt(&e->u.method.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);

                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* A coroutine value cannot cross a function boundary yet: the argument's type is
                 * the `coroutine<T>` marker, while codegen materialises the coroutine's own frame.
                 * Loud beats a C type mismatch. The uniform representation (a boxed handle) lands
                 * with slice C, which is what a scheduler needs anyway. */
                /* Legal now: `coroutine<T>` is the unified handle, a plain 24-byte value. */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, pt, at, a, "argument");
                }
            }
            /* The rule has to be checked after the arguments are checked: until then `a->type`
             * is not filled in, `typeContainsRef` answers "no" for everything, and a violation
             * passes silently -- a counterexample that should have been rejected was not.
             * The receiver is prepended as argument 0, because the callee's `params[0]` is
             * `self`. */
            {
                Vec margs; vecInit(&margs, c->arena, sizeof(Expr *));
                *(Expr **)vecPush(&margs) = e->u.method.recv;
                for (size_t ai = 0; ai < e->u.method.args.len; ai++)
                    *(Expr **)vecPush(&margs) = *(Expr **)vecAt(&e->u.method.args, ai);
                checkCallRefArgs(c, f, &margs, &f->params, e->homeDepth,
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
             * resolves as written. The context-free `ttFromName` used to be called here, which
             * sees only the entry file's structs: a **named** literal inside a module was
             * `unknown struct` while the very same literal written as an annotated `var` worked
             * (PLAN #54). The message for a name that really is unknown comes from `ttResolve`,
             * which also reports the ambiguous case (`pair` exported by two modules). */
            Type *st = e->type;
            if (e->u.lit.name) {
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
            }

            /* An omitted field is filled in with its zero value, but `ref` has no zero value.
             * Left alone this generates `.field = 0`, which is a null reference, while the
             * language says `ref` can never be null. The same kind of hole appeared for `str`
             * before. */
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
                            "`ref` has no default value (it is a non-nullable reference)",
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

/* Report whether evaluating an expression may print, directly or through a call.
 *
 * Params:
 *   c - checker
 *   e - the expression to inspect; NULL counts as "does not print"
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 */
static bool exprMayPrint(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return true;
        if (strcmp(n, "flush") == 0) return false;      /* flushing is not printing */
    }
    switch (e->kind) {
/* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every
     * walker has to look inside it -- the walkers end in `default:`, which is why `-Wswitch`
     * never pointed at the omission. */
    case EX_DYN: return exprMayPrint(c, e->u.dynv.payload);
    case EX_CALL:
        if (e->func && funcMayPrint(c, e->func)) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (e->func && funcMayPrint(c, e->func)) return true;
        if (exprMayPrint(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_ASSOC:
        if (e->func && funcMayPrint(c, e->func)) return true;
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

/* Report whether executing a statement may print, directly or through a call.
 *
 * Params:
 *   c - checker
 *   s - the statement to inspect; NULL counts as "does not print"
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 */
static bool stmtMayPrint(Checker *c, Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMayPrint(c, s->u.yield_.value);
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

/* Report whether a function may print, directly or through the calls it makes.
 *
 * Params:
 *   c - checker
 *   f - the function to inspect; NULL means the callee could not be resolved
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 *
 * Notes:
 *   - `FuncDef.callees` is filled in only after a body has been checked, so it cannot be used
 *     while bodies are still being checked. This walks the AST of the callee instead, which
 *     needs a cycle guard of its own.
 *   - Both an unresolved callee and a recursive one count as printing, which is the safe
 *     direction for an answer that gates reordering.
 */
static bool funcMayPrint(Checker *c, FuncDef *f) {
    if (!f) return true;                       /* unresolved: assume it prints */
    if (f->mayPrintState == 1) return true;
    if (f->mayPrintState == 2) return false;
    if (f->mayPrintState == 3) return true;    /* a cycle: assume it prints */
    f->mayPrintState = 3;
    bool r = stmtMayPrint(c, f->body);
    f->mayPrintState = r ? 1 : 2;
    return r;
}

/* Report whether calling this function has an observable side effect.
 *
 * A call counts when it prints, when it takes a `mut ref` or a `mut` view parameter and may
 * therefore write the argument, when it reaches such a call itself, or when it cannot be
 * resolved at all.
 *
 * Params:
 *   c - checker
 *   f - the callee; NULL means it could not be resolved
 *
 * Returns:
 *   True when reordering this call would change what the program does.
 *
 * Notes:
 *   - A pure getter does not count: swapping two of them cannot be observed. Judging by
 *     "contains a call" instead rejected a benchmark program built from plain accessors, whose
 *     expression `t.get() + (v.get(0) ?? 0)` is entirely side-effect free.
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

/* Report whether an expression contains a call whose reordering would be observable.
 *
 * Params:
 *   c - checker
 *   e - the expression to inspect; NULL counts as "no call"
 *
 * Returns:
 *   True when an effectful call appears anywhere inside, at any depth.
 *
 * Notes:
 *   - This used to `break` out of the switch instead of returning, which dropped control to the
 *     end of the function and returned an indeterminate value: undefined behaviour in the
 *     compiler itself, and `-Wreturn-type` had been reporting it. The consequence was that a
 *     call known to be free of side effects, `pure(x)`, made the answer garbage, so the "an
 *     earlier call is still in place" test fired only sometimes.
 */
/* Does this expression contain a **call**, whatever the effect summary says?
 *
 * `exprHasCall` answers the semantic question ("can evaluating this twice be observed?"),
 * which is what the `??` ordering rule needs. This one answers the syntactic question, and
 * it is deliberately the blunt one: it guards the single place where the compiler has to
 * name an expression twice - the target of a compound assignment whose operator is a
 * user-defined method (`x += y` becomes `x = add(&x, y)`). There, "the summary says it is
 * pure" is not a good enough reason to duplicate a call: a body that reads a global it
 * never writes is pure by that summary, and duplicating it would still be wrong the day
 * the global moves. One rule, no exceptions: a call in the target is refused, and the
 * message says to split the line.
 */
static bool exprHasAnyCall(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every walker
     * has to look inside it -- the walkers end in `default:`, which is why `-Wswitch` never
     * pointed at the omission when the kind was added. */
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

static bool exprHasCall(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every walker
     * has to look inside it -- the walkers end in `default:`, which is why `-Wswitch` never
     * pointed at the omission when the kind was added. */
    case EX_DYN: return exprHasCall(c, e->u.dynv.payload);
    case EX_CALL: case EX_METHOD: case EX_ASSOC:
        if (callIsEffectful(c, e->func)) return true;
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

/* Check an expression, do the per-statement bookkeeping, and store the resulting type.
 *
 * This is the entry point the rest of the checker uses, so the bookkeeping below sees every
 * subexpression of a statement.
 *
 * Params:
 *   c - checker
 *   e - the expression; NULL yields the error type
 *
 * Returns:
 *   The type of the expression, which is also left on `e->type`. A missing type is turned into
 *   the error type, so no caller can observe one.
 *
 * Notes:
 *   - The effect flag records that a call left in place has already been seen in this statement.
 *     A later `??` that needs a temporary would hoist that temporary in front of the call and
 *     reorder the two, so it is reported instead.
 *   - The subject of `??` does not set the flag: its temporary is hoisted together with the
 *     other temporaries in source order, so their relative order is unchanged. Counting it
 *     wrongly reported `println(a, v.get(3) ?? -1, b, v.get(99) ?? -1)`.
 */
Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    if (exprHasCall(c, e) && !(e->kind == EX_COALESCE && e->needTemp)) c->stmtFx = 1;
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

