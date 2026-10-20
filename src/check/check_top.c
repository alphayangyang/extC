/* Top level: function bodies, generic instantiation, deferred checks.
 *
 * This is the last checking pass. It validates every declaration for duplicate
 * names, computes the effect summary of every function (does it reach an allocator,
 * can a reference it hands out escape), instantiates generic functions at each call
 * site, and checks each function body and global initializer for lifetimes and
 * arena levels.
 */

#include "dbg.h"
#include "check_internal.h"
#include "typelayer.h"   /* the pool questions below are shared with codegen */
#include "plan.h"          /* 计划/分析产物：写入经 setter，读取经访问器（X2）*/
/* Defined further down; the call-site publication needs it (INV-K / ). */
static void noteFieldSrc(Checker *c, Sym *root, const char *field, int d2, Expr *src);
#include "dataflow.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <stdlib.h>   /* getenv (the EXTC_DUMP_EFFECTS debug switch) */
#include <time.h>

/**
 * @brief 计时开关是否打开（环境变量 `EXTC_DBG_TIME`），只读一次环境。
 *
 * @return  打开时为 1，关闭时为 0。
 */
static int ctOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }

/**
 * @brief 进程 CPU 时间，秒。
 *
 * @return  自进程启动以来消耗的 CPU 秒数。
 */
static double ctNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/**
 * @brief 打一行阶段耗时；开关关着时什么都不做。
 *
 * @param n   阶段名。
 * @param t0  阶段开始时的 CPU 秒数。
 */
static void ctPhase(const char *n, double t0) { if (ctOn()) fprintf(stderr, "[time] %-9s %.3f s\n", n, ctNow() - t0); }

/**
 * @brief 取一个函数或方法可见的类型参数列表。
 *
 * 方法看它所在 struct 的类型参数，自由函数看自己的；函数体里解析类型的地方都只问这一处。
 *
 * @param f  函数或方法；NULL 允许。
 * @return   要实例化的参数表；不是泛型时返回 NULL。
 */
Vec *funcTParams(FuncDef *f) {
    if (!f) return NULL;
    if (f->typeParams.len) return &f->typeParams;
    if (f->owner) return &f->owner->typeParams;
    return NULL;
}

/* Forward declarations: the effect-summary walk and the escape analysis call each
 * other, so one of them has to be declared ahead of its definition. */
/**
 * @brief 判断两个声明是否来自同一个模块。
 *
 * @param a  模块名；NULL 表示根文件（或 prelude）。
 * @param b  模块名；NULL 表示根文件（或 prelude）。
 * @return   两个都为 NULL、或名字相同时返回 true；否则 false。
 */
static bool sameModule(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static int  paramIndexByName(FuncDef *f, const char *name);   /* 只在只有名字时用；有表达式用 paramIndexOfExpr */
static bool exprIsFresh(Expr *e);   /* used by the call-site publication (INV-K) */

/* ------------------------------------------------------------------- top level */

/**
 * @brief 就地解析一个签名里写到的所有类型。
 *
 * 在任何函数体被检查之前跑完，之后的检查不必再解析类型名。签名里的类型名属于声明它的文件，
 * 因此在那个文件的上下文（`FuncDef.ctx`）下解析。
 *
 * @param c  checker（类型表与解析用的模块上下文）。
 * @param f  参数与返回类型就地解析的函数。
 */
void resolveSignature(Checker *c, FuncDef *f) {
    /* A method sees the type parameters of the struct it belongs to; a free function
     * sees its own (`funcTParams` returns whichever applies). */
    Vec *params = funcTParams(f);
    /* The type names in a signature belong to the file that declares it, so they are
     * resolved under that file's context (`FuncDef.ctx`), not the entry file's. */
    Ctx *savedCtx = c->ctx;
    if (f->ctx) c->ctx = f->ctx;

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line, params);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line, params);
    if (f->ret && ttIs(f->ret, "void")) f->ret = NULL;
    c->ctx = savedCtx;
}

/**
 * @brief 拒绝模块声明之间的重名。
 *
 * 名字在作用域表里按字符串查找，重名会静默遮蔽另一条定义而不是被报告；每类声明各自比较，
 * 诊断才能点出冲突的那一对。错误报在较晚那条声明的行上。
 *
 * @param c  checker。
 */
static void checkDeclarations(Checker *c) {
    Module *m = c->m;

    /* Duplicate struct and function names. */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *a = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = i + 1; j < m->structs.len; j++) {
            StructDef *b = *(StructDef **)vecAt(&m->structs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line,
                        (a->reserved || b->reserved)
                            ? "It comes from stdlib/prelude.extc and is part of the language contract."
                            : NULL,
                        (a->reserved || b->reserved)
                            ? "`%s` is a reserved definition and cannot be redefined"
                            : "duplicate struct `%s`", b->name);
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        if (i + 8 < m->funcs.len) __builtin_prefetch(*(FuncDef **)vecAt(&m->funcs, i + 8), 0, 0);
        FuncDef *a = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t j = i + 1; j < m->funcs.len; j++) {
            FuncDef *b = *(FuncDef **)vecAt(&m->funcs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line,
                        (a->reserved || b->reserved)
                            ? "It comes from stdlib/prelude.extc and is part of the language contract."
                            : NULL,
                        (a->reserved || b->reserved)
                            ? "`%s` is a reserved definition and cannot be redefined"
                            : "duplicate function `%s`", b->name);
        }
    }

    /* Duplicate field names, duplicate method names, and a method that collides with
     * a field of the same struct (the field would become unreachable). */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            if (j + 8 < sd->fields.len) __builtin_prefetch(*(FieldDef **)vecAt(&sd->fields, j + 8), 0, 0);
            FieldDef *fa = *(FieldDef **)vecAt(&sd->fields, j);
            for (size_t k = j + 1; k < sd->fields.len; k++) {
                FieldDef *fb = *(FieldDef **)vecAt(&sd->fields, k);
                if (strcmp(fa->name, fb->name) == 0)
                    ckError(c, fb->line, NULL, "struct `%s` has duplicate field `%s`",
                            DN(sd), fb->name);
            }
        }
        for (size_t j = 0; j < sd->methods.len; j++) {
            if (j + 8 < sd->methods.len) __builtin_prefetch(*(FuncDef **)vecAt(&sd->methods, j + 8), 0, 0);
            FuncDef *ma = *(FuncDef **)vecAt(&sd->methods, j);
            if (findField(sd, ma->name))
                ckError(c, ma->line, NULL, "`%s.%s`: a field and a method cannot share a name",
                        DN(sd), ma->name);
            for (size_t k = j + 1; k < sd->methods.len; k++) {
                FuncDef *mb = *(FuncDef **)vecAt(&sd->methods, k);
                if (strcmp(ma->name, mb->name) != 0) continue;
                /* An operator name may be defined once per right-operand type: its shape
                 * is fixed (`self: ref T` plus one operand), so the operand type is the
                 * only thing left to tell two definitions apart, and matching it exactly
                 * is what makes the set decidable. The same operand type twice is still a
                 * duplicate -- there would be no rule to pick between them. */
                if (isOverloadableOp(ma->name) && ma->params.len == mb->params.len &&
                    ma->params.len >= 2) {
                    Param *qa = *(Param **)vecAt(&ma->params, 1);
                    Param *qb = *(Param **)vecAt(&mb->params, 1);
                    if (!ttEquals(ttBase(qa->type), ttBase(qb->type))) continue;
                    ckError(c, mb->line,
                            "Operators are matched on the type of the right operand, so two "
                            "definitions that take the same one cannot be told apart.",
                            "struct `%s` has two `%s` for `%s`", DN(sd), mb->name,
                            typeStr(c, ttBase(qb->type)));
                    continue;
                }
                /* Name the earlier declaration too: with one method set per type, "which of the
                 * two is the duplicate" is the first question a reader asks. This is the single
                 * place that answers it, for body methods and `impl` methods alike. */
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "a type has one method set and `impl` adds to it, it does not"
                                 " replace it: the first declaration is at line %d%s",
                          (ma->body && ma->body->line > 0) ? ma->body->line : 0,
                          ma->modName ? " of the module that declared it" : "");
                ckError(c, mb->line, bufCstr(&note), "struct `%s` has duplicate method `%s`",
                        DN(sd), mb->name);
            }
        }
    }

    /* Duplicate type names, duplicate variant names, and a type with no variants
     * (there would be no value of it to construct). */
    for (size_t i = 0; i < m->types.len; i++) {
        if (i + 8 < m->types.len) __builtin_prefetch(*(TypeDef **)vecAt(&m->types, i + 8), 0, 0);
        TypeDef *a = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = i + 1; j < m->types.len; j++) {
            TypeDef *b = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate type `%s`", b->name);
        }
        if (a->variants.len == 0)
            ckError(c, a->line, NULL, "type `%s` has no variants", a->name);
        for (size_t j = 0; j < a->variants.len; j++) {
            if (j + 8 < a->variants.len) __builtin_prefetch(*(Variant **)vecAt(&a->variants, j + 8), 0, 0);
            Variant *va = *(Variant **)vecAt(&a->variants, j);
            for (size_t k = j + 1; k < a->variants.len; k++) {
                Variant *vb = *(Variant **)vecAt(&a->variants, k);
                if (strcmp(va->name, vb->name) == 0)
                    ckError(c, vb->line, NULL, "type `%s` has duplicate variant `%s`",
                            a->name, vb->name);
            }
        }
    }

    /* A struct and a type share one namespace, so they may not collide either. */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < m->types.len; j++) {
            if (j + 8 < m->types.len) __builtin_prefetch(*(TypeDef **)vecAt(&m->types, j + 8), 0, 0);
            TypeDef *td = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(sd->name, td->name) == 0)
                ckError(c, td->line, NULL, "`%s` is already a struct", DN(td));
        }
    }
    /* `impl Trait for Type`: do the signatures agree?
     *
     * This runs here rather than at attachment because comparing types requires them to be
     * **resolved** -- `Self` in a trait body is one name away from the implementing type, and
     * `ttSubstitute` is the existing machinery for exactly that substitution. The pair itself
     * (which trait, which type) was recorded at attachment, so nothing is looked up twice. */
    for (size_t i = 0; i < m->impls.len; i++) {
        if (i + 8 < m->impls.len) __builtin_prefetch(*(ImplDef **)vecAt(&m->impls, i + 8), 0, 0);
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        StructDef *sd = structOf(planImplTarget(im));
        if (!sd) continue;
        Vec selfParams, selfArgs;
        vecInit(&selfParams, c->arena, sizeof(const char *));
        vecInit(&selfArgs,   c->arena, sizeof(Type *));
        *(const char **)vecPush(&selfParams) = "Self";
        *(Type **)vecPush(&selfArgs) = planImplTarget(im);
        /* `impl Codec<i64> for box`: the trait's **own** parameters substitute through the same
         * pair of parallel lists, so a declared signature that mentions `T` is compared against
         * the implementation as `i64`. Without this the conformance check compared `T` with `i64`
         * and rejected every explicit-parameter trait. */
        for (size_t pi = 0; pi < im->traitArgs.len && pi < planImplTrait(im)->typeParams.len; pi++) {
            *(const char **)vecPush(&selfParams) =
                *(const char **)vecAt(&planImplTrait(im)->typeParams, pi);
            /* Resolved, not as written: the parser hands back an unresolved `i64`, and substituting
             * that in produces a look-alike node that `ttEquals` (interning identity) rejects --
             * the check then reported "is `i64`, but the trait declares `i64`". */
            *(Type **)vecPush(&selfArgs) =
                ttResolve(c->tt, c->ctx, *(Type **)vecAt(&im->traitArgs, pi), im->line, NULL);
        }
        for (size_t k = 0; k < planImplTrait(im)->methods.len; k++) {
            if (k + 8 < planImplTrait(im)->methods.len) __builtin_prefetch(*(FuncDef **)vecAt(&planImplTrait(im)->methods, k + 8), 0, 0);
            FuncDef *want = *(FuncDef **)vecAt(&planImplTrait(im)->methods, k);
            FuncDef *have = NULL;
            for (size_t j = 0; j < sd->methods.len && !have; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (strcmp(cand->name, want->name) == 0) have = cand;
            }
            if (!have) continue;            /* absence was reported at attachment */
            if (funcIsMethod(want) != funcIsMethod(have) ||
                want->params.len != have->params.len) {
                ckError(c, im->line,
                        "The implementation repeats the trait's signature exactly, including"
                        " whether the method takes a receiver.",
                        "`%s`: `%s` does not match the signature the trait declares",
                        planImplTrait(im)->name, want->name);
                continue;
            }
            for (size_t j = 1; j < want->params.len; j++) {
                Param *wp = *(Param **)vecAt(&want->params, j);
                Param *hp = *(Param **)vecAt(&have->params, j);
                /* Substitute only when the type actually mentions a parameter: `ttEquals` is
                 * interning identity, and rebuilding `i64` through `ttSubstitute` produces a
                 * look-alike node that would never compare equal (found by a false positive on
                 * the positive case). */
                Type  *wantT = mentionsParam(wp->type)
                             ? ttBase(ttSubstitute(c->tt, wp->type, &selfParams, &selfArgs))
                             : ttBase(wp->type);
                if (!ttEquals(wantT, ttBase(hp->type)))
                    ckError(c, have->line,
                            "A trait method's parameter types are part of the contract and must"
                            " match exactly (`Self` stands for the implementing type).",
                            "`%s`: parameter %zu of `%s` is `%s`, but the trait declares `%s`",
                            planImplTrait(im)->name, j + 1, want->name,
                            typeStr(c, ttBase(hp->type)), typeStr(c, wantT));
            }
            Type *wantR = want->ret && mentionsParam(want->ret)
                        ? ttBase(ttSubstitute(c->tt, want->ret, &selfParams, &selfArgs))
                        : (want->ret ? ttBase(want->ret) : NULL);
            Type *haveR = have->ret ? ttBase(have->ret) : NULL;
            if (wantR && haveR && !ttEquals(wantR, haveR))
                ckError(c, have->line,
                        "A trait method's return type is part of the contract and must match"
                        " exactly (`Self` stands for the implementing type).",
                        "`%s`: `%s` returns `%s`, but the trait declares `%s`",
                        planImplTrait(im)->name, want->name, typeStr(c, haveR), typeStr(c, wantR));
        }
    }

}

/**
 * @brief 检查方法的形状：声明在哪里、`self` 是什么。
 *
 * 作用于值的函数必须声明在 struct 里；`==` 这类运算符就是这种函数，写在 struct 外是错误，
 * 而不是一个恰好叫 `==` 的自由函数。
 *
 * @param c  checker。
 * @param f  要检查的函数；自由函数的 `f->owner` 为 NULL。
 */
static void checkMethodShape(Checker *c, FuncDef *f) {
    if (!f->owner) {
        /* A free function has no receiver, so it may not take `self`. */
        if (strcmp(f->name, "==") == 0 || strcmp(f->name, "!=") == 0)
            ckError(c, f->line, "an operator is a method, so it must be declared inside a `struct`",
                    "operator `%s` must be defined inside a `struct`", f->name);
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (strcmp(p->name, "self") == 0)
                ckError(c, p->line, "methods must be declared inside their `struct`",
                        "`self` is only allowed in a method declared inside a `struct`");
        }
        return;
    }

    Param *p0 = f->params.len ? *(Param **)vecAt(&f->params, 0) : NULL;
    if (!p0 || strcmp(p0->name, "self") != 0) {
        /* No `self` means an associated function: `option<i64>::some(x)` or
         * `point::origin()`. It belongs to the type but acts on no value, which is
         * how a container exposes its constructors. Such a call is explicit: the
         * call site writes the full type name (`option<i64>::some`), so nothing is
         * inferred from the expected type. */
        anSetIsAssoc(f, true);
    } else {
        /* Compare the struct definition rather than the type node: the `self` of a
         * method on a generic struct is written `ref Pair<A, B>`, so its type node
         * is not the same pointer as the owner's type. */

        Type *sb = ttBase(p0->type);
        /* A builtin scalar has no `sdef` to compare against, so its holder is matched by the
         * type it stands for (`impl i64` -> `self: ref i64`). Every other owner is a real
         * declaration and is matched by definition, which is also what lets a generic method
         * write `ref Pair<A, B>` and still belong to `Pair`. */
        bool selfOk = planBuiltinHolder(f->owner)
                        ? (p0->type->kind == TY_REF && p0->type->inner == f->owner->type)
                        : (p0->type->kind == TY_REF && sb && sb->sdef == f->owner);
        if (!selfOk)
            ckError(c, p0->line, NULL, "`self` of `%s.%s` must be `ref %s`",
                    DN(f->owner), f->name, DN(f->owner));
    }
    for (size_t i = 1; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0)
            ckError(c, p->line, NULL, "`self` must be the first parameter");
    }

    checkOperatorSig(c, f);
    checkCtorSig(c, f);
}

/* Does the body allocate (`new`)?
 *
 * The answer decides whether the function needs a home arena of its own. It is a
 * pure syntax walk, so it can run before any type is known, and the arena level of
 * each `new` inside the body is therefore decided on the spot.
 *
 * Returns:
 *   True when the statement contains an allocation or an unknown-length allocation.
 */
/* Does the statement contain an allocation (`new`)?
 *
 * This decides whether a function needs a home arena of its own, and it is a pure syntax
 * walk, so it can answer before any type is known.
 *
 * Params:
 *   s - statement to walk; may be NULL
 *
 * Returns:
 *   True when the statement contains an allocation or an unknown-length allocation.
 */
static bool stmtHasNew(Stmt *s);
static void coroFrameLay(Checker *c, FuncDef *f);
static void coroCheckDeferred(Checker *c, Module *m);

typedef struct {
    const char *cname;
    Expr       *init;
    int         line;
    /* Did the initializer name storage that outlives every frame? Recorded while the scopes were
     * live (`ST_VAR.initOutOfFrame`): by the time `coroCheckDeferred` runs, `lookup` cannot
     * find a function-local name, so asking `exprOutOfFrame` again would answer "no" for a view of
     * a `global` declaration. */
    bool        outOfFrame;
} CoroDeferred;

static bool hasNewInStmt(void *ctx, Stmt *s);
/**
 * @brief 判断表达式里是否含 `new` 或 `alloc<T>(n)`。
 *
 * 遍历走 `astWalkExprChildren` / `astWalkStmtChildren`，AST 种类只在那里列一次。
 *
 * @param ctx  未使用。
 * @param e    表达式。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool hasNewInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (e->kind == EX_NEW || e->kind == EX_GENCALL) return false;   /* found */
    AstVisit v = { hasNewInExpr, hasNewInStmt, NULL };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief `hasNewInExpr` 的语句版遍历回调。
 *
 * @param ctx  未使用；本问题不需要上下文。
 * @param s    语句。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool hasNewInStmt(void *ctx, Stmt *s) {
    (void)ctx;                              /* this question needs no context */
    AstVisit v = { hasNewInExpr, hasNewInStmt, NULL };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 判断表达式里是否含 `new` 或 `alloc<T>(n)`。
 *
 * @param e  表达式；NULL 返回 false。
 * @return   含则 true。
 */
static bool exprHasNew(Expr *e) {
    if (!e) return false;
    return !hasNewInExpr(NULL, e);
}

/**
 * @brief 判断语句里是否含 `new` 或 `alloc<T>(n)`。
 *
 * @param s  语句；NULL 返回 false。
 * @return   含则 true。
 */
static bool stmtHasNew(Stmt *s) {
    if (!s) return false;
    return !hasNewInStmt(NULL, s);
}

/* Does the body call a function that needs a home arena?
 *
 * Params:
 *   s - statement to walk
 *
 * Returns:
 *   True when some call in the statement reaches a callee with `needsHome` set.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which
 *     the checker fills in when it resolves a call.
 */
/* Does the statement call a function that needs a home arena?
 *
 * Params:
 *   s - statement to walk; may be NULL
 *
 * Returns:
 *   True when some call in the statement reaches a callee whose `needsHome` is set.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which the
 *     checker fills in when it resolves a call.
 */
/* Does this expression, or this statement, reach a callee that needs a home arena?
 *
 * Two questions, one walk:
 *   precise = false  "does the callee reach a function with a home arena?"  -- the escape
 *                    question, which is what decides whether *this* function gets one;
 *   precise = true   "does the callee itself take a home arena?"             -- the narrow
 *                    question a **signature** needs.
 * `callsNeedsHome` / `callsUsesHome` below name the two, so no call site carries a bare flag.
 *
 * The walk is `astWalkExprChildren` / `astWalkStmtChildren` (`ast.c`), the one place that lists
 * the AST kinds. That matters here more than anywhere else, because the answer decides where
 * storage lives -- bugs of exactly this shape are on record:
 *
 *   - `EX_STRUCTLIT` was missing from this predicate, so `var b: box = { r: mknode() }` looked
 *     like a body that reaches no callee with a home arena. The function was not given one, the
 *     callee allocated into the caller's *block* arena (released when the block ends) while the
 *     depth recorded for the value said 0, and the pointer dangled. Every shape a call can hide
 *     in therefore has to be listed -- which is now the job of `ast.c`, not of this file.
 * The same missing-kind mistake was also made once for `EX_DYN`'s payload and once for
 * `EX_SLICE`'s bounds; migrating this predicate ends that class
 * of bug for it.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which the checker
 *     fills in when it resolves a call. A node without one (a generic primitive) is descended
 *     into like any other.
 *   - An associated call (`EX_ASSOC`) counts as well: an associated function such as
 *     `T::make` is declared without `self` but takes a home arena like any other. */
/* **One question, one name.** The question travels as a predicate with a name, never as a bare
 * `bool` switch that changes what the predicate below *meant* ("does the callee reach a home arena?"
 * versus "does it take one itself?"). A bare switch is precisely the shape that lets the two sides
 * of a convention drift apart: the definition asks one, the call site the other, and nothing in the
 * code says which is which. The two entry points below (`callsNeedsHome` / `callsUsesHome`) are the
 * only places that pick one. */
typedef struct { bool (*takesHomeArena)(const FuncDef *); } HomeQ;

/**
 * @brief 这个函数是否会（传递地）走到某个接收 home arena 的被调者。
 *
 * 保守的那一问，决定本函数体是否需要自己的 home arena。
 *
 * @param f  函数定义。
 * @return   需要时为 `true`；`f` 为 NULL 时为 `false`。
 */
static bool reachesHomeArenaTaker(const FuncDef *f) { return f && anNeedsHome(f); }

static bool needsHomeInStmt(void *ctx, Stmt *s);

/**
 * @brief 判断表达式是否调用到满足给定谓词的被调者。
 *
 * 谓词由调用方给出：`reachesHomeArenaTaker` 问“能否到达需要 home arena 的被调者”，
 * `funcTakesHomeArena` 问“被调者自己是否收 home arena”。
 *
 * @param ctx  `HomeQ *`，携带被调者谓词。
 * @param e    表达式。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool needsHomeInExpr(void *ctx, Expr *e) {
    HomeQ *q = (HomeQ *)ctx;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && planCallee(e) &&
        q->takesHomeArena(planCallee(e)))
        return false;                       /* found: stop the walk */
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief `needsHomeInExpr` 的语句版遍历回调。
 *
 * @param ctx  `HomeQ *`，携带被调者谓词。
 * @param s    语句。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool needsHomeInStmt(void *ctx, Stmt *s) {
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 判断语句里是否有调用满足给定的 `takesHomeArena` 谓词。
 *
 * 只在调用已解析后有效：遍历读 `planCallee(e)`，由 checker 在解析调用时填入。
 *
 * @param s               语句；NULL 返回 false。
 * @param takesHomeArena  被调者谓词。
 * @return                找到则 true。
 */
static bool stmtCallsNeedsHome(Stmt *s, bool (*takesHomeArena)(const FuncDef *)) {
    if (!s) return false;
    HomeQ q = { takesHomeArena };
    return !needsHomeInStmt(&q, s);
}


/* Does this expression, or this statement, mention the binding whose generated-C name is
 * `cname`?
 *
 * Used for one question only: is a parameter ever read?
 *
 * The walk itself lives in `astWalkExprChildren` / `astWalkStmtChildren` (`ast.c`) since
 * 2026-09-26: the AST kinds are listed **once** there, so a new kind cannot be forgotten here.
 * That is not hypothetical -- this question and seven siblings each carried their own copy of
 * the kind list, each ending in `default:`, so `-Wswitch` stayed silent while `EX_SLICE`'s two
 * bounds and `EX_DYN`'s payload went missing from all of them.
 *
 * The comparison is on the *generated* name, so a local that shadows the parameter (which gets
 * `__2` appended) cannot be mistaken for it.
 *
 * A callback returns false to stop the walk; "found" is carried by that early exit. */
typedef struct { const char *cname; } CnameCtx;

static bool cnameInStmt(void *ctx, Stmt *s);

/**
 * @brief 判断表达式是否引用生成名为 `cname` 的绑定。
 *
 * @param ctx  `CnameCtx *`，携带要比较的生成名。
 * @param e    表达式。
 * @return     回调语义：命中返回 false 以停止遍历，否则返回 true。
 */
static bool cnameInExpr(void *ctx, Expr *e) {
    CnameCtx *c = (CnameCtx *)ctx;
    if (e->kind == EX_IDENT)
        return !(planCName(e) && strcmp(planCName(e), c->cname) == 0);
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief `cnameInExpr` 的语句版遍历回调。
 *
 * @param ctx  `CnameCtx *`，携带要比较的生成名。
 * @param s    语句。
 * @return     回调语义：命中返回 false 以停止遍历，否则返回 true。
 */
static bool cnameInStmt(void *ctx, Stmt *s) {
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 判断语句是否读取了生成名为 `cname` 的绑定。
 *
 * 只用于一个问题：某个参数是否被读过。比较的是**生成名**，因此遮蔽参数的同名局部
 * （生成名带 `__2` 后缀）不会被误认。
 *
 * @param s      语句；NULL 返回 false。
 * @param cname  生成 C 名。
 * @return       读取过则 true。
 */
static bool stmtUsesCname(Stmt *s, const char *cname) {
    if (!s) return false;
    CnameCtx c = { cname };
    return !cnameInStmt(&c, s);
}

/**
 * @brief 这条语句里是否有调用会（传递地）走到接收 home arena 的被调者。
 *
 * @param body  语句。
 * @return      有则 `true`。
 */
static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body, reachesHomeArenaTaker); }

/**
 * @brief 精确那一问：这条语句里是否**直接**调用接收 home arena 的函数。
 *
 * 用的是共享 accessor `funcTakesHomeArena`，与代码生成问的是同一处拼写。
 *
 * @param body  语句。
 * @return      有则 `true`。
 */
static bool callsUsesHome(Stmt *body)  { return stmtCallsNeedsHome(body, funcTakesHomeArena); }

/* ---- how a "reaches X through its calls" property is closed over the call graph ----
 *
 * The rule is monotone and the lattice is finite, so iterating until a round changes nothing
 * yields the **least** fixed point -- which is the answer wanted, not merely a consistent one: a
 * larger fixed point would mark functions that cannot have the property and give back the
 * optimisations these passes exist for. Recursion and mutual recursion need no special case: a
 * cycle stays false until one of its members is reached from a body that really does have it, and
 * if none of them ever is, none of them can have it at all.
 *
 * This rule lives here, with the per-node criterion in `bodyReaches`, so changing it is a
 * change in one place. What legitimately differs between callers is only **which**
 * functions a round visits -- `makesPool` has an extra round over the generic instances -- and
 * that stays at the call site, where its reason is visible.
 *
 * The lazy path (`funcAllocates`) keeps its own recursion and memo: it has to answer *before*
 * these closures run, and it treats a function already on the stack as allocating, which is the
 * conservative direction for a question that is asked mid-check. Same criterion, same field; the
 * evaluation strategy is the only difference, and that difference is deliberate. */
typedef enum {
    REACH_HOME,     /* `FuncDef.needsHome`: the body reaches a callee that needs a home arena */
    REACH_POOL,     /* `FuncDef.makesPool`: the body may create a pool                       */
    REACH_ALLOC,    /* `FuncDef.allocState`: the body may allocate (also asked lazily)        */
    REACH_USES_HOME /* `FuncDef.usesHome`: the body really reads the home arena it is given  */
} ReachKind;

/* What a round needs: the module being closed, and the type table (`makesPool` resolves protocol
 * methods on generic instances through it). The closures never needed a `Checker *` -- their
 * criterion is `planCallee(e)`, which the checker filled in while the bodies were checked -- and the
 * pointer is here for one reason only: `EXTC_DBG_FX` counts the rounds this driver runs. */
typedef struct { Module *m; TypeTable *tt; Checker *c; } CloseCtx;

/**
 * @brief 读取一个函数在给定 reach 性质上的标志。
 *
 * @param f  函数定义。
 * @param k  要读的性质。
 * @return   该性质当前的值。
 */
static bool getReach(const FuncDef *f, ReachKind k) {
    switch (k) {
    case REACH_HOME:  return anNeedsHome(f);
    case REACH_POOL:  return planMakesPool(f);
    case REACH_ALLOC: return anAllocState(f) == 1;
    case REACH_USES_HOME: return planUsesHome(f);
    }
    return false;
}

/**
 * @brief 写入一个函数在给定 reach 性质上的标志。
 *
 * @param f  函数定义。
 * @param k  要写的性质。
 * @param v  新值。
 */
static void setReach(FuncDef *f, ReachKind k, bool v) {
    switch (k) {
    case REACH_HOME:  anSetNeedsHome(f, v); break;
    case REACH_POOL:  planSetMakesPool(f, v); break;
    case REACH_ALLOC: anSetAllocState(f, v ? 1 : 2); break;
    case REACH_USES_HOME: planSetUsesHome(f, v); break;
    }
}

/**
 * @brief 判断函数体是否**直接**触达给定性质的被调者。
 *
 * 传递部分由 `closeReach` 的固定点负责。
 *
 * @param f  函数定义。
 * @param k  要问的性质。
 * @return   直接触达则 true。
 */
static bool bodyReaches(FuncDef *f, ReachKind k) {
    switch (k) {
    case REACH_HOME:  return callsNeedsHome(f->body);
    case REACH_POOL:  return stmtMakesPool(f->body, true);
    case REACH_ALLOC: return stmtHasNew(f->body);   /* `funcAllocates` adds the callee half */
    case REACH_USES_HOME:
        /* Narrower than `needsHome` on purpose: `needsHome` is the escape question and is
         * deliberately wide (a `new` whose value is copied into an out-parameter marks the
         * function even though nothing escapes), while the hidden parameter is only needed when
         * the body really reads it. The placement pass decides that per site, so a site sitting
         * in the home arena counts, as does handing this function's home on to a callee. A
         * function whose home arena is never read is not marked, so it gets no unused hidden
         * parameter. */
        if (callsUsesHome(f->body)) return true;
        Vec as_ = anArenaSites(f);
        for (size_t i = 0; i < as_.len; i++)
            if (planArenaLevel(*(Expr **)vecAt(&as_, i)) == ARENA_HOME) return true;
        return false;
    }
    return false;
}

/**
 * @brief 反复跑同一轮闭包，直到某一轮什么都没改。
 *
 * 规则单调、格有限，所以得到的是**最小**不动点，而不只是某个一致解。
 *
 * @param cx     闭包上下文。
 * @param k      要闭合的属性。
 * @param round  跑一轮的函数；这一轮有变化就由它把 `*changed` 置真。
 */
static void closeReach(CloseCtx *cx, ReachKind k,
                       void (*round)(CloseCtx *cx, ReachKind k, bool *changed)) {
    for (bool changed = true; changed; ) {
        changed = false;
        round(cx, k, &changed);
        if (cx->c && cx->c->fxOn) cx->c->fxReachRounds++;
    }
}

/**
 * @brief 对模块的自由函数与各 struct 的方法做一轮 reach 传播。
 *
 * @param cx          关闭上下文。
 * @param k           要传播的性质。
 * @param changed     本轮有改动时置 true。
 * @param skipExtern  跳过 `extern!`：它没有函数体可走。
 */
static void roundOverModule(CloseCtx *cx, ReachKind k, bool *changed, bool skipExtern) {
    Module *m = cx->m;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (cx->c && cx->c->fxOn) cx->c->fxReachVisits++;
        if (getReach(f, k) || !f->body || (skipExtern && f->isExtern)) continue;
        if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (cx->c && cx->c->fxOn) cx->c->fxReachVisits++;
            if (getReach(f, k) || !f->body) continue;
            if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
        }
    }
}

/**
 * @brief `needsHome` 的一轮传播：走模块的自由函数与方法，跳过 extern。
 *
 * @param cx       关闭上下文。
 * @param k        性质（`REACH_HOME`）。
 * @param changed  本轮有改动时置 true。
 */
static void roundNeedsHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);      /* an extern has no body to walk */
}

/**
 * @brief `usesHome` 的一轮传播：走模块的自由函数与方法，跳过 extern。
 *
 * @param cx       关闭上下文。
 * @param k        性质（`REACH_USES_HOME`）。
 * @param changed  本轮有改动时置 true。
 */
static void roundUsesHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);
}

static void roundMakesPool(CloseCtx *cx, ReachKind k, bool *changed);   /* defined below */

/* ---- Does this code create a pool? (`FuncDef.makesPool`) ----
 *
 * The runtime primitive that creates one is `extc_pool_new`, and it is the *only* one:
 * `drop`/`reset`/`generation`/`live`/`capacity`/`zoneDepth` neither create a record nor
 * need a zone to exist. So the whole predicate rests on finding calls to that one name,
 * directly or through a callee that reaches it.
 *
 * Two callers ask two different questions with it:
 *   - the fixed-point closure below asks the transitive one ("can this function create a
 *     pool at all?"), and walks nested blocks, which each have a zone of their own;
 */
/**
 * @brief 数一个函数体里 `@overwrite` 站点的个数。
 *
 * 顺序必须与 codegen 的 `collectOwSites` 一致：两边按源码序遍历同一棵树，第 n 个站点对应
 * 第 n 个格子。
 *
 * @param s  函数体。
 * @return   `@overwrite` 绑定的个数。
 */
static int countOwSites(Stmt *s) {

    if (!s) return 0;
    switch (s->kind) {
    case ST_VAR:  return s->u.var.overwrite ? 1 : 0;
    case ST_BLOCK: {
        int n = 0;
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            n += countOwSites(*(Stmt **)vecAt(&s->u.block.stmts, i));
        return n;
    }
    case ST_IF:    return countOwSites(s->u.ifs.thenBody) + countOwSites(s->u.ifs.elseBody);
    case ST_WHILE: return countOwSites(s->u.whiles.body);
    case ST_MATCH: {
        int n = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            n += countOwSites((*(MatchArm **)vecAt(&s->u.match.arms, i))->body);
        return n;
    }
    default: return 0;
    }
}

/**
 * @brief 判断函数是否能通过被调者回到自身。
 *
 * 能重入自身的函数需要每个激活一个 `@overwrite` 格子：共用一个格子时，嵌套调用会覆盖仍在
 * 运行的那个激活的存储。保守方向是“是”：被调者记录为 NULL 也答 true，代价只是每个激活多
 * 一个格子；走超过 64 跳即停并答 true，环不会溢出栈。
 *
 * @param c      未使用；遍历只读 `FuncDef` 里记录的调用图。
 * @param f      要测的函数。
 * @param depth  已走过的调用图跳数，用于环守卫。
 * @return       能回到自身或无法判定时 true。
 */
static bool funcReachesItself(Checker *c, FuncDef *f, int depth) {
    if (depth > 64) return true;                      /* guard against a cycle: assume re-entrant */
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (!g) return true;
        if (g == f) return true;
        if (funcReachesItself(c, g, depth + 1)) return true;
    }
    return false;
}

/**
 * @brief 取一个 place 表达式根标识符的名字。
 *
 * 需要它才能判定 `*head = cell`、`l.head = cell`、`l.buf[i] = cell` 是否落在参数侧。
 * 引用型绑定（`?ref node`）不能再借出，所以出参约定上包一层 struct。
 *
 * @param e  place 表达式；NULL 允许。
 * @return   根标识符的名字；不是 place（字面量、调用等）时 NULL。
 */
const char *placeRootName(Expr *e) {
    /* 问题④：根的**语法名字**（诊断与老接口）。掩码 = FIELD|INDEX|DEREF，不解析 ——
     * 所以它对未解析的节点也能用，这正是它与 `placeRoot` 的关键差别。*/
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF, 0);
    return leaf ? leaf->u.ident.name : NULL;
}
/**
 * @brief 判断名字 `n` 是否为 `f` 的参数。
 *
 * @param f  要搜索的函数。
 * @param n  要查找的名字。
 * @return   有同名参数则 true。
 */
static bool isParamName(FuncDef *f, const char *n) {
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, n) == 0) return true;
    return false;
}

/**
 * @brief 判断一个值表达式是否可能携带引用。
 *
 * 不要写成两个互相调用的函数：两个问题（类型能否带引用 / 表达式的结果能否带引用）都由这
 * 一个递归函数回答，每种表达式只在自己的分支里递归。
 *
 * @param v  值表达式；NULL 答 false。
 * @return   可能携带活引用时 true。答案过近似：不透明形状答 true，最多多要一个 home arena，
 *           但不会漏掉真正存在的引用。
 */
static bool valueMayCarryRef(Expr *v) {
    if (!v) return false;
    switch (v->kind) {
    /* It is itself a reference or a view. */
    case EX_REF: case EX_SLICE:
    case EX_DEREF:      /* the storage `*p` points at may hold a reference (conservative) */
    case EX_INDEX: case EX_FIELD:   /* `a[i]` / `o.f` may read out a reference */
        return true;
    /* These shapes are decided by what they hold. */
    case EX_IDENT:      return true;      /* a binding: unknown content => conservative */
    /* `dyn Trait(x)`: the payload is copied into the pool, so the references it carries are
     * carried by this value (family E). */
    case EX_DYN:        return valueMayCarryRef(v->u.dynv.payload);
    case EX_SIGN:       return valueMayCarryRef(v->u.sign.operand);
    case EX_COALESCE:   return valueMayCarryRef(v->u.coalesce.main)
                            || valueMayCarryRef(v->u.coalesce.fallback);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < v->u.lit.inits.len; i++)
            if (valueMayCarryRef((*(FieldInit **)vecAt(&v->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < v->u.arraylit.elems.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < v->u.enumval.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.enumval.args, i))) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < v->u.assoc.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.assoc.args, i))) return true;
        return false;
    case EX_CALL:
        for (size_t i = 0; i < v->u.call.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (valueMayCarryRef(v->u.method.recv)) return true;
        for (size_t i = 0; i < v->u.method.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.method.args, i))) return true;
        return false;
    default: return false;   /* scalar literal / `new` (its depth has its own case) */
    }
}

/**
 * @brief 判断函数体是否向经由参数到达的地方写入。
 *
 * 与 `valueMayCarryRef` 一起决定函数是否需要 home arena：经由出参的写入
 * （`fn push(head: mut ref ?ref node) { *head = cell }`）必须把分配放进调用者的 arena。
 *
 * @param c  checker。
 * @param s  要遍历的语句；可为 NULL。
 * @param f  其参数名界定写入目标的函数。
 * @return   有赋值命中以参数为根的地方、且存入值可能携带引用时 true。
 */
static bool stmtStoresThroughDeref(Checker *c, Stmt *s, FuncDef *f) {
    if (!s) return false;
    switch (s->kind) {
    case ST_ASSIGN: {
        const char *rn = placeRootName(s->u.assign.target);
        /* Writing into the place a parameter points at requires the stored value to
         * outlive this frame only when the value may carry a reference. */
        return rn && isParamName(f, rn) && valueMayCarryRef(s->u.assign.value);
    }
    case ST_IF:     return stmtStoresThroughDeref(c, s->u.ifs.thenBody, f) ||
                           stmtStoresThroughDeref(c, s->u.ifs.elseBody, f);
    case ST_WHILE:  return stmtStoresThroughDeref(c, s->u.whiles.body, f);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtStoresThroughDeref(c, *(Stmt **)vecAt(&s->u.block.stmts, i), f)) return true;
        return false;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtStoresThroughDeref(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body, f))
                return true;
        return false;
    default: return false;
    }
}

/* The argument-lifetime rule: whatever an argument points at must live at least as long
 * as the home arena of the call (the arena the caller passes in).
 *
 * Why it is needed: the callee may store the argument into its own home arena, and the
 * chained argument behind that ("an allocation that outlives every writable target can
 * be stored anywhere safely") holds only for arguments that live long enough. If the
 * caller passes a deeper local, the home arena outlives it and the stored reference
 * dangles.
 *
 *     fn bind(s: mut ref slot, target: mut ref i32) { s.r = target }
 *     var keeper: slot                              // depth 1
 *     { var n: i32 = 1  bind(ref keeper, ref n) }   // n has depth 2 > h = 1, so the
 *                                                   // call site reports an error
 */

/**
 * @brief 闭合并缓存一个函数的效应摘要。
 *
 * 三条纪律：惰性 + 记忆化（`effState`）；环守卫（`EFF_IN_PROGRESS` 视为环，标为不完整）；
 * 未解析的调用（`effUnknown`）同样标为不完整。只有本函数返回 true，才允许据摘要放宽检查。
 *
 * @param c  checker。
 * @param f  摘要被闭合并缓存的函数；NULL 不可用。
 * @return   摘要完整时 true；有环、未解析调用或不完整的被调者时 false，此时被调者必须当作
 *           可能存下任何东西。
 */
bool computeEffectsTransitive(Checker *c, FuncDef *f) {
    if (!f) return false;
    /* An external declaration's summary is the signature that was collected for it
     * (`collectEffects` has already filled it in), so it counts as complete: we choose
     * to trust the declaration. Never let the absence of a body turn into an empty
     * summary.
     */
    if (f->isExtern) return true;
    if (anEffState(f) == EFF_DONE) { if (c->fxOn) c->fxEffCached++; return anEffComplete(f); }
    if (anEffState(f) == EFF_IN_PROGRESS) { anSetEffComplete(f, false); return false; }   /* a cycle => incomplete */
    anSetEffState(f, EFF_IN_PROGRESS);
    if (c->fxOn) c->fxEffCalls++;      /* one body entered (a memo hit returned above) */
    bool complete = !anEffUnknown(f);
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (c->fxOn) c->fxEffEdges++;  /* one callee edge merged */
        if (g == f) { complete = false; continue; }                   /* self-call: conservative */
        if (computeEffectsTransitive(c, g)) {                         /* merge the callee summary */
            anSetAddrMask(f, anAddrMask(f) | anAddrMask(g));
            anSetContMask(f, anContMask(f) | anContMask(g));
            anSetHomeAddrMask(f, anHomeAddrMask(f) | anHomeAddrMask(g));
            anSetHomeContMask(f, anHomeContMask(f) | anHomeContMask(g));
            anSetOtherMask(f, anOtherMask(f) | anOtherMask(g));
            anSetAddrFromLocal(f, anAddrFromLocal(f) || anAddrFromLocal(g));
        } else {
            complete = false;
        }
    }
    anSetEffComplete(f, complete);
    anSetEffState(f, EFF_DONE);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects-closed] %-20s complete=%d toParam[Addr=0x%llx Cont=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] other=0x%llx\n",
                FN(f), (int)anEffComplete(f), (unsigned long long)anAddrMask(f), (unsigned long long)anContMask(f),
                (unsigned long long)anHomeAddrMask(f), (unsigned long long)anHomeContMask(f), (unsigned long long)anOtherMask(f));
    return complete;
}

/**
 * @brief 检查一次调用的引用实参能否活过这次调用，并据此定下 arena 的层。
 *
 * 实参与形参个数不一致时直接返回。extern 被调者是 C 侧的黑盒：按签名里的 effects 位
 * 逐位判断哪个参数可能被保存，被保存的引用实参必须活到层 0。有函数体的被调者先做效果
 * 闭包，再分别判地址流与内容流，两个判据各自决定是否运行；可提升的值先尝试提升，提不动
 * 才报错。`mut ref` 实参是写权限，其非空证明在这里作废。
 *
 * @param c          检查器。
 * @param callee     被调函数定义；NULL 表示被调者未解析，按保守处理。
 * @param args       实参表达式表。
 * @param params     被调函数的形参表，顺序与 `args` 一致。
 * @param homeDepth  被调者声明的 arena 层；小于等于 0 表示没有显式声明。
 * @param line       报错用的源码行。
 * @param fname      被调者的名字，用于诊断。
 */
void checkCallRefArgs(Checker *c, FuncDef *callee, Vec *args, Vec *params, int homeDepth,
                       int line, const char *fname) {
    if (params->len != args->len) return;
    /* An argument only has to outlive the home arena when an address really flows.
     *
     *   - every `Addr` bit is clear in the effect summary => the callee never stored
     *     `&argument`, so the constraint does not exist. This is the `push`-like callee
     *     that only puts new things into a container, and it is still handled as the
     *     worst case today.
     *   - the answer is uncertain (an incomplete summary, or a non-empty `otherMask`)
     *     => fall back to the conservative rule.
     */
    /* This rule was narrowed once ("every `Addr` bit clear => skip the rule") and the
     * two-way criterion rejected it on the spot:
     *
     *   - evidence: `tests/errors/ref_arg_too_deep` went from "must report an error" to
     *     passing;
     *   - root cause: the effect summary had no transitive closure, so a store made one
     *     call deeper was invisible => while the summary is incomplete, "every `Addr`
     *     bit clear" is false => narrowing the rule is the same as allowing a dangling
     *     reference;
     *   - discipline: until the summary is provably complete, the argument-lifetime rule
     *     stays conservative (rejecting a safe program is better than missing undefined
     *     behavior).
     *
     * The narrowing becomes legal only with the lazy transitive closure (memo plus cycle
     * guard), and only for a summary that can be proven complete.
     */
    /* Only a summary that is proven complete may skip the conservative checks below on
     * the grounds that no address flows. */
    /* An external declaration takes a separate path: it is a black box on the C side,
     * and the effects written in its signature are the whole basis for it. "It may
     * store the argument" means that storage may live beyond this frame (C can put it
     * in a global or a static), so an argument at that position must live to depth 0.
     *
     * Without a signature every parameter counts as "stores it", so passing a local is
     * rejected: safe by default. With `effects Addr=0 Cont=0` no parameter is checked
     * at all, which is what makes such a declaration usable.
     */
    if (callee && callee->isExtern) {
        uint64_t stored = anAddrMask(callee) | anContMask(callee) | anOtherMask(callee);
        if (stored == 0) return;                 /* the signature says nothing is stored */
        for (size_t j = 0; j < params->len && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || p->type->kind != TY_REF) continue;   /* a scalar has no lifetime */
            Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
            int d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, a);
            if (d == 0) continue;                /* it already outlives this frame */
            ckError(c, line,
                    "A C function is a black box: unless its declaration says it does not keep the"
                    " pointer, it may store it somewhere that outlives this frame. Sign it --"
                    " `effects Addr=0 Cont=0` on the `extern!`, or on the table slot that holds the"
                    " function pointer -- or pass something that lives longer.",
                    "argument %zu of `%s` may be kept by C forever, but it points into this"
                    " frame (depth %d)", j + 1, fname, d);
        }
        return;
    }

    bool complete = callee && computeEffectsTransitive(c, callee);
    /* The summary this check is about to use -- the switch that answers "why was this call
     * site rejected / why did it pass". Same shape as `EXTC_DBG_ZONE` / `EXTC_DUMP_EFFECTS`. */
    if (getenv("EXTC_DBG_REFARGS"))
        fprintf(stderr, "[refargs] %-16s mod=%-8s line=%-5d complete=%d"
                        " addr=0x%llx cont=0x%llx other=0x%llx homeAddr=0x%llx homeCont=0x%llx homeDepth=%d\n",
                fname, callee && callee->modName ? callee->modName : "-", callee ? callee->line : -1,
                (int)complete, callee ? (unsigned long long)anAddrMask(callee) : 0,
                callee ? (unsigned long long)anContMask(callee) : 0,
                callee ? (unsigned long long)anOtherMask(callee) : 0,
                callee ? (unsigned long long)anHomeAddrMask(callee) : 0,
                callee ? (unsigned long long)anHomeContMask(callee) : 0, homeDepth);
    /* The two checks decide independently whether to run; do not give them one shared
     * early return. One shared return makes an over-strict combination possible: a `Cont`
     * bit on `push` made the address-flow check run as well, and `examples/list-return`
     * was wrongly rejected. On the escape-analysis path, `homeDepth = -1 => h = 0` is too
     * strict for a local `mut ref` argument.
     */
    bool addrMaybe = !complete || !callee
                   || anAddrMask(callee) != 0 || anHomeAddrMask(callee) != 0
                   || anAddrFromLocal(callee) || anOtherMask(callee) != 0;
    bool contMaybe = !complete || !callee
                   || anContMask(callee) != 0 || anHomeContMask(callee) != 0
                   || anAddrMask(callee) != 0 || anHomeAddrMask(callee) != 0
                   || anOtherMask(callee) != 0;
    if (!addrMaybe && !contMaybe) return;
    /* The bound is the DESTINATION's own depth (below), falling back to the
     * caller's body depth. A `needsHome => 0` or `homeDepth < 0 => 0` guess collapses to
     * "must live in the home arena" for any argument living in any block, which rejects
     * safe programs: `homeDepth < 0 => 0` is too strict for a local `mut ref` argument. A
     * destination that is really home-promoted reports depth 0, so h = 0 still happens when
     * it must. */
    int h = (homeDepth > 0) ? homeDepth : (int)c->scopes.len;
    /* PRECISION: the destination is one of the `ref`-typed arguments. The callee may store a
     * value it received into any of them, so that value has to outlive every one of them:
     * take the shallowest (longest-lived) destination, never a deeper one than the caller's
     * own body. This replaces the old "the caller owns a home arena => h = 0" fallback,
     * which rejected every argument that lived in any block at all.
     *
     * min() can only make the requirement STRICTER than the destination really demands, so
     * nothing that was rejected for a real lifetime reason becomes accepted; the guards stay
     * `tests/errors/ref_arg_too_deep` and `tests/errors/borrowed_into_param_place`, and a
     * destination that lives forever (depth 0) still forces depth 0 on the source. */
    /* Only when the callee did NOT declare its arena (`homeDepth > 0` is an explicit
     * declaration and stays authoritative -- narrowing it further rejected four examples:
     * examples/borrowing, examples/container-of-view, examples/stream-sum,
     * tests/traps/main_question). */
    if ((addrMaybe || contMaybe) && homeDepth <= 0) {
        bool haveDest = false;
        for (size_t i2 = 0; i2 < params->len && i2 < args->len; i2++) {
            Param *p2 = *(Param **)vecAt(params, i2);
            if (!p2->type || p2->type->kind != TY_REF) continue;
            /* Only a reference to something that can **hold** references can be the destination a
             * callee stores into (a container, a plate, a record with ref fields). A `ref u8` is
             * not one: counting it lets a depth-0 argument (a view of a global) collapse the
             * destination to "lives forever", which rejects the *callee's own table*.
             * The predicate is the framework's own "this type can carry a reference". */
            if (!typeContainsRef(c->tt, tsub(c, p2->type->inner))) continue;
            Expr *a2 = *(Expr **)vecAt(args, i2);
            Expr *place2 = (a2->kind == EX_REF) ? a2->u.ref.operand : a2;
            /* NOTE: a generic parameter (`mut ref table<T>`) is NOT skipped here, unlike in
             * the bit-mask loops: this bound only ever LOWERS h (stricter), and the place's
             * depth does not depend on T. Skipping it left "no measurable destination" and
             * fell back to the strict 0, which is what kept rejecting the safe call. */
            /* EXACTLY the expression the store rules use for the same argument below
             * (`placeRoot ? slotDepth : targetDepth`). Mixing the two queries is what made
             * earlier attempts collapse h to 0: for a container `slotDepth` answers about
             * the 40-byte handle (1) while `targetDepth` answers about its storage, the pool
             * plate, which is born in the home arena and therefore reports 0. */
            int d2 = placeRoot(c, place2) ? slotDepth(c, place2) : targetDepth(c, place2);
            if (d2 == 0) { h = 0; haveDest = true; break; }   /* lives forever: the strictest */
            if (!haveDest || d2 < h) h = d2;
            haveDest = true;
        }
        /* Nothing measurable to store into, or a summary that is not proven complete: stay
         * strict, exactly as before this change. */
        if (!haveDest && (homeDepth < 0 || !complete)) h = 0;
    }

    for (size_t i = 0; addrMaybe && i < params->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* generic: defer */
        /* An argument is not always a place (the typical case: `stash(ref l, new node)`
         * passes a `new` directly). `slotDepth` answers 0 for a non-place, and that
         * number is read as "lives forever", so the rule would check nothing. The
         * lifetime of such an argument is taken from the level of the arena block it was
         * allocated in. */
        int d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, place);
        /* When the value is out of reach, first try to promote it (the same rule as on
         * assignment edges): if it cannot be promoted, the error below stands as it is;
         * if it can, the value really does live to this level. */
        if (d != 0 && d > h && promoteInto(c, place, h))
            d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, place);
        if (d == 0 || d <= h) continue;              /* lives long enough */
        ckError(c, line,
                "The callee may store this reference into the arena it was given, so the"
                " argument must live at least that long. Move it to a shallower scope.",
                "argument %zu of `%s` points into a deeper scope (depth %d) than the arena"
                " this call may store it in (depth %d)", i + 1, fname, d, h);
    }

    /* Content flow: the callee stores into a container either a pointer read out of
     * argument j or a value that carries references, so that data must live at least to
     * the level h of the destination as well.
     *
     * This is the borrow rule moved to the call site: the callee is compiled once and
     * does not know the caller's pool, so it only publishes the constraint, and the
     * substitution that solves it happens here.
     *
     *   - a complete summary => check only the j the summary names;
     *   - an incomplete summary (a cycle, or a call that cannot be resolved) or a
     *     non-empty `otherMask` => check every argument that carries a reference, as the
     *     worst case.
     *
     * This half is inseparable: without it, relaxing anything on the callee side would
     * be the same as allowing a dangling reference. That is the lesson from the earlier
     * attempt to narrow the argument-lifetime rule.
     */
    if (contMaybe && (!complete || (callee && anOtherMask(callee) != 0))) {
        for (size_t j = 0; j < args->len; j++) {
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;                  /* generic: defer */
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue; /* no reference: always true */
            /* 先把"它得活到 h"这件事记成事实，**不管当下算出来的 d 是多少**：
             * `d` 是现场算的，而"这个值是不是建池的调用"（`makesPool`）要到所有函数体检查完
             * 才算得出来 —— 现场它还是 0，于是深度查询给出 0，提权那条路根本不会被走到。
             * 末轮重放（`c->lvlFacts`）在闭包之后跑，那时 `makesPool` 已经为真，
             * `promoteInto` 就能把建池的站点提到 h 那一层。重放忽略返回值，所以多记事实
             * 不会引入新报错（"提不动"的那些照旧由下面的深度判据负责）。 */
            recordLvlFact(c, a, h);
            int d = targetDepth(c, a);
            if (d != 0 && d > h && promoteInto(c, a, h)) d = targetDepth(c, a);
            if (d == 0 || d <= h) continue;
            ckError(c, line,
                    "Nothing was stored here that could be checked at this call, so the"
                    " compiler has to assume the worst: this value may end up in the place"
                    " the callee stores into. Make it live at least that long (or give the"
                    " callee a decidable body).",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d); the callee's"
                    " effects could not be fully analyzed", j + 1, fname, d, h);
        }
    } else if (contMaybe && callee) {
        /* All four bit classes must be checked here: the summary may record a by-value
         * view or a by-value argument that carries a reference as either an address-flow
         * bit or a content-flow bit, because the collector classifies by whether the
         * expression itself is a pointer or a view. For a by-value argument the two cases
         * reduce to one statement: the data it carries must live at least h.
         *
         * Checking only the content-flow bits would accept `names.push(buf[..])` with
         * `buf` declared in a deeper block, which is a real dangling pointer. */
        uint64_t cont = anContMask(callee) | anHomeContMask(callee)
                      | anAddrMask(callee) | anHomeAddrMask(callee);
        for (size_t j = 0; j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((cont >> j) & 1u)) continue;
            Param *pj = *(Param **)vecAt(params, j);
            if (pj->type && pj->type->kind == TY_REF) continue;   /* `ref` arg: checked above */
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue;
            recordLvlFact(c, a, h);         /* 同上：先记事实，末轮重放时再提 */
            int d = targetDepth(c, a);
            /* PROMOTE BEFORE REJECTING -- the same half the incomplete-summary branch above
             * has always had, and the reason a container can be handed to something that
             * outlives it.
             *
             * A **fresh allocation** has no other owner, so its site can be moved to the
             * level the destination needs: `stash(ref b, p)` with `p = new i32[4]` in a
             * deeper block is safe once that `new` is promoted to `b`'s level. A **borrow**
             * cannot be moved (`names.push(buf[..])` where `buf` is a deeper block's array),
             * and `promoteInto` says so by returning false -- then the error below stands,
             * which is exactly the case this branch was added for.
             *
             * Sharing this half matters for pools as much as for arenas: a container built
             * inside a loop body and pushed into a container that outlives the loop needs its
             * pool born at the destination's place. */
            if (d != 0 && d > h && promoteInto(c, a, h)) d = targetDepth(c, a);
            if (d == 0 || d <= h) continue;
            ckError(c, line,
                    "The callee stores what this value points at into a container it was"
                    " given, so that data must live at least as long as that container.",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d)", j + 1, fname, d, h);
        }
    }

    /* ---- : a `mut ref` argument is write access, so the proofs about it are stale --------
     * The callee may rewrite what the place holds, so every non-null proof about that binding --
     * and about a path through it -- stops holding: `if s.top != null { s.pop() ... s.top.value }`
     * dereferenced a NULL `s.top`, because `pop` had set it and the proof was still in effect
     * (ASan SEGV). This is the write-point invalidation of the proof stack, the same
     * one an assignment performs (`unNarrow`); the stack drops a path fact when its root is
     * dropped, so un-narrowing the root is enough.
     *
     * The condition is `mut ref`, not "the summary says it stores": a `mut ref` parameter is write
     * access by definition, and the summary is an upper bound on stores into the caller's
     * containers, not on writes through the reference. */
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        Sym *rs = placeRoot(c, place);
        if (rs) unNarrow(c, rs->cname);
    }

    /* ---- INV-K: the other half of a store, on the caller's side ---------------------------
     * A store has two halves and the second one was missing: the **destination** has to learn that
     * something was written into it, or the caller's binding goes on looking like a record with no
     * live references. `fn stash(out: mut ref box, v: ?ref node) { out.p = v }` called as
     * `stash(ref b, ref n)` and then `return b` was accepted, and the generated program read a
     * local of the returning frame after it died (ASan heap-use-after-free).
     *
     * The summary says *which argument* was stored, never into which field, so the write goes to
     * the binding's whole-object slot (`field == NULL`). The destination is normally **addressed**
     * here -- `ref b` is what takes its address -- which is not a reason to skip it: the depth
     * update is a weak max, and `promoteFieldsAt` now promotes for an addressed root while refusing
     * to lower it. No `recordStore` here: it constrains the level solver and was the suspected
     * source of two false rejections (R1R3-LOG 补8/补9). */
    if (callee && (addrMaybe || contMaybe)) {
        uint64_t stored = anAddrMask(callee) | anHomeAddrMask(callee)
                        | anContMask(callee) | anHomeContMask(callee);
        for (size_t j = 0; stored && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *src = *(Expr **)vecAt(args, j);
            if (mentionsParam(src->type)) continue;
            Expr *splace = (src->kind == EX_REF) ? src->u.ref.operand : src;
            int dj = targetDepth(c, splace);
            if (dj == 0 && placeRoot(c, splace)) dj = slotDepth(c, splace);
            /* not skipped: for a reference-typed argument the level is not filled in yet */
            for (size_t i = 0; i < params->len && i < args->len; i++) {
                if (i == j) continue;
                Param *pp = *(Param **)vecAt(params, i);
                if (!pp->type || pp->type->kind != TY_REF || !pp->type->mut) continue;
                Expr *a = *(Expr **)vecAt(args, i);
                Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
                Sym *dsym = placeRoot(c, place);
                if (!dsym) continue;
                if (dj != 0) {
                    /* 不给 `dsym` 记 `otherSrc`：这里只知道"哪个实参被存了"，不知道存进哪个
                     * 字段；记下的来源没有对应的目的地要求（`otherMinReq` 会停在 0 = 必须活过
                     * 本帧），于是 `promoteInto` 在 `at > 0` 时反而失败、把正常形状误拒。
                     * 这里只发布 `callSrc`（它有自己的深度），要提权时沿它走。 */
                    Expr *saveCsv = c->curStoreVal;
                    c->curStoreVal = NULL;   /* 只更新深度，不要顺带记来源 */
                    noteFieldDepthWrite(c, dsym, NULL, dj);
                    c->curStoreVal = saveCsv;
                }
                /* The dedicated slot (). No `recordLvlFact` here: constraining the level
                 * for *every* `mut ref` call was the first suspect for the three false rejections the
                 * full version produced (`tests/pool` rt_nest/rt_rett, `tests/stl` custom). */
                /* Publish the dedicated slot when the level is known, **or** when the source is a
                 * promotable fresh allocation -- those are the shapes whose site can really be moved,
                 * which is what `return b` needs (`var n: mut ref node = new node`).
                 * An unknown level whose source cannot be promoted gets no slot: publishing it added
                 * promotion pressure and nothing else, and that is what rejected `tests/pool`'s
                 * rt_nest/rt_rett and `tests/stl`'s custom when the publication was unconditional. */
                Sym *ss = placeRoot(c, splace);
                bool promotable = ss && ss->origin && exprIsFresh(ss->origin);
                /* `callSrc` 也存**压平后的根**：容器元素在别的作用域里被取出、提权时那个
                 * 中间绑定（`p`）早就出了作用域，`lookup` 查不到，站点就提不动。 */
                Expr *csrc = splace;
                if (splace && splace->kind == EX_IDENT) {
                    Expr *r = originOf(c, splace, 0);
                    if (r) csrc = r;
                }
                if (dj != 0 || promotable) {
                    if (!dsym->callSrc) {
                        dsym->callSrc = csrc;
                        /* 门槛从来源现在的深度起步，不是 0（"必须活过本帧"）：来源现在至少
                         * 活到 `dj`，之后 `promoteFieldsAt` 只会收得更紧。从 0 起步会让
                         * `link(prev, n)` 之后的 `prev = n` 去提一个不需要提到 home 的站点、
                         * 答 false，把正常形状误拒。 */
                        dsym->callMinReq = dj;
                    }
                    if (dj > dsym->callSrcDepth) dsym->callSrcDepth = dj;
                }
            }
        }
    }

}

/**
 * @brief 按调用结果是否离开本块来为该调用选定 arena。
 *
 * 结果存在当前块里传当前块的 arena，每轮随块回收、峰值恒定；存在更浅的地方或返回时传本函数
 * 的 home arena；实参位置刻意不标记，回落到 home arena。`homeDepth` 与 `arenaArg` 必须一起
 * 更新：codegen 只读 `arenaArg`。
 *
 * @param c   checker（作用域栈）。
 * @param v   结果被存下的调用表达式；其它形状忽略。
 * @param at  接收结果的 place 的作用域深度；0 表示结果被返回，必须活过本帧。
 */
void markCallHomeIfEscaping(Checker *c, Expr *v, int at) {
    if (!v) return;
    if ((v->kind != EX_CALL && v->kind != EX_METHOD) || !planCallee(v)) return;
    if (!anNeedsHome(planCallee(v))) return;
    anSetHomeDepth(v, (at < (int)c->scopes.len) ? -1 : (int)c->scopes.len);
    setCallArenaArg(c, v);          /* always keep `homeDepth` and `arenaArg` in step */
}

/**
 * @brief 判断一个被调者会不会创建池（读取处统一走这里）。
 *
 * 泛型方法的模板与实例是两份 `FuncDef`，闭包只走到模板那一份，而调用点上 `e->func` 可能是
 * 模板。`@poolObject` 声明优先于推断；结构体的 `makesPoolAny` 只对关联函数（`new` /
 * `withCap` / `withParent` 那一族）放宽，别的库方法不进这一族，免得把不可提权的值误判成站点。
 *
 * @param f  被调者；NULL 返回 false。
 * @return   可能创建池时 true。
 */
bool calleeMakesPool(FuncDef *f) {
    if (!f) return false;
    /* **声明优先**：`@poolObject` 标在 struct 上，说"这个类型拥有一个池"。它比推断可靠：
     * 看有没有 `pid` 字段是魔数，看方法建不建池要等闭包。 */
    if (f->owner && f->owner->poolObject) return true;
    if (planMakesPool(f)) return true;
    /* 运行期那一句**本身**就是池站点：`extc_pool_new`（extern，没有函数体 ⇒ 闭包标不到它，
     * 所以它按名字认，与 codegen 里把 `extc_pool_new` 改写成 `_at` 的那处同一个名字）。
     *
     * 少了这一条的后果：用户**在自己文件里**写一个池底容器时，构造器里的这一次
     * `extc_pool_new` 提不了权 ⇒ 池生进构造器自己的帧 ⇒ 返回时被回收 ⇒ 段错误。
     * 库里的容器没这个毛病，只因为库函数"对地方透明"；用户的文件不是库，
     * 于是那条隐藏的差别就露出来了。 */
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return anIsAssoc(f) && f->owner && anMakesPoolAny(f->owner);
}

/* 闭包实例那一轮的代入上下文。 */
static TypeTable *instResTT = NULL;
static Vec *instResParams = NULL;
static Vec *instResTargs = NULL;
/**
 * @brief 在闭包实例那一轮的代入上下文里重新解析一个方法调用点。
 *
 * @param e  调用表达式。
 * @return   代入后找到的方法定义；没有实例上下文、或不是带接收者的方法调用时返回 NULL。
 */
static FuncDef *resolveOnInstance(Expr *e) {
    if (!instResTT || !instResParams || !instResTargs) return NULL;
    if (!e || e->kind != EX_METHOD || !e->u.method.recv) return NULL;
    Type *rt = ttSubstitute(instResTT, e->u.method.recv->type, instResParams, instResTargs);
    return findMethod(ttBase(rt), e->u.method.name);
}


static bool exprMakesPool(Expr *e, bool descendBlocks);   /* defined with the pool questions at the end */

/**
 * @brief 确定一个建池调用点所在的 zone 层。
 *
 * 库函数（`modName` 非空）对“地方”透明，没有自己的地方可言，把选择权交给调用者
 * （`ZONE_HOME`）；否则默认当前块。只往下调、不往上抬：同一节点可能被检查多次，而提权已经
 * 把它改小，再按当前块覆盖就会抹掉提权（越小越长寿）。
 *
 * @param c  checker（作用域栈）。
 * @param e  调用表达式；NULL 忽略。
 */
void setCallZoneArg(Checker *c, Expr *e) {
    if (!e) return;
    int lvl = (c->curFunc && c->curFunc->modName && *c->curFunc->modName)
                  ? ZONE_HOME : (int)c->scopes.len;
    /* **只往下调，不往上抬**：同一个节点可能被检查不止一次（泛型实例、末轮那一类重走），
     * 而提权已经把这一格改小了；再按"当前块"覆盖一遍就等于把提权抹掉。
     * 越小越长寿，所以取更小的那个。 */
    if (planZoneLevel(e) == 0 || lvl < planZoneLevel(e)) planSetZoneLevel(e, lvl);
}

/**
 * @brief 把 `homeDepth` 解成调用实际传入的 arena（`Expr.arenaArg`）。
 *
 * 决策只在这里做，codegen 只打印，不得回落到按函数计的 home 标志。没有 `mut ref` 实参给出
 * 理由时，把该站点按当前块记下并置 `arenaArgPending`，由末轮落定。
 *
 * @param c  checker（作用域栈）。
 * @param e  已设好 `homeDepth` 的调用表达式；NULL 忽略。
 */
void setCallArenaArg(Checker *c, Expr *e) {
    if (!e) return;
    if (anHomeDepth(e) == -1) {                 /* destination at the home level: my home arena */
        planSetArenaArg(e, ARENA_HOME);
        e->arenaArgPending = false;
    } else if (anHomeDepth(e) >= 1) {           /* an explicit block level: use that arena */
        planSetArenaArg(e, anHomeDepth(e));
        e->arenaArgPending = false;
    } else {
        /* No `mut ref` argument gave a reason, so the fallback applies: pass my home
         * arena if I have one, otherwise the current block.
         *
         * Whether I have a home arena is decided by the transitive closure of
         * `needsHome`, which is only known after every function body has been checked.
         * The site is therefore recorded as the current block with `arenaArgPending`
         * set, and the final pass settles it (see `curArenaSites`).
         *
         * The decision lives here so it is made in exactly one place: codegen reads
         * `arenaArg` and decides nothing on its own. */
        planSetArenaArg(e, (int)c->scopes.len);
        e->arenaArgPending = true;
        *(Expr **)vecPush(&c->curArenaSites) = e;   /* the final pass comes back to this site */
    }
}

/**
 * @brief 从 `mut ref` 实参读出调用要传的 arena 深度。
 *
 * 被调者的分配至少要活过它能存进去的任何东西，所以最浅的 `mut ref` 实参所指对象决定 arena。
 * 实参是自身参数或全局时已经活过本帧，改传调用者的 home arena。
 *
 * @param c         checker。
 * @param args      按调用顺序排列的实参表达式。
 * @param params    被调者的声明参数，顺序相同。
 * @param callNode  调用表达式；答案依赖逃逸分析时记录它，可为 NULL（不记录）。
 * @return          要传的 arena 深度；没有 `mut ref` 实参时 0，必须传调用者 home arena 时 -1。
 */
int callHomeDepth(Checker *c, Vec *args, Vec *params, Expr *callNode) {
    int best = 0;                       /* 0 = no `mut ref` argument was found */
    /* Arguments whose answer depends on the escape set E must be recorded and
     * recomputed once the effect summaries are closed (see the `EArenaSite` comment). */
    EArenaSite *rec = NULL;
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        int d = slotDepth(c, place);   /* a parameter reports 0, a local its block depth */
        /* What the record keeps: 0 = a parameter or a global (outside this frame), -1 = an
         * escaping local (also outside, for good), k >= 1 = a block of this body. */
        int recD = d;
        const char *rn = placeRootName(place);
        if (d == 0) d = -1;             /* the place is in a parameter: pass my home arena */
        else if (rn && isEscapeeName(c, rn)) { d = -1; recD = -1; }
        /* Record the site in **both** cases, including the `d == 0` one.
         *
         * The arena answer for a parameter is already final -- the caller's home arena is
         * the longest-lived one there is -- but the **zone** answer is not: the zone is only
         * passed on when the callee allocates (`makesPool`), and `makesPool` is a closure,
         * so at this moment a callee that allocates only through its own callees still reads
         * as "no". Without a record here the final pass never sees the site, the zone keeps
         * its provisional level, and a call chain of two hops passes its own place down --
         * that is H2 (`tests/arena-soundness/H2_home_zone_two_hops.extc`). */
        if (callNode && c->eSites.arena) {
            if (!rec) {
                rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                rec->call = callNode;
                *(EArenaSite **)vecPush(&c->eSites) = rec;
            }
            EXTC_DBG_ASSERT(rec->n <= 8);   /* the slot count the struct declares */
            if (rec->n < 8) {
                rec->argRoot[rec->n]  = (char *)rn;
                rec->argDepth[rec->n] = recD;
                rec->n++;
            } else {
                rec->overflow = true;   /* conservative final pass; no silent truncation */
            }
        }
        if (best == 0 || d < best) best = d;
    }
    return best;
}


/**
 * @brief 判断名字向量里是否有某个名字。
 *
 * @param v  `const char *` 向量。
 * @param n  要查找的名字。
 * @return   任一元素与之相等时 true。
 */
static bool vecHasName(Vec *v, const char *n) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), n) == 0) return true;
    return false;
}
/**
 * @brief 判断表达式是否为池 plate 切出的块。
 *
 * `poolSlice` / `poolSliceRaw` 取新块，`poolResize` / `poolResizeRaw` 调整已有块，内存都属于
 * 容器自己拥有的池，不发布调用者的引用；SSO `string` 的 `grow` 依赖这条豁免，否则整条
 * `push`/`append` 链都会背上 `otherMask`。
 *
 * @param e  表达式；可为 NULL。
 * @return   是池 plate 块时 true。
 */
static bool exprIsPoolBlock(Expr *e) {
    if (!e || e->kind != EX_GENCALL || !e->u.gencall.name) return false;
    const char *n = e->u.gencall.name;
    return strcmp(n, "poolSlice")     == 0 || strcmp(n, "poolSliceRaw") == 0 ||
           strcmp(n, "poolResize")    == 0 || strcmp(n, "poolResizeRaw") == 0;
}

/**
 * @brief 判断初始值是否本帧新分配。
 *
 * `new` 算、池 plate 块算（见 `exprIsPoolBlock`），每个字段本身是新的或标量的 struct 字面量
 * 也算。
 *
 * @param e  初始化表达式；可为 NULL。
 * @return   是新分配、或只由这类值构成的字面量时 true。
 */
static bool exprIsFresh(Expr *e) {
    if (!e) return false;
    if (e->kind == EX_NEW) return true;
    if (exprIsPoolBlock(e)) return true;
    if (e->kind == EX_STRUCTLIT) {
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (!exprIsFresh((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return false;
        return true;
    }
    return false;
}
/**
 * @brief 记录每个分配行所在的最近循环与循环体的块层级。
 *
 * 层级必须取**循环体**的层级，而不是 `new` 恰好写在哪一层块里：分配落在循环体 arena 或更深
 * 才每轮回收，更浅的会跨轮存活、随迭代增长。循环体的层级取该循环内站点的最小
 * `lexicalLevel`，因为嵌套块只会报告更深的值。
 *
 * @param s         要遍历的语句。
 * @param loopId    当前循环 id；0 表示不在循环里，此时不记录。
 * @param nextLoop  下一个可用的循环 id。
 * @param sites     输出：追加 `(line, loopId, lexicalLevel)` 三元组。
 */
static void collectLoopSites(Stmt *s, int loopId, int *nextLoop, Vec *sites) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        if (loopId && exprHasNew(s->u.trap_.msg)) {
            *(int *)vecPush(sites) = s->u.trap_.msg->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.trap_.msg);
        }
        return;
    case ST_YIELD:
        if (loopId && exprHasNew(s->u.yield_.value)) {
            *(int *)vecPush(sites) = s->u.yield_.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.yield_.value);
        }
        return;
    case ST_DOMAIN:
        /* A domain block is not a loop: it introduces no new loop id, so the enclosing one is
         * passed straight through and only the body is walked. */
        collectLoopSites(s->u.domain_.body, loopId, nextLoop, sites);
        return;
    case ST_WHILE: {
        Stmt *b = s->u.whiles.body;
        if (!b) return;
        int id = (*nextLoop)++;
        collectLoopSites(b, id, nextLoop, sites);
        return;
    }
    case ST_VAR:
        if (loopId && exprHasNew(s->u.var.init)) {
            *(int *)vecPush(sites) = s->u.var.init->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.var.init);
        }
        return;
    case ST_ASSIGN:
        if (loopId && exprHasNew(s->u.assign.value)) {
            *(int *)vecPush(sites) = s->u.assign.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.assign.value);
        }
        return;
    case ST_EXPR:
        if (loopId && exprHasNew(s->u.expr.expr)) {
            *(int *)vecPush(sites) = s->u.expr.expr->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.expr.expr);
        }
        return;
    case ST_RETURN:
        if (loopId && exprHasNew(s->u.ret.value)) {
            *(int *)vecPush(sites) = s->u.ret.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.ret.value);
        }
        return;
    case ST_IF:
        collectLoopSites(s->u.ifs.thenBody, loopId, nextLoop, sites);
        collectLoopSites(s->u.ifs.elseBody, loopId, nextLoop, sites);
        return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectLoopSites(*(Stmt **)vecAt(&s->u.block.stmts, k), loopId, nextLoop, sites);
        return;
    case ST_MATCH:
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectLoopSites((*(MatchArm **)vecAt(&s->u.match.arms, k))->body, loopId, nextLoop, sites);
        return;
    default: return;
    }
}

/**
 * @brief 取含该行的循环 id。
 *
 * @param sites  `(line, loopId, lexicalLevel)` 三元组表。
 * @param line   源码行号。
 * @return       包含该行的循环 id；没有循环时为 0。
 */
static int loopIdOf(const Vec *sites, int line) {
    for (size_t i = 0; i + 2 < sites->len; i += 3)
        if (*(int *)vecAt((Vec *)sites, i) == line) return *(int *)vecAt((Vec *)sites, i + 1);
    return 0;
}

/**
 * @brief 打印 `--explain-memory` 报告：每个分配站点一行，附决策用的数字。
 *
 * 层级用的是 checker 决策时的同一批数字，把它们说出来正是报告的意义。站点按落到的层级、曾被
 * 发布到的最浅层级、是否在循环里列出。`minAt == 0` 表示站点被存进活过本帧的东西，这是数字
 * 自身唯一能支撑的结论；站点落到的层级与块释放点由不同规则决定，报告不对增长下结论。
 *
 * @param c    checker。
 * @param all  全部分配站点。
 */
static void reportMemory(Checker *c, Vec *all) {
    int nSite = 0, nLoop = 0;
    fprintf(stderr, "extC memory report\n");
    fprintf(stderr, "------------------\n");
    fprintf(stderr, "  One line per allocation site, with the numbers the checker decided on.\n");
    fprintf(stderr, "  `level` is the arena the site was placed in (0 = this frame, -1 = the\n");
    fprintf(stderr, "  caller's); `lexical` is the block it is written in. A site whose level\n");
    fprintf(stderr, "  is shallower than its own block is not released by that block.\n");
    fprintf(stderr, "  (Lines under `prelude:` are in stdlib/prelude.extc, not in %s.)\n",
            (c->ctx && c->ctx->path) ? c->ctx->path : "?");
    for (size_t i = 0; i < all->len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(all, i);
        if (!anArenaSites(f).len) continue;
        Vec sites;
        vecInit(&sites, c->arena, sizeof(int));
        int nextLoop = 1;
        collectLoopSites(f->body, 0, &nextLoop, &sites);
        fprintf(stderr, "\n  %s\n", f->name ? f->name : "?");
        Vec as_ = anArenaSites(f);
        for (size_t j = 0; j < as_.len; j++) {
            Expr *site = *(Expr **)vecAt(&as_, j);
            if (!site) continue;
            nSite++;
            bool inLoop = loopIdOf(&sites, site->line) != 0;
            if (inLoop) nLoop++;
            fprintf(stderr, "    line %-5d level %-3d lexical %-3d  %s\n",
                    site->line, planArenaLevel(site), anLexicalLevel(site),
                    inLoop ? "allocated once per round of a loop" : "allocated outside any loop");
        }
    }
    if (!nSite) {
        fprintf(stderr, "\n  no allocation site in this module\n");
        return;
    }
    fprintf(stderr, "\n  %d allocation site(s), %d of them inside a loop\n", nSite, nLoop);
    /* What this report does not do, yet.
     *
     * Whether a site's memory actually grows with the iteration count needs the level of
     * the block that releases the site's arena, and that is not derivable from the numbers
     * printed here by a predicate over them: such a predicate either misses a shape that
     * really grows or flags one that stays constant. The reliable form is to record, at code
     * generation time, which loop releases which arena; until then this report prints the
     * facts and claims nothing. */
    fprintf(stderr, "  (this report prints facts only: it does not yet say which sites grow)\n");
}

/**
 * @brief 从 fresh 集合里删掉一个名字。
 *
 * 集合是普通名字向量且始终很小（一个函数的局部），压缩比任何索引都便宜。
 *
 * @param fresh  名字向量。
 * @param name   要删的名字。
 */
static void freshDrop(Vec *fresh, const char *name) {
    if (!fresh || !name) return;
    size_t w = 0;
    for (size_t r = 0; r < fresh->len; r++) {
        const char *n = *(const char **)vecAt(fresh, r);
        if (strcmp(n, name) == 0) continue;
        *(const char **)vecAt(fresh, w++) = n;
    }
    fresh->len = w;
}

/**
 * @brief 收集初始值是新分配的本帧局部名。
 *
 * 只认 `var x = new ...` 的直接形式；别名传播刻意不做，缺名字只会保留本可去掉的要求，这是
 * 保守方向。
 *
 * @param a      未使用，只转发给递归调用。
 * @param fresh  输出：收集到的名字追加到这里。
 * @param s      要遍历的语句；块、if/else、while、match 分支都会跟进。
 */
static void collectFreshLocals(Arena *a, Vec *fresh, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (exprIsFresh(s->u.var.init)) *(const char **)vecPush(fresh) = s->u.var.name;
        return;
    case ST_ASSIGN: {
        /* A name stops being fresh the moment an assignment gives it a value that is not itself
         * fresh: after `n = id(target)`, `n` points at whatever `id` returned, not at the `new
         * i32` its declaration made. Keeping
         * the name made `classifyStoredValue` answer "freshly allocated, so trivially safe" for a
         * value that now points into the caller's frame, so `stash`'s summary recorded nothing,
         * `effComplete` stayed true, and the whole call-site lifetime check was skipped -- a real
         * stack-use-after-scope (). This is the write-point invalidation of the fresh
         * set; the fresh-value case keeps the name, so `p = new i32` does not lose it. */
        /* Only a binding that is **retargeted** stops being fresh. A field write such as
         * `n.value = v` writes *through* the binding: `n` still points at the object its
         * declaration made. Dropping it there removed the fresh answer for a value that really is
         * fresh, and the call-site check then rejected `stash(dest, ref deep, v)` -- the accepted
         * and ASan-clean program of -- which is how this narrowing was found. */
        if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
            const char *rn = placeRootName(s->u.assign.target);
            if (rn && !exprIsFresh(s->u.assign.value)) freshDrop(fresh, rn);
        }
        return;
    }
    case ST_IF:    collectFreshLocals(a, fresh, s->u.ifs.thenBody);
                   collectFreshLocals(a, fresh, s->u.ifs.elseBody); return;
    case ST_WHILE: collectFreshLocals(a, fresh, s->u.whiles.body); return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectFreshLocals(a, fresh, *(Stmt **)vecAt(&s->u.block.stmts, k));
        return;
    case ST_MATCH:
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectFreshLocals(a, fresh, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body);
        return;
    default: return;
    }
}


/**
 * @brief 判断一个名字是否在逃逸集合 E 里。
 *
 * 规则（过近似：多一个名字只多花内存，少一个名字会留下悬空指针）：`return e` 里出现过的名字
 * 进 E；赋给已在 E 的绑定、或存进参数指向的容器的名字进 E。E 按不动点闭合。
 *
 * @param c  checker；E 存在 `c->escapees`。
 * @param n  绑定名。
 * @return   在 E 里（其内容可能离开本帧）时 true。
 */
bool isEscapeeName(Checker *c, const char *n) {
    if (!n) return false;
    for (size_t i = 0; i < c->escapees.len; i++)
        if (strcmp(*(const char **)vecAt(&c->escapees, i), n) == 0) return true;
    return false;
}
/**
 * @brief 把一个名字加进逃逸集合。
 *
 * @param c  checker。
 * @param n  要加的名字；可为 NULL。
 * @return   之前不在集合里时 true，用于驱动不动点迭代。
 */
static bool addEscapee(Checker *c, const char *n) {
    if (!n || isEscapeeName(c, n)) return false;
    *(const char **)vecPush(&c->escapees) = n;
    return true;
}

/* Add every name appearing in the expression to E; true when a new name was added,
 * which is what the fixpoint loop uses. */
static bool markNamesInExpr(Checker *c, Expr *e);
/**
 * @brief 把一条语句里出现的名字加进逃逸集合。
 *
 * @param c  checker。
 * @param f  正在算逃逸集合的函数。
 * @param s  语句；可为 NULL。
 * @return   至少加进一个新名字时 true。
 */
static bool markNamesInStmt(Checker *c, FuncDef *f, Stmt *s) {
    if (!s) return false;
    bool grew = false;
    switch (s->kind) {
    case ST_TRAP:
        /* The message is read in this frame; nothing in it leaves. */
        return markNamesInExpr(c, s->u.trap_.msg);
    case ST_YIELD:
        /* The value handed to the resumer leaves this frame just like a `return` does. */
        return markNamesInExpr(c, s->u.yield_.value);
    case ST_RETURN:
        return markNamesInExpr(c, s->u.ret.value);          /* everything handed back escapes */
    case ST_VAR:
        /* Propagate only when the declared name itself escapes. Propagating
         * unconditionally made E hold every name that ever appears, so everything
         * escaped, which wasted memory and changed every golden output; fixed. */
        if (isEscapeeName(c, s->u.var.name)) grew |= markNamesInExpr(c, s->u.var.init);
        return grew;
    case ST_ASSIGN: {
        /* Propagate only when the target is in E or is a container a parameter points
         * at. Propagating on every assignment made E too large. */
        const char *rn = placeRootName(s->u.assign.target);
        if (rn && (isEscapeeName(c, rn) || paramIndexByName(f, rn) >= 0))
            grew |= markNamesInExpr(c, s->u.assign.value);
        return grew;
    }
    case ST_DOMAIN:
        /* The receiver (`d.run`) is a domain object, not a name that escapes; the block's
         * body is ordinary code and gets exactly the treatment a block gets. */
        grew |= markNamesInStmt(c, f, s->u.domain_.body);
        return grew;
    case ST_IF:
        /* Names in the condition do not count as escaping. Adding them unconditionally
         * made E too large, so the home arena changed all over the place and
         * out-parameters were falsely rejected. */
        grew |= markNamesInStmt(c, f, s->u.ifs.thenBody);
        grew |= markNamesInStmt(c, f, s->u.ifs.elseBody);
        return grew;
    case ST_WHILE:
        /* As above: the loop condition does not count as escaping. */
        grew |= markNamesInStmt(c, f, s->u.whiles.body);
        return grew;
    case ST_EXPR:
        /* A local published through a call. An unconditional false here costs real
         * use-after-free bugs of this shape: a local published
         * through a call never enters E, so `callHomeDepth` takes the receiver for depth
         * 1, the caller's frame, and the arena argument becomes the current frame's arena,
         * the callee's own frame, instead of the home arena. Whatever the callee stored
         * into that container is then released when this call returns.
         *
         * Marking every call is not an option either: a plain read such as `println(x)`
         * was marked too, which produced six false rejections (including `borrowing`,
         * `container-of-view`, `ref-field-write`, `borrowed-stash-sameframe`, and
         * `G_stale_origin`). So only the arguments of callees that can really publish
         * them are marked.
         *
         * The criterion is the existing effect summary: any bit that says it stores
         * somewhere (`addrMask`, `contMask`, `otherMask`, or their home variants) means
         * this callee's arguments may be published, so they are marked. A clean summary
         * (as for `println`) leaves them unmarked, which keeps the precision.
         *
         * The summary is an upper bound on whether a store happens at all, so answering
         * that it may store is the safe direction. */
        /* `out.put(x)?` is an **`EX_TRY` around the call**: the statement's top-level kind is
         * therefore neither `EX_CALL` nor `EX_METHOD`, so it fell into the `default` below and the
         * call's arguments never entered E. `callHomeDepth` then took the receiver for depth 1,
         * the arena argument became this frame's arena instead of the home arena, and whatever the
         * callee stored was released when the call returned -- , heap-use-after-free,
         * with a one-character difference from the accepted twin (`out.put(x)` without the `?`).
         * The `?` decides what happens *after* the call, not what the call can publish, so unwrap
         * it and ask the same question. */
        {
        Expr *se = s->u.expr.expr;
        while (se && se->kind == EX_TRY) se = se->u.try_.operand;
        if (!se) return false;
        switch (se->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: {
            FuncDef *cf = planCallee(se);
            if (!cf) return false;
            uint64_t pub = anAddrMask(cf) | anContMask(cf) | anOtherMask(cf)
                         | anHomeAddrMask(cf) | anHomeContMask(cf);
            if (anAddrFromLocal(cf)) pub |= 1u;
            /* An incomplete summary must never be read as proof that nothing is
             * stored: when the body contains a call that cannot be resolved,
             * `effComplete` stays false forever. So a callee with any `mut ref`
             * parameter, which is an entry point for a store, is treated as one that may
             * store. The direction is safe: marking more only costs memory. */
            if (!anEffComplete(cf)) {
                for (size_t pi = 0; pi < cf->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&cf->params, pi);
                    if (pp->type && pp->type->kind == TY_REF && pp->type->mut) { pub |= 1u; break; }
                }
            }
            if (pub == 0) return false;        /* the callee stores nothing (e.g. `println`) */
            return markNamesInExpr(c, se);
        }
        default:
            return false;                      /* other expression statements do not escape */
        }
        }
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            grew |= markNamesInStmt(c, f, *(Stmt **)vecAt(&s->u.block.stmts, k));
        return grew;
    case ST_MATCH:
        /* As above: the match subject does not count as escaping. */
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            grew |= markNamesInStmt(c, f, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body);
        return grew;
    default: return false;
    }
}
/**
 * @brief 把表达式里出现的名字加进逃逸集合。
 *
 * @param c  checker。
 * @param e  表达式；可为 NULL。
 * @return   至少加进一个新名字时 true。
 */
static bool markNamesInExpr(Checker *c, Expr *e) {
    if (!e) return false;
    bool grew = false;
    switch (e->kind) {
/* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every
     * walker has to look inside it -- the walkers end in `default:`, which is why `-Wswitch`
     * never pointed at the omission. */
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.gencall.args, i));
        return grew;
    case EX_DYN: return markNamesInExpr(c, e->u.dynv.payload);
    case EX_IDENT: return addEscapee(c, e->u.ident.name);
    case EX_BIN:   grew |= markNamesInExpr(c, e->u.bin.left);
                   grew |= markNamesInExpr(c, e->u.bin.right); return grew;
    case EX_UN:    return markNamesInExpr(c, e->u.un.operand);
    case EX_REF:   return markNamesInExpr(c, e->u.ref.operand);
    case EX_EXT: return markNamesInExpr(c, e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return markNamesInExpr(c, e->u.deref.operand);
    case EX_SIGN:  return markNamesInExpr(c, e->u.sign.operand);
    case EX_CONV:  return markNamesInExpr(c, e->u.conv.operand);
    case EX_TRY:   return markNamesInExpr(c, e->u.try_.operand);
    case EX_SLICE: grew |= markNamesInExpr(c, e->u.slice.obj);
                   grew |= markNamesInExpr(c, e->u.slice.lo);
                   grew |= markNamesInExpr(c, e->u.slice.hi); return grew;
    case EX_FIELD: return markNamesInExpr(c, e->u.field.obj);
    case EX_INDEX: grew |= markNamesInExpr(c, e->u.index.obj);
                   grew |= markNamesInExpr(c, e->u.index.index); return grew;
    case EX_COALESCE:
        grew |= markNamesInExpr(c, e->u.coalesce.main);
        grew |= markNamesInExpr(c, e->u.coalesce.fallback); return grew;
    case EX_NEW:   return markNamesInExpr(c, e->u.new_.count);
    case EX_CALL:
        for (size_t k = 0; k < e->u.call.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.call.args, k));
        return grew;
    case EX_METHOD:
        grew |= markNamesInExpr(c, e->u.method.recv);
        for (size_t k = 0; k < e->u.method.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.method.args, k));
        return grew;
    case EX_ASSOC:
        for (size_t k = 0; k < e->u.assoc.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.assoc.args, k));
        return grew;
    case EX_ENUMVAL:
        for (size_t k = 0; k < e->u.enumval.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.enumval.args, k));
        return grew;
    case EX_STRUCTLIT:
        for (size_t k = 0; k < e->u.lit.inits.len; k++)
            grew |= markNamesInExpr(c, (*(FieldInit **)vecAt(&e->u.lit.inits, k))->value);
        return grew;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t k = 0; k < e->u.lambda.inits.len; k++)
            grew |= markNamesInExpr(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, k))->value);
        return grew;
    case EX_ARRAYLIT:
        for (size_t k = 0; k < e->u.arraylit.elems.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.arraylit.elems, k));
        return grew;
    default: return false;
    }
}

/* Fixpoint closure: whatever is moved out takes its contents with it. */
static void computeEscapes(Checker *c, FuncDef *f);
/**
 * @brief 以求不动点的方式算出函数体的逃逸集合。
 *
 * @param c          checker；集合存在 `c->escapees`。
 * @param f          要分析的函数。
 * @param unionMode  true 时保留已收集的名字（并集），false 先清空。
 */
static void computeEscapesMode(Checker *c, FuncDef *f, bool unionMode) {
    if (!c->escapees.arena) vecInit(&c->escapees, c->arena, sizeof(const char *));
    if (!unionMode) c->escapees.len = 0;     /* union mode: keep the names already collected */
    if (unionMode) {
        /* Union mode: iterate until the set stops growing. `isEscapeeName` reports a name
         * that is already in E as present, so the loop has to run round after round until
         * nothing changes. */
        for (int round = 0; round < 64; round++)
            if (!markNamesInStmt(c, f, f->body)) break;
    } else {
        for (int round = 0; round < 32; round++)
            if (!markNamesInStmt(c, f, f->body)) break;   /* stop when the set stops growing */
    }
    if (getenv("EXTC_DUMP_EFFECTS")) {
        fprintf(stderr, "[escapes] %-22s { ", FN(f));
        for (size_t i = 0; i < c->escapees.len; i++)
            fprintf(stderr, "%s ", *(const char **)vecAt(&c->escapees, i));
        fprintf(stderr, "}\n");
    }
}

/**
 * @brief 算出函数体会转义出哪些绑定（非静默模式）。
 *
 * @param c  检查器。
 * @param f  函数定义。
 */
static void computeEscapes(Checker *c, FuncDef *f) { computeEscapesMode(c, f, false); }

/**
 * @brief 字段表按需增长（翻倍，起步 4），返回增长后的表。
 *
 * 拷贝进 checker 的 arena：`Sym` 里的指针只增不减，旧数组留给 arena 整片回收，没有 free
 * 路径要管。**没有上限** —— 上限会改变提升决策（语料实测 63 次撞上限）。
 *
 * @param c    checker。
 * @param s    要增长的绑定。
 * @param need 至少要能放下的格数。
 * @return     增长后的表（调用方通常不用，`s->fields` 已更新）。
 */
static SymField *symFieldsGrow(Checker *c, Sym *s, int need) {
    if (s->fieldsCap >= need) return s->fields;
    int cap = s->fieldsCap ? s->fieldsCap : 4;
    while (cap < need) cap *= 2;
    /* **必须置零**：新表里 `nfields` 之后那些格的 `.src`/`.srcSym`/`.minReq` 没人写
     * （`fieldDepthEntry` 只写 `.name`/`.depth`），而 `promoteFieldsAt` 会对每个下标 < `nfields`
     * 的格读 `.src` 并当 `Expr *` 解引用。用 `arenaAlloc` 的话那是个未初始化堆指针 —— 实测
     * `MALLOC_PERTURB_=165 ./build/extc --run tests/stl/clone.extc` 100% 段错误
     * （`promoteInto2` 解引用 `val=0x211940`）；不扰动堆时也有 1–4% 的偶发崩溃。
     * 置零把没来源的槽变成 `src == NULL`（`promoteFieldsAt` 直接 `continue`）、`.minReq == 0`
     * （"必须活过本帧"，保守方向）。 */
    SymField *nf = (SymField *)arenaAllocZero(c->arena, (size_t)cap * sizeof *nf);
    for (int i = 0; i < s->nfields; i++) nf[i] = s->fields[i];
    s->fields = nf;
    s->fieldsCap = cap;
    return nf;
}

/**
 * @brief 取绑定字段级深度表里某字段的槽，必要时新建。
 *
 * 表最多 4 槽，放不下的记进 `otherDepth` 作保守兜底；根的有效深度是 `otherDepth` 与各字段槽
 * 的最大值。槽里的名字来自 AST，活得比绑定记录久，因此不会悬空。
 *
 * @param c      未使用；保留以符合 checker 参数的惯例。
 * @param s      要查字段表的绑定。
 * @param field  要查或新建的字段名。
 * @param create 表里还没有该字段时是否新建槽。
 * @return       该字段的深度槽指针；没有槽且 `create` 为 false、或表已满时返回 NULL。
 */
/**
 * @brief 找到（或按需新建）某个引用字段的深度槽。
 *
 * @param c      checker。
 * @param s      绑定的引用字段表。
 * @param field  字段名（AST 里 intern 过，不会悬垂）。
 * @param create 表里还没有该字段时是否新建槽。
 * @return       该字段的深度槽指针；`create` 为 false 且没有槽时返回 NULL。
 */
int *fieldDepthEntry(Checker *c, Sym *s, const char *field, bool create) {
    if (!s || !field) return NULL;
    for (int i = 0; i < s->nfields; i++)
        if (s->fields[i].name && strcmp(s->fields[i].name, field) == 0) return &s->fields[i].depth;
    if (!create) return NULL;
    symFieldsGrow(c, s, s->nfields + 1);
    s->fields[s->nfields].name  = field;          /* interned in the AST, so it cannot dangle */
    s->fields[s->nfields].depth = 0;
    return &s->fields[s->nfields++].depth;
}

/**
 * @brief 字段槽变化后重算绑定的有效深度。
 *
 * 读到的是绑定仍可能携带的最深引用，即兜底槽与所有字段槽的最大值；它是上界，只能变大。
 *
 * @param s  刚更新过字段表的绑定；NULL 允许且忽略。
 */
static void refreshRootDepth(Sym *s) {
    if (!s) return;
    int m = s->otherDepth;
    for (int i = 0; i < s->nfields; i++) if (s->fields[i].depth > m) m = s->fields[i].depth;
    /* The recorded depth of the root may only grow: it is an upper bound over the live
     * pointers in the binding, so a value may be reported too deep but never too
     * shallow. Letting it drop is unsound: storing a deep value into one field and then
     * a shallow value into another shrinks the root, while the shallow store does not
     * erase the pointer still live in the first field, so a later whole-value copy or
     * return is accepted. Three counterexamples of that shape reproduced as an ASan
     * heap-use-after-free. */
    if (anRefDepth(s) > m) m = anRefDepth(s);
    anSetRefDepth(s, m);
}

/**
 * @brief 保留下写入某个字段槽的来源表达式。
 *
 * 只对能指向站点的形状记录：`null` 与字面量不记录，记录会顶掉先前真正的来源。比较基准是本
 * 次写入的深度 `srcDepth`，而不是做弱更新的 `depth`（后者取最大值，晚来的 `null` 会显得更
 * 深并清掉来源）。
 *
 * @param root   要更新字段表的绑定；可为 NULL。
 * @param field  字段名；整个对象或元素写入时为 NULL。
 * @param d2     本次写入的深度。
 * @param src    存下的表达式；可为 NULL。
 */
static void noteFieldSrc(Checker *c, Sym *root, const char *field, int d2, Expr *src) {
    if (!root || !src) return;
    if (src->kind != EX_IDENT && src->kind != EX_FIELD && src->kind != EX_INDEX &&
        src->kind != EX_NEW   && src->kind != EX_GENCALL) return;
    /* 只有**能携带引用**的值才值得记。`n.val = m`（i64）也会走到这里（根类型的字段里可能有
     * 引用），把它记成来源只会在提权时去提一个提不动的表达式，把正常形状误拒。 */
    if (!src->type || !typeContainsRef(c->tt, tsub(c, src->type))) return;
    /* 来源压平到根表达式再存：中间绑定（`p`、`pt`、`q`）到提权那一刻可能已经出了作用域，
     * 那时 `lookup` 查不到它，`promoteInto2` 就在"没有来源"处停下 —— 站点于是停在词法层，
     * 而返回出去的链已经指向它。根表达式（`new` 之类）没有这个问题。
     *
     * 根是零值/字面量或一个没有来源的绑定（`p.next = saved`，saved 的根是 `null`）时**不记**：
     * 那样的来源提不动，记下来只会让 `promoteFieldsAt` 答 false，把 `saved = p` 这种正常形状
     * 误拒。来源的唯一用途是提权，提不动的来源没有价值。 */
    /* 来源展平到根表达式存（末轮重放没有作用域，`lookup` 查不到中间绑定），同时把那个绑定
     * 另存一格 `srcSym`：展平会丢掉它的**字段表**（`b.next = ca` 记在 `b` 上），环/链的提权
     * 必须能走到那里。Sym 住在 checker 的 arena 里，出了作用域照样能用。 */
    Sym *srcSym = (src->kind == EX_IDENT) ? lookup(c, src->u.ident.name) : NULL;
    {
        Expr *r = originOf(c, src, 0);
        if (!r) return;
        if (r->kind == EX_NULL || r->kind == EX_INT || r->kind == EX_FLOAT ||
            r->kind == EX_BOOL || r->kind == EX_STR) return;
        if (r->kind == EX_IDENT) return;      /* 追不到站点（参数、无来源的绑定） */
        src = r;
    }
    if (!field) {
        /* 没有单一字段的写 —— 整值拷贝、元素写、经 `mut ref` 的调用 —— 进 `otherSrc` 那一格。
         * 更新规则与字段槽相同：比较本次写入的 `d2` 而不是弱更新的 `depth`，否则晚来的 `null`
         * 会显得更深并把真来源顶掉。 */
        if (d2 >= root->otherSrcDepth) {
            /* 第一次记来源时把 `otherMinReq` 定在这次写入的深度：0 是"必须活过本帧"这句很强的
             * 断言，当缺省用会让每次提权都去提一个不需要提的站点并失败。之后
             * `promoteFieldsAt` 只会把它收得更紧。 */
            if (!root->otherSrc) root->otherMinReq = d2;
            root->otherSrc = src;
            root->otherSrcSym = srcSym;
            root->otherSrcDepth = d2;
        }
        return;
    }
    int n = -1;
    for (int i = 0; i < root->nfields; i++)
        if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) { n = i; break; }
    if (n < 0) {
        /* 槽不存在就**建**。只写已存在的槽会让 `bx.mid.next = p` 这类两层投影永远没有槽：
         * 声明 `{ mid: { next: null } }` 只建了 `mid`，来源于是没人记，返回值的检查也就够不着
         * 那个分配站点。表满时退化到 `otherSrc`：来源仍在，只是粒度更粗（保守方向）。 */
        symFieldsGrow(c, root, root->nfields + 1);
        n = root->nfields++;
        root->fields[n].name     = field;   /* interned in the checker's arena, so it cannot dangle */
        root->fields[n].depth    = 0;
        root->fields[n].src      = NULL;
        root->fields[n].srcSym   = NULL;
        root->fields[n].srcDepth = 0;
        /* 门槛从这次写入的深度起步，不是 0（0 是"必须活过本帧"这句很强的断言）：新槽的来源
         * 现在至少活到 `d2`，之后 `promoteFieldsAt` 只会把它收得更紧。从 0 起步会让
         * `head = b`（b 的 `next` 里存着上一层的 head）这种正常提权去提一个不需要提到 home
         * 的站点、答 false 而误拒。 */
        root->fields[n].minReq   = d2;
    } else if (!root->fields[n].src) {
        root->fields[n].minReq = d2;        /* 第一次给这个已存在的槽记来源 */
    }
    if (dbgOn("EXTC_DBG_FS"))
        fprintf(stderr, "[fs] %s.%s d2=%d oldSrcDepth=%d srckind=%d\n", root->name,
                field, d2, root->fields[n].srcDepth, (int)src->kind);
    if (d2 >= root->fields[n].srcDepth) {
        root->fields[n].src = src;
        root->fields[n].srcSym = srcSym;
        root->fields[n].srcDepth = d2;
    }
}

/**
 * @brief 记录一次经由绑定到达的写入的深度。
 *
 * 引用型绑定（`n: mut ref node`）的 `refDepth` 只能升、不能降：降会把“指向谁”覆盖成“所指
 * 对象含什么”，让悬空指针被接受。普通 struct 绑定可以降，字段槽的强更新需要这一点。
 *
 * @param c      checker；`c->curStoreVal` 是存入的表达式。
 * @param root   要更新字段表的绑定；可为 NULL。
 * @param field  字段名；整个对象或元素写入时为 NULL。
 * @param d2     存入值的深度。
 */
void noteFieldDepthWrite(Checker *c, Sym *root, const char *field, int d2) {
    if (!root) return;
    noteFieldSrc(c, root, field, d2, c->curStoreVal);   /* the depth record is left untouched */
    /* A field write through a reference-typed binding (`n: mut ref node`) writes a
     * field of the pointee, and `refreshRootDepth` recomputes the root as the max over
     * the field slots, which replaces what I point at (depth 2) with what is stored
     * inside the object I point at (the field depth, 0). A later `keeper = n` was then
     * accepted and dangled (confirmed with ASan). The fix: for a reference-typed
     * binding, `refDepth` may only be moved toward the longer-lived value, never
     * lowered. A struct binding that is not a reference may still drop, and that is
     * exactly what the strong update is for. */
    bool isRefRoot = root->type && tsub(c, root->type)->kind == TY_REF;
    int  before    = anRefDepth(root);
    if (!field) {                                  /* whole-object assignment or element write */
        /* An element write invalidates no other element or field, so it must neither
         * clear the field table nor overwrite `otherDepth`. The old behavior cleared the
         * table and wrote `otherDepth = 0`, so `a[1] = x; a[0] = null` let a whole-value
         * escape through. */
        if (d2 > root->otherDepth) root->otherDepth = d2;
        refreshRootDepth(root);
        if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);   /* keep the pointee */
        return;
    }
    int *slot = fieldDepthEntry(c, root, field, true);
    if (!slot) {                                   /* full table: the fallback slot takes the max */
        if (d2 > root->otherDepth) root->otherDepth = d2;
    } else if (root->addressed) {                  /* address taken: aliases may write, take max */
        if (d2 > *slot) *slot = d2;
    } else if (root->fieldsComplete) {
        /* First step of the data-flow part: with a complete field table, the new value
         * replaces the old one in this slot, so the old value is not a property of
         * any slot of the container, and the root is recomputed skipping it. That is the
         * strong update. It is sound because the root record is an upper bound over all
         * slots: with a complete table, removing an edge that does not hold leaves an
         * upper bound over the rest. The depth is not shrunk out of thin air. */
        *slot = d2;
        int m = root->otherDepth;
        for (int i = 0; i < root->nfields; i++) {
            if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) continue;
            if (root->fields[i].depth > m) m = root->fields[i].depth;
        }
        anSetRefDepth(root, m);
        if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);   /* keep the pointee */
        return;
    } else {
        /* An incomplete field table forbids any drop: the missing slots may hold values.
         * This is how a real `stack-use-after-scope` was built once. `var h3 = h` dropped
         * the `q` slot, so after `h3.p = null` the depth of `q` vanished and `return h3`
         * was accepted; the regression test
         * `tests/arena-soundness/H1_strongupdate_missed.extc` caught it. */
        *slot = d2;
    }
    refreshRootDepth(root);
    /* Do not forget what a reference-typed binding points at: the field table only
     * describes what is stored inside the pointee, so `refDepth` is restored here. */
    if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);
}

/**
 * @brief 记录整聚合赋值，同时保留逐字段来源。
 *
 * 只记 `h.otherDepth = depth(t)` 会丢掉 `t` 的逐字段分配站点，之后安全的 `return h` 会被拒。
 * 来源表完整时精确复制；目标已暴露或来源不完整时退回保守的聚合更新。
 *
 * @param c      checker。
 * @param dst    赋值目标绑定。
 * @param value  被赋的聚合值。
 * @param d2     该值的深度。
 */
void noteWholeValueDepthWrite(Checker *c, Sym *dst, Expr *value, int d2) {
    if (!dst) return;

    Sym *src = value && value->kind == EX_IDENT ? identBindOf(value) : NULL;
    if (!dst->addressed && src && src != dst && src->fieldsComplete) {
        symFieldsGrow(c, dst, src->nfields > 0 ? src->nfields : 1);
        dst->nfields = src->nfields;
        for (int i = 0; i < src->nfields; i++) dst->fields[i] = src->fields[i];
        dst->otherDepth = src->otherDepth;
        dst->fieldsComplete = true;

        int m = dst->otherDepth;
        for (int i = 0; i < dst->nfields; i++)
            if (dst->fields[i].depth > m) m = dst->fields[i].depth;
        anSetRefDepth(dst, m);
        return;
    }

    noteFieldDepthWrite(c, dst, NULL, d2);
}
/**
 * @brief 按名字取参数位置。
 *
 * 参数遮蔽是合法语言特性，按名字匹配会认错人；手上已有表达式时应改用 `paramIndexOfExpr`
 * （按符号判断）。
 *
 * @param f     要搜索的函数；可为 NULL。
 * @param name  参数名；可为 NULL。
 * @return      从 0 开始的参数下标；不是参数时 -1。
 */
static int paramIndexByName(FuncDef *f, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, name) == 0) return (int)i;
    return -1;
}

/**
 * @brief 不经作用域查找，取表达式根解析到的绑定。
 *
 * `placeRoot` 也回答同一个问题，但它走 checker 的作用域，而作用域只在检查函数体时存在；
 * 效应摘要在那之后才跑。`Sym` 已由解析挂在 `EX_IDENT` 节点上，所以这里不需要作用域。
 *
 * @param e  表达式。
 * @return   根标识符的绑定；不是标识符或未解析时 NULL。
 */
static Sym *rootSymNoScope(Expr *e) {
    /* 问题③的**无作用域**版本（摘要 pass 在所有函数体检查完之后才跑，那时没有作用域可查，
     * 这是  的根因）。掩码含 `SIGN`、保留 16 跳上限 —— 差异来自历史，现在写在掩码里。*/
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF | EXTC_ROOT_SIGN, 16);
    return leaf ? identBindOf(leaf) : NULL;
}

/**
 * @brief 按符号（而不是拼写）判断值 `e` 来自 `f` 的哪个参数。
 *
 * 摘要记录“第 j 个实参被存下”，调用点据此只检查该实参，所以答案必须经得起遮蔽；只有名字的
 * 调用方用 `paramIndexByName`。
 *
 * @param c  checker。
 * @param f  函数。
 * @param e  值表达式。
 * @return   参数下标；不来自参数时 -1。
 */
static int paramIndexOfExpr(Checker *c, FuncDef *f, Expr *e) {
    (void)c;
    if (!f || !e) return -1;
    Sym *root = rootSymNoScope(e);
    if (!root) return -1;
    EXTC_DBG_ASSERT(anParamSymCount(f) <= 64);         /* must equal the array the struct declares */
    for (int i = 0; i < anParamSymCount(f); i++)
        if ((Sym *)anParamSyms(f)[i] == root) return i;
    return -1;
}

/**
 * @brief 记录存入值 `e` 为 `f` 置上的效应摘要位。
 *
 * 经由 `mut ref` 参数、或存进本帧新分配对象的赋值，可能把指针交给调用者。摘要按参数分别
 * 记录：存进来自实参的地址（地址流）还是从实参内容里读出的指针（内容流），调用点只检查被置
 * 上的位。
 *
 * @param c         checker；NULL 时跳过类型判断。
 * @param f         正在填摘要的函数。
 * @param e         被存入的值。
 * @param i         目标容器参数的下标；目标不是 `f` 的参数时 -1。
 * @param intoHome  目标为本帧新分配对象时为 true，目标为 `mut ref` 参数时为 false。
 * @param fresh     本帧分配的名字；来自它们的值在调用点不需要约束。
 */
static void classifyStoredValue(Checker *c, FuncDef *f, Expr *e, int i, bool intoHome, Vec *fresh) {
    if (!e || !f) return;
    /* A one-payload variant construction (`some(v)`, `success(v)`) holds exactly what `v`
     * holds -- one tag and one copy -- so classify the payload, not the wrapper: the wrapper
     * is not a second source of references. SSO `string` keeps its pool block in an `option`
     * field (`self.big = some(blk)`), and without this the store reads as unattributable. */
    if (e->kind == EX_ENUMVAL && e->u.enumval.args.len == 1)
        e = *(Expr **)vecAt(&e->u.enumval.args, 0);
    /* Address flow: the address of a place is stored, so that place has to live at least
     * as long as the container it is stored into. */
    if (e->kind == EX_REF) {
        int j = paramIndexOfExpr(c, f, e->u.ref.operand);
        if (j >= 0) {
            if (intoHome) anSetHomeAddrMask(f, anHomeAddrMask(f) | (((uint64_t)1 << j)));
            else if (i >= 0) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << j)));
        } else {
            anSetAddrFromLocal(f, true);              /* local address: a different problem */
        }
        return;
    }
    /* A value that comes from a parameter must be classified: is it the pointer itself,
     * or a pointer read out of a container?
     *   - `s.r = target`, where `target` is a pointer parameter, stores the caller's own
     *     pointer. That is address flow: the call site has to guarantee that what it
     *     points at lives at least as long as the destination.
     *   - `n.next = l.head` reads a pointer out of the contents of a parameter. That is
     *     content flow.
     * The first version treated both as content flow, and tests/errors/ref_arg_too_deep
     * went from being rejected to passing. With this slot wrong, the call-site
     * rule that requires whatever an argument points at to outlive the destination
     * accepts a dangling pointer instead. */
    {
        int j = paramIndexOfExpr(c, f, e);
        if (j >= 0) {
            /* A scalar carries no reference and so imposes no lifetime constraint;
             * saying otherwise would make `varArray<i32>::push` a false positive. A type
             * that mentions a type parameter, however, counts as carrying a reference:
             * the summary is computed on the template, where `T` is still opaque. Without
             * that, the `Cont(v)` slot of `varArray<T>::push` would record no parameter at
             * all, the call site would check nothing, and a real dangling pointer would be
             * accepted. The cost is zero, because the call site looks at the concrete
             * argument types: in `push(ref v, 5)` the `5` is i32, so that step is
             * skipped. */
            bool carrier = (c && e->type)
                         ? (typeContainsRef(c->tt, tsub(c, e->type)) || mentionsParam(tsub(c, e->type)))
                         : true;
            bool isPtr = (e->kind == EX_IDENT) && e->type &&
                         (e->type->kind == TY_REF || ttIsViewType(e->type));
            if (carrier) {
                if (isPtr) {                      /* address flow: the caller's pointer is stored */
                    if (intoHome) anSetHomeAddrMask(f, anHomeAddrMask(f) | (((uint64_t)1 << j)));
                    else if (i >= 0) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << j)));
                } else {                          /* content flow: pointer read from a container */
                    if (intoHome) anSetHomeContMask(f, anHomeContMask(f) | (((uint64_t)1 << j)));
                    else if (i >= 0) anSetContMask(f, anContMask(f) | (((uint64_t)1 << j)));
                }
            }
            return;
        }
    }
    if (e->kind == EX_NEW) return;                /* freshly allocated, so it is trivially safe */
    if (exprIsPoolBlock(e)) return;               /* a pool plate block: fresh, and the container owns it */
    /* A variant construction with **no payload** (`none`, and any payload-free enum variant)
     * holds no reference, whatever the union's type says: the payload is simply not there.
     * Without this, `self.big = none` -- how an SSO string clears its pool view -- marks the
     * whole container as "stores something unknown", every caller of `push` inherits
     * `otherMask`, and the call-site rule turns into the strictest one there is: an argument
     * of a caller may not live in any block at all. `failure(e)` and `some(v)` carry a value
     * and are classified as before. */
    if (e->kind == EX_ENUMVAL && e->u.enumval.args.len == 0) return;
    { const char *vr = placeRootName(e);          /* `l.head = n`: the source is a fresh local */
      if (vr && vecHasName(fresh, vr)) return; }
    if (c && e->type && !typeContainsRef(c->tt, e->type)) return;   /* scalar, trivially safe */
    if (i >= 0) anSetOtherMask(f, anOtherMask(f) | (((uint64_t)1 << i)));        /* not sure, so stay conservative */
}

static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e);

/**
 * @brief 记录一条语句可能执行的全部“存入”。
 *
 * 摘要按参数记录：哪个参数收到地址、哪个收到从另一参数读出的指针、哪个未知。
 *
 * @param c      checker。
 * @param f      正在填摘要的函数。
 * @param s      要遍历的语句；NULL 允许。
 * @param fresh  本帧分配的名字；来自它们的值安全，不记录。
 */
static void collectEffectsStmt(Checker *c, FuncDef *f, Stmt *s, Vec *fresh) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        collectEffectsExpr(c, f, s->u.trap_.msg);
        return;
    case ST_YIELD:
        collectEffectsExpr(c, f, s->u.yield_.value);
        return;
    case ST_ASSIGN: {
        /* Is the target a container named by a `mut ref` parameter?
        * `l.head = ...`, `*p = ...`, and `l.buf[i] = ...` all are. */
        const char *dstRoot = placeRootName(s->u.assign.target);
        int i = paramIndexByName(f, dstRoot);
        if (i >= 0) {
            /* Destination: the container parameter `i` names. */
            Param *p = *(Param **)vecAt(&f->params, i);
            if (p->type && p->type->kind == TY_REF && p->type->mut)
                classifyStoredValue(c, f, s->u.assign.value, i, false, fresh);
        } else if (dstRoot && vecHasName(fresh, dstRoot)) {
            /* Destination: an object freshly allocated in this frame
            * (`n.next = l.head`, `n.owner = ref l`). This is the store into the home
            * arena, which is where the content flow of `push` comes from. */
            classifyStoredValue(c, f, s->u.assign.value, -1, true, fresh);
        }
        collectEffectsExpr(c, f, s->u.assign.target);
        collectEffectsExpr(c, f, s->u.assign.value);
        return;
    }
    case ST_VAR:    collectEffectsExpr(c, f, s->u.var.init); return;
    case ST_DOMAIN:
        /* The receiver is a domain object; the body is ordinary code. */
        collectEffectsStmt(c, f, s->u.domain_.body, fresh);
        return;
    case ST_IF:
        collectEffectsExpr(c, f, s->u.ifs.cond);
        collectEffectsStmt(c, f, s->u.ifs.thenBody, fresh);
        collectEffectsStmt(c, f, s->u.ifs.elseBody, fresh);
        return;
    case ST_WHILE:
        collectEffectsExpr(c, f, s->u.whiles.cond);
        collectEffectsStmt(c, f, s->u.whiles.body, fresh);
        return;
    case ST_RETURN: collectEffectsExpr(c, f, s->u.ret.value); return;
    case ST_EXPR:   collectEffectsExpr(c, f, s->u.expr.expr); return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectEffectsStmt(c, f, *(Stmt **)vecAt(&s->u.block.stmts, k), fresh);
        return;
    case ST_MATCH:
        collectEffectsExpr(c, f, s->u.match.scrutinee);
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectEffectsStmt(c, f, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body, fresh);
        return;
    default: return;
    }
}

/**
 * @brief 记录表达式里的调用及其到达的被调者。
 *
 * 解析不了的调用会置 `anEffUnknown(f)`，摘要永久不完整，不能据此放宽任何检查；记录的边就是
 * 该函数的调用图，`computeEffectsTransitive` 之后在它上面闭合。
 *
 * @param c  checker。
 * @param f  正在填被调者表与摘要的函数。
 * @param e  要遍历的表达式；NULL 允许。
 */
static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e) {
    if (!e) return;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && !planCallee(e)) {
        /* A call through a `fn` **value** (no `FuncDef` on the node).
         *
         * When the value came from a **table slot that carries an `effects` clause**, that clause
         * is exactly as authoritative as an `extern!` declaration -- it is the same parser, the
         * same three keys, and the checker already builds the call-site signature out of it. So
         * merge it into this function's summary and keep the summary **complete**; marking it
         * unknown instead made every caller of a wrapper pay the worst case, even when the slot
         * said "I store nothing":
         *
         *     fn rgb(api: ref cairoApi, cr: ref void, …) { api.setSourceRgb(cr, …) }
         *     // `setSourceRgb` is signed `Addr=0 Cont=0`, yet `rgb`'s summary would come out
         *     // incomplete ⇒ its callers could not pass a frame-local pointer
         *     // ("points into a deeper scope (depth 1) than the arena this call may store it
         *     // it in").
         *
         * A `fn` value that did **not** come from a signed slot (a local copy, a parameter, an
         * unsigned slot) still has nothing to go on: the summary stays incomplete, conservatively.
         * Note this is the same trade the extern path makes: the clause is believed, and the whole
         * basis for believing it is that the author wrote it. */
        FieldDef *sfd = NULL;
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_FIELD)
            sfd = planField(e->u.call.callee);
        else if (e->kind == EX_METHOD && e->u.method.recv && e->u.method.recv->kind == EX_FIELD)
            sfd = planField(e->u.method.recv);
        if (sfd && sfd->hasEffects) {
            Vec *args = (e->kind == EX_CALL)  ? &e->u.call.args :
                        (e->kind == EX_METHOD) ? &e->u.method.args : &e->u.assoc.args;
            uint64_t addr = sfd->effAddrMask, cont = sfd->effContMask;
            for (size_t j = 0; j < args->len && j < EFF_MAX_PARAMS; j++) {
                if (!(((addr | cont) >> j) & 1u)) continue;      /* this argument is not kept */
                Expr *a = *(Expr **)vecAt(args, j);
                if (a && a->kind == EX_REF) a = a->u.ref.operand; /* `f(ref x)` stores `&x` */
                int pi = paramIndexByName(f, placeRootName(a));
                if (pi >= 0) {
                    if ((addr >> j) & 1u) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << pi)));
                    if ((cont >> j) & 1u) anSetContMask(f, anContMask(f) | (((uint64_t)1 << pi)));
                } else {
                    /* It keeps something that does not trace back to a parameter of mine: say so
                     * in the one way the summary has (`otherMask` = "not attributable"), which the
                     * call site reads as the strictest case. */
                    anSetOtherMask(f, anOtherMask(f) | (1u));
                }
            }
        } else {
            anSetEffUnknown(f, true);   /* nothing to go on -> incomplete, conservatively */
        }
    }
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && planCallee(e)) {
        bool seen = false;
        for (size_t k = 0; k < f->callees.len; k++)
            if (*(FuncDef **)vecAt(&f->callees, k) == planCallee(e)) { seen = true; break; }
        if (!seen) *(FuncDef **)vecPush(&f->callees) = planCallee(e);
    }
    switch (e->kind) {
    case EX_BIN:   collectEffectsExpr(c, f, e->u.bin.left);
                   collectEffectsExpr(c, f, e->u.bin.right); return;
    case EX_UN:    collectEffectsExpr(c, f, e->u.un.operand); return;
    case EX_REF:   collectEffectsExpr(c, f, e->u.ref.operand); return;
    case EX_EXT: collectEffectsExpr(c, f, e->u.ext_.call); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: collectEffectsExpr(c, f, e->u.deref.operand); return;
    case EX_SIGN:  collectEffectsExpr(c, f, e->u.sign.operand); return;
    case EX_CONV:  collectEffectsExpr(c, f, e->u.conv.operand); return;
    case EX_TRY:   collectEffectsExpr(c, f, e->u.try_.operand); return;
    case EX_SLICE: collectEffectsExpr(c, f, e->u.slice.obj);
                   collectEffectsExpr(c, f, e->u.slice.lo);
                   collectEffectsExpr(c, f, e->u.slice.hi); return;
    case EX_FIELD: collectEffectsExpr(c, f, e->u.field.obj); return;
    case EX_INDEX: collectEffectsExpr(c, f, e->u.index.obj);
                   collectEffectsExpr(c, f, e->u.index.index); return;
    case EX_COALESCE:
        collectEffectsExpr(c, f, e->u.coalesce.main);
        collectEffectsExpr(c, f, e->u.coalesce.fallback); return;
    case EX_NEW:   collectEffectsExpr(c, f, e->u.new_.count); return;
    case EX_CALL:
        for (size_t k = 0; k < e->u.call.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.call.args, k));
        return;
    case EX_METHOD:
        collectEffectsExpr(c, f, e->u.method.recv);
        for (size_t k = 0; k < e->u.method.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.method.args, k));
        return;
    case EX_ASSOC:
        for (size_t k = 0; k < e->u.assoc.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.assoc.args, k));
        return;
    /* The aggregate shapes hold expressions too, and a call inside one belongs in the
     * summary exactly like a call anywhere else: `[f(), g()]`, `some(f())`,
     * `point { x: f() }`. Letting them fall to `default` drops the call and makes the
     * summary of the enclosing function incomplete - the one direction the doc above
     * says must not happen (`effUnknown` exists so incompleteness is conservative). */
    case EX_ARRAYLIT:
        for (size_t k = 0; k < e->u.arraylit.elems.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.arraylit.elems, k));
        return;
    case EX_ENUMVAL:
        for (size_t k = 0; k < e->u.enumval.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.enumval.args, k));
        return;
    case EX_STRUCTLIT:
        for (size_t k = 0; k < e->u.lit.inits.len; k++)
            collectEffectsExpr(c, f, (*(FieldInit **)vecAt(&e->u.lit.inits, k))->value);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t k = 0; k < e->u.lambda.inits.len; k++)
            collectEffectsExpr(c, f, (*(FieldInit **)vecAt(&e->u.lambda.inits, k))->value);
        return;
    case EX_GENCALL:
        for (size_t k = 0; k < e->u.gencall.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.gencall.args, k));
        return;
    case EX_DYN:
        collectEffectsExpr(c, f, e->u.dynv.payload);   /* a call in the payload is a call */
        return;
    default: return;
    }
}

/**
 * @brief 算出一个函数的效应摘要：它往被交到手里的容器里存了什么。
 *
 * 只有地址流 `Addr(j)` 与内容流 `Cont(j)` 会约束实参；`new`、字面量、标量不约束。
 * `extern!` 声明按 `effects` 子句总结：没有子句时 32 以下每个参数都记为“已存”（安全默认）。
 *
 * @param c  checker。
 * @param f  要总结的函数。
 */
static void collectEffects(Checker *c, FuncDef *f) {
    /* An extern declaration has no body, so the summary comes from the signed clause or
    * is the worst case. Skipping this would leave the summary empty, which reads as
    * "stores nothing" and would accept a dangling pointer. */
    if (f->isExtern) {
        if (f->hasEffects) {
            anSetAddrMask(f, f->extAddrMask);
            anSetContMask(f, f->extContMask);
        } else {
            /* No clause means every parameter may be stored. That is nearly unusable, and it
            * is what a safe default should look like: sign the declaration to make it
            * usable. */
            uint64_t all = 0;
            for (size_t i = 0; i < f->params.len && i < EFF_MAX_PARAMS; i++) all |= ((uint64_t)1 << i);
            anSetAddrMask(f, all);
            anSetContMask(f, all);
        }
        anSetEffComplete(f, true);      /* the declaration is authoritative, as a body would be */
        anSetOtherMask(f, 0);
        return;
    }
    /* `FuncDef` comes from `arenaAllocZero`, so `callees` has no arena yet; without an
     * explicit `vecInit`, `vecPush` would allocate through a NULL arena and crash. */
    if (!f->callees.arena) vecInit(&f->callees, c->arena, sizeof(FuncDef *));
    /* The escape set decides which arena the calls in this body pass, so it has to be
     * known before the body is walked. */
    computeEscapes(c, f);
    /* 这里不写 `c->escapeesFor = (int)(size_t)f;` —— 把指针截断成 `int` 当"已算过"的键，
     * 而那个字段**只写不读**（死字段 + 指针截断，一并删掉）。将来若真需要记忆化，
     * 用 `FuncDef *` 或 `Sym *` 字段，而不是把地址塞进整数。*/
    if (getenv("EXTC_DUMP_EFFECTS")) fprintf(stderr, "[escapes-for] %s\n", FN(f));
    Vec fresh; vecInit(&fresh, c->arena, sizeof(const char *));
    collectFreshLocals(c->arena, &fresh, f->body);
    anSetFreshCount(f, (unsigned)fresh.len);
    collectEffectsStmt(c, f, f->body, &fresh);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects] %-22s toParam[Addr=0x%llx Cont=0x%llx Other=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] localAddr=%d fresh=%u callees=%zu\n",
                FN(f), (unsigned long long)anAddrMask(f), (unsigned long long)anContMask(f),
                (unsigned long long)anOtherMask(f),
                (unsigned long long)anHomeAddrMask(f), (unsigned long long)anHomeContMask(f),
                (int)anAddrFromLocal(f), anFreshCount(f), f->callees.len);
}

/**
 * @brief 判断一个值的深度是否由分配站点决定。
 *
 * 只有这类形状允许重算深度：模板期记录的是快照，电平求解器之后可能把站点从 home arena 移回
 * 块层级。重算只能调小 `rc->depth`（放宽检查），所以绑定、调用结果、来自参数的值一律按记录
 * 值处理（偏拒绝）。`targetDepth` 不能替代它：它为绑定调用 `lookup`，而 `runRefCheck` 在别
 * 的模块作用域里跑，找到的不是原来那条绑定。
 *
 * @param c     checker。
 * @param e     要分类的表达式；可为 NULL。
 * @param hops  已跟随的间接层数；走超过 32 即停。
 * @return      值可追溯到 `new` 或内建泛型分配时 true。
 */
static bool depthComesFromAlloc2(Checker *c, Expr *e, int hops) {
    if (!e || hops > 32) return false;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL: return true;
    /* A `dyn` value's depth comes from its payload, which is copied into the pool. */
    case EX_DYN: return depthComesFromAlloc2(c, e->u.dynv.payload, hops + 1);
    /* Follow the origin: `var v = { buf: new T[cap], ... }  return v` reports `v`, a
    * binding, at the moment the error is raised, and its depth is decided by the `new`
    * it came from. Measured: the diagnostic said `kind=4 dca=0 svd=0` while the depth
    * was 1, which was a false rejection. */
    case EX_IDENT: {
        /* `lookup` cannot be used here: by the closing pass the scope has been popped, so
        * it finds nothing (the first fix still reported `dca=0`). Use the binding that
        * resolution pinned onto the node instead. */
        Sym *sy = identBindOf(e);
        Expr *org = sy ? sy->origin : NULL;
        if (!org) return false;
        return depthComesFromAlloc2(c, org, hops + 1);
    }
    case EX_DEREF:
        return depthComesFromAlloc2(c, e->u.deref.operand, hops + 1);
    case EX_FIELD:
        return depthComesFromAlloc2(c, e->u.field.obj, hops + 1);
    case EX_SIGN:     return depthComesFromAlloc2(c, e->u.sign.operand, hops+1);
    case EX_SLICE:    return depthComesFromAlloc2(c, e->u.slice.obj, hops+1);
    case EX_COALESCE: return depthComesFromAlloc2(c, e->u.coalesce.main, hops+1)
                          || depthComesFromAlloc2(c, e->u.coalesce.fallback, hops+1);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (depthComesFromAlloc2(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, hops+1)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (depthComesFromAlloc2(c, *(Expr **)vecAt(&e->u.arraylit.elems, i), hops+1)) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (depthComesFromAlloc2(c, *(Expr **)vecAt(&e->u.enumval.args, i), hops+1)) return true;
        return false;
    default: return false;   /* a binding, a field, a call, or a dereference decides nothing */
    }
}


/**
 * @brief 读取一个值最终定下的深度。
 *
 * 分配站点读 `planArenaLevel`（不要回落到 `refDepth`，那个数可能已过期并跟 arena 层级矛盾）；
 * 绑定、字段、下标读 `refDepth`；`dyn` 载荷与容器形状取所含值的最大深度。
 *
 * @param e  表达式；NULL 返回 0。
 * @return   该值的深度；无要求时为 0。
 */
static int solvedDepth(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        /* Do not fall back to `refDepth`: that number can be stale and would then
        * contradict the arena level. */
        return arenaDepthOf(planArenaLevel(e));
    case EX_IDENT: case EX_FIELD: case EX_INDEX:
        return anRefDepth(e) > 0 ? anRefDepth(e) : 0;
    /* The payload is copied into the pool: its depth is this value's depth (family E). */
    case EX_DYN:      return solvedDepth(e->u.dynv.payload);
    case EX_SIGN:     return solvedDepth(e->u.sign.operand);
    case EX_DEREF:    return solvedDepth(e->u.deref.operand);
    case EX_SLICE:    return solvedDepth(e->u.slice.obj);
    case EX_COALESCE: {
        int a = solvedDepth(e->u.coalesce.main);
        int b = solvedDepth(e->u.coalesce.fallback);
        return a > b ? a : b;
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            int x = solvedDepth((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.arraylit.elems, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.enumval.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_CALL: {
        int d = 0;
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.call.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    default:
        /* 走到这里的只有两种形状：`EX_INT` 与 `EX_CONV`。两种都**不可能携带引用**：
         * 字面量没有引用；`EX_CONV` 的源类型被刻意收窄（只有 `ref void` 这一种指针来源，
         * 不做一般指针转换）⇒ **答 0 是语义，不是兜底**。
         *
         * 那为什么还留着这个 NOTE：它是**计数哨兵** —— 将来若有新的 `ExprKind` 走到这里，
         * 计数会变，那才是要查的信号（与 walker 预算的关系：本仓库限制 23 个手写遍历
         * `tools/check_walkers.py`，把这些 kind 逐个列出来会让它变成第 24 个，
         * 而"列全"在这里买不到任何正确性）。*/
        EXTC_DBG_NOTEF("solvedDepth: kind=%d answered 0 (INT/CONV only, both carry no reference)", (int)e->kind);
        return 0;
    }
}

/* The level a requirement carries when there is none: a literal, a call result, or the
 * value of a binding this analysis has no level for. It must be a number rather than a
 * sentinel, because requirements are combined with `min`, and it must be large enough
 * that it can never win that comparison, because "no requirement" is not a requirement --
 * reading it as 0 would make every such value look as if it had to outlive the frame. */
#define LEVEL_INF 1000000

/* The level of every binding of the function being decided, and the only state the level
 * pass keeps between rounds.
 *
 * It is a table rather than a field of `Sym` because a level and the depth a `Sym` already
 * carries are different questions with opposite monotonicity -- a depth only grows, a
 * level only shrinks -- and the repository's iron rule is that one fact has one place.
 * `LEVEL_INF` means nothing has demanded anything of the binding yet. */
typedef struct {
    Sym *sym;
    int  lv;
} SymLevel;

/* One entry of the cycle cut: a (value, level) pair that is on the walk right now.
 *
 * The walk follows a binding's publications, and a binding assigned from itself --
 * `s = s + x` inside a loop, which is what every fold looks like -- makes that graph
 * cyclic. The `hops` cap bounds the *depth* of the recursion but not the number of
 * *paths*: with a cycle and two edges per level there are 2^32 of them, and the compiler
 * simply did not return (no message, no position, flat memory). Measured on
 *
 *     fn acc<T>(a: T, b: T) -> T { var s: T = a  var i: i64 = 0
 *         while i < 3 { s = s + b  i = i + 1 }  return s }
 *
 * which is the shape of every generic fold (`sum<T>`, `max<T>`, `reduce<T>`).
 *
 * Cutting at a repeated pair is sound for what this walk is for: a level only ever gets
 * smaller, the pair carries the level being demanded, and the pass runs to a fixed point
 * over the publications -- so a demand this cut drops is applied by the next round,
 * starting from the state this one settled. */
typedef struct {
    Expr *val;
    int   target;
} LvlVisit;

/* Work budget for one level pass, in visited pairs.
 *
 * A backstop, not the fix: the cut above is what bounds the walk on a cycle, but a value
 * graph of diamonds can still branch. Hitting the budget is reported loudly and once --
 * a compiler that hangs has no message and no position, which is strictly worse than an
 * error saying so. Measured cost of the whole corpus is far below this: the pass visits
 * tens of pairs per ordinary function. */
#define LVL_STEP_BUDGET 4000000L

typedef struct {
    Vec      tbl;     /* SymLevel */
    Vec      path;    /* LvlVisit: the (value, level) pairs on the walk right now */
    long     steps;   /* pairs visited in this pass, against LVL_STEP_BUDGET */
    bool     loud;    /* the budget was already reported */
    FuncDef *fn;      /* the function being decided, for the diagnostic */
    Checker *c;       /* the checker, for the `EXTC_DBG_FX` counters */
    bool     fx;      /* `EXTC_DBG_FX`: count, or do not */
} LvlState;

static int  symLevel(LvlState *ls, Sym *sy);
static bool setSymLevel(LvlState *ls, Sym *sy, int lv);
static int  valueLevel(Checker *c, LvlState *ls, Expr *val, int hops);
/* The carrier chain of a `dyn Trait(x)` continues into its payload: the payload is copied into
 * the pool, so it must live as long as this value must. */

/* Walk a value's carrier chain and return the level the value itself has to live at.
 *
 * A level is a lower bound on lifetime: the smaller the number, the longer the storage
 * has to live, with 0 meaning "beyond this frame". The walk visits every allocation site
 * reachable through the value and lowers it to that level, and returns the smallest level
 * any part of the value carries.
 *
 * Params:
 *   c      - checker
 *   ls     - the levels of this function's bindings, as the pass currently knows them
 *   val    - the value to walk; may be NULL
 *   target - the level the value is being published at
 *   hops   - recursion guard (bindings may form cycles, as in `a = b  b = a`)
 *
 * Returns:
 *   The level the value has to live at, or `LEVEL_INF` when nothing about it demands a
 *   lifetime -- a literal, a call result, a binding with no level yet.
 *
 * Notes:
 *   - Only the forms that carry a value are followed. `*e` (the thing `e` points at),
 *     `&e` (the address of `e`) and `e[a:b]` (a view) are deliberately absent: following
 *     one is a false rejection rather than a missed promotion, because it
 *     pulls in the level of a value that has nothing to do with this one.
 *   - A binding is followed in both directions. Outwards, along what it holds, which is
 *     what reaches the allocation sites inside the value; inwards, to the binding's own
 *     level, which is where a demand that arrived from another publication enters. The
 *     descent uses the tighter of the two, because a binding that must outlive this frame
 *     cannot be holding storage that dies with it.
 *   - The answer is what travels along the edges between publications, which is why the
 *     pass that calls this runs to a fixed point over them. */
static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops);

/**
 * @brief 走一层值到分配站点的层级求解，带环切与步数预算。
 *
 * 体内每条递归边都经过这个包装，所以它维护的路径就是正在走的链，预算统计到达的每一对
 * （包括重复到达的，那正是环切存在的理由）。
 *
 * @param c       checker。
 * @param ls      电平求解状态（路径与步数预算）。
 * @param val     要解的值。
 * @param target  目的地要求的层级。
 * @param hops    已跟随的跳数；超过 32 返回 `LEVEL_INF`。
 * @return        值需要活到的层级；无要求时为 `LEVEL_INF`。
 */
static int levelOfValue(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    for (size_t i = 0; i < ls->path.len; i++) {
        LvlVisit *v = (LvlVisit *)vecAt(&ls->path, i);
        /* Already on this path, at this level: everything reachable from here was reached
         * when it was entered, and whatever this visit could add is picked up by the next
         * round of the fixed point. */
        if (v->val == val && v->target == target) return LEVEL_INF;
    }
    if (++ls->steps > LVL_STEP_BUDGET && !ls->loud) {
        ls->loud = true;
        Ctx *cx = (ls->fn && ls->fn->ctx) ? ls->fn->ctx : c->ctx;
        ctxError(cx, val->line, 1,
                 "This is a limit of the compiler, not something wrong with the program;"
                 " please report it together with the program that triggered it.",
                 "internal: the level solver walked more than %ld steps and was stopped",
                 (long)LVL_STEP_BUDGET);
    }
    LvlVisit *slot = (LvlVisit *)vecPush(&ls->path);
    slot->val = val;
    slot->target = target;
    int r = levelOfValue2(c, ls, val, target, hops);
    ls->path.len--;                       /* pop: the path is the current chain, not a set */
    return r;
}

/**
 * @brief 电平求解的主体：沿载体把一次发布追到它能触及的分配站点。
 *
 * 一次发布 `(value, at)` 说“这个值会落进活 `at` 层深的地方”，沿值能到达的每个分配站点也被
 * 要求活到 `at`。跟随关系：绑定走 `Sym.origin`；`ref` / `*` / `!` 走同一份存储；
 * slice/deref/sign 认对象的一部分；字段与下标住在对象/数组里；聚合走每个元素；调用不跟随
 * （被调者发布什么是它自己的事）。这是一个对数据的折叠，与顺序无关。
 *
 * @param c       checker。
 * @param ls      电平求解状态。
 * @param val     被发布的值。
 * @param target  目的地所在的 arena 层级；0 = 本帧之外。
 * @param hops    递归守卫：绑定可能成环（`a = b  b = a`）。
 * @return        值需要活到的层级。
 */
static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    if (dbgOn("EXTC_DBG_LV"))
        fprintf(stderr, "      [lv] kind=%-3d line=%-4d target=%-2d hops=%d\n",
                (int)val->kind, val->line, target, hops);
    switch (val->kind) {
    case EX_NEW:
    case EX_GENCALL:
        /* The site this whole walk was looking for. */        if (target < LEVEL_INF && (anMinAt(val) < 0 || target < anMinAt(val))) {
            anSetMinAt(val, target);
        }
        return target;
    case EX_IDENT: {
        Sym *sy = identBindOf(val);
        /* Reaching an allocation through a *value* answers "where did this number come
         * from", and only the three forms above, this one, and the containers below
         * preserve that. The three forms deliberately absent from this switch -- `*e`,
         * `&e` and `e[a:b]` -- do not: `*e` is the thing `e` points at, and `&e` is the
         * address of `e`, so neither is the value the sub-expression holds.
         * Following one anyway is a false rejection, not a missed
         * promotion: in `examples/alloc-in-block`, `return total` is followed through
         * `total = *q` into `q`, and from there into the allocation, so the `alloc` inside
         * the block is told "must outlive this frame" although nothing outlives the
         * block; the function has no home arena to give it, and the generated C references
         * `__extc_home`, which does not exist there. */

        if (dbgOn("EXTC_DBG_LV"))
            fprintf(stderr, "      [lv] ident=%-5s sym=%s origin=%s target=%d\n",
                    val->u.ident.name, sy ? "yes" : "NULL",
                    (sy && sy->origin) ? "yes" : "NULL", target);
        if (dbgOn("EXTC_DBG_LV") && sy && sy->origin)
            fprintf(stderr, "      [lv]   -> origin kind=%d line=%d (sym %s)\n",
                    (int)sy->origin->kind, sy->origin->line, sy->name ? sy->name : "?");
        /* A binding's level is its own: what it holds now may have arrived from several
         * statements, and `origin` remembers only one of them. It is computed by
         * `levelPass` and read here, which is how the requirement crosses from one
         * publication record to another. */
        /* Two edges leave a binding, and both are followed: outwards along what the
         * binding holds, which is what reaches the allocation sites inside the value, and
         * inwards to the binding's own level, which is where a requirement that arrived
         * from another publication enters. A binding's level is its own because what it
         * holds may have arrived from several statements, and `origin` remembers only one
         * of them. */
        int fromSym = sy ? symLevel(ls, sy) : LEVEL_INF;
        /* The level the value is being handed to also reaches the binding on the way, and
         * this is the edge that carries a demand *through* a binding.
         *
         * `out = h` hands `h` to `out`, which is level 0, so whatever `h` holds has to
         * reach level 0 as well -- otherwise the demand stops at the store and never
         * arrives at the allocation behind `h`. Lowering a binding's own level is the safe
         * direction: a binding that lives longer than it had to costs memory and cannot
         * leave a reference dangling. */
        if (sy && target < LEVEL_INF) setSymLevel(ls, sy, target);
        /* And the other direction of the same edge: a binding that is already known to
         * live longer than this walk demands tightens the walk.
         *
         * Without it the walk keeps carrying the level of the store it started from. `out`
         * is at level 0 because it is returned, while the store `out = h` is recorded at
         * level 1, and entering `out` at 1 leaves everything `out` holds one level too deep:
         * the `alloc` behind `h` stays at the block level while the struct carrying it is
         * returned to the caller. */
        if (sy) {
            int cur = symLevel(ls, sy);
            if (cur < target) target = cur;
        }
        /* A binding is *not* descended into here.
         *
         * What a binding holds may have arrived from several assignments, and one
         * expression field cannot describe a join over them: whatever it remembers is the
         * last one written, which is one arm of an `if` chosen by source order. Measured on
         * `tests/arena-promoted/C3_if_join_wholevalue`, where `h = g` is followed by
         * `h = zero` and every walk of `h` therefore ended at the empty box in the other
         * arm, never reaching the allocation inside `g`.
         *
         * The records already hold every assigned expression, so the walk descends into
         * those instead -- `levelPass` walks the values recorded for a binding, which is
         * the join done properly rather than a race between statements. All that is read
         * here is the binding's own level.
         */
        /* Descend into the expression the binding was actually given, never into the
         * flattened root of its origin chain: the root belongs to whichever binding the
         * chain ended at, and `out = h` following `h = zero` therefore pointed at the
         * literal that initializes `zero`.
         *
         * This is the chain of assignments, and the level asked of the value is the
         * smaller of the binding's own level and the level being demanded here. */
        /* The walk descends at the tighter of the binding's level and the level demanded
         * of it: if the binding has to outlive the frame, so does the storage inside the
         * value it holds. */
        int inner = fromSym < target ? fromSym : target;
        int best = fromSym;
        /* Every expression ever assigned to the binding, not just the last one.
         *
         * What a binding holds at a control-flow merge is one of the values written to it,
         * and a single field cannot describe that: `Sym.heldSrc` remembers the assignment
         * that came last in source order, so `h = g` followed by `h = zero` made every walk
         * of `h` end at the empty box and never reach the allocation inside `g`. The
         * publications recorded while the body was checked are the authority -- they hold
         * each assigned expression, and walking all of them is the merge done properly
         * rather than a race between statements. */
        bool viaRecords = false;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *alt = *(StoreSite **)vecAt(&c->stores, i);
            if (!alt || alt->fn != ls->fn) continue;
            if (!alt->target || alt->target->kind != EX_IDENT) continue;
            if (identBindOf(alt->target) != sy) continue;
            viaRecords = true;
            int v = levelOfValue(c, ls, alt->value, inner, hops + 1);
            if (v < best) best = v;
        }
        /* A binding with no publication of its own -- only ever initialized, or written
         * through a projection -- is still described by the expression it was given. */
        if (!viaRecords) {
            /* An identifier that never resolved has no binding to walk, and reaching
             * `sy->heldSrc` with a NULL `sy` is a SIGSEGV, found by tools/fuzz.py from a
             * three-line program (`fn make<T>(x: T) -> pair<T> { return { a: make, b: x } impl }`).
             * `best` is the answer for a binding with no publication to follow, and this cannot
             * hide an escape: an unresolved name is reported as an error, so no accepted program
             * takes this path. */
            if (!sy) return best;
            Expr *held = sy->heldSrc ? sy->heldSrc : sy->origin;
            if (!held) return fromSym;
            int v = levelOfValue(c, ls, held, inner, hops + 1);
            if (v < best) best = v;
        }
        return best;
    }
    /* The join node of an `if`: one value with two operands, either of which can be what
     * the destination ends up holding. Both are walked at the same level, and the answer is
     * the smaller of the two.
     *
     * Leaving this form out is the last thing keeping the whole-value if-join unsound.
     * `out = h` stores a join node, so a walk without this case returns "no requirement"
     * at the store itself and never reaches the allocation behind either arm -- on
     * `tests/arena-promoted/C3_if_join_wholevalue`, the site would stay at the block
     * level while the struct holding it was returned. */
    case EX_BIN: {
        int a = levelOfValue(c, ls, val->u.bin.left, target, hops + 1);
        int b = levelOfValue(c, ls, val->u.bin.right, target, hops + 1);
        /* The join takes the smaller of its arms, and that number belongs to the operands
         * as well as to the join.
         *
         * Each arm is a value the destination can end up holding, so an arm's bindings are
         * held to what the *other* arm needs: `h = g` in one arm and `h = zero` in the other
         * means either one is what `h` holds. When both arms are reached through the join
         * and neither is walked on its own, the binding inside the arm carrying the
         * allocation never learns what the join learned, and the demand stops at the arm
         * carrying nothing. Measured on `tests/arena-promoted/C3_if_join_wholevalue`. */
        int r = a < b ? a : b;
        /* Both arms are always revisited at the joined level. The first pass through them
         * is what produced `a` and `b`, but a walk can stop at a binding before reaching the
         * sites inside the value it holds, and then the level the join just settled on has
         * not reached those sites yet. The revisit is a no-op once they agree, so the
         * iteration still terminates. */
        levelOfValue(c, ls, val->u.bin.left, r, hops + 1);
        levelOfValue(c, ls, val->u.bin.right, r, hops + 1);
        return r;
    }
    case EX_SIGN:  return levelOfValue(c, ls, val->u.sign.operand, target, hops + 1);
    case EX_FIELD: return levelOfValue(c, ls, val->u.field.obj, target, hops + 1);
    case EX_INDEX: return levelOfValue(c, ls, val->u.index.obj, target, hops + 1);
    case EX_COALESCE: {
        /* Both sides can become the result, so both are published, and either may be the
         * one that carries the requirement. */
        int a = levelOfValue(c, ls, val->u.coalesce.main, target, hops + 1);
        int b = levelOfValue(c, ls, val->u.coalesce.fallback, target, hops + 1);
        return a < b ? a : b;
    }
    case EX_STRUCTLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.lit.inits.len; i++) {
            int x = levelOfValue(c, ls, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value,
                                 target, hops + 1);
            if (x < r) r = x;
        }
        /* The literal is how a binding gets its fields, and the field table stores the
         * expression that wrote each one. Reading it here covers the same fact from the
         * other side: `{ p: null, q: x }` names `x` in the literal, while a later read of
         * `g.q` is answered from the table, and the walk has to reach the allocation either
         * way. */
        for (size_t i = 0; i < c->allSyms.len; i++) {
            Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
            if (sy && sy->origin == val) promoteFieldsAt(c, sy, target, hops + 1);
        }
        return r;
    }
    case EX_ARRAYLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++) {
            int x = levelOfValue(c, ls, *(Expr **)vecAt(&val->u.arraylit.elems, i), target, hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ENUMVAL: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.enumval.args.len; i++) {
            int x = levelOfValue(c, ls, *(Expr **)vecAt(&val->u.enumval.args, i), target, hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    default:
        /* A scalar, a literal, a call result: nothing that reaches an allocation site
         * through this value. A callee's own sites are settled by the callee's
         * publications, and what it stores into a parameter is settled at the call site
         * from the arguments. */
        return LEVEL_INF;
    }
}

/**
 * @brief 读取一个绑定当前已知需要活到的层级。
 *
 * 绑定的层级不是单条语句决定的：它现在持有的值可能来自多条语句，它被发布到哪里也可能要求比
 * 值本身更长的寿命。两者都是发布记录之间的边，这张表就是它们相遇的地方。
 *
 * @param ls  电平求解状态。
 * @param sy  绑定。
 * @return    该绑定的层级。
 */
static int symLevel(LvlState *ls, Sym *sy) {
    if (!ls || !sy) return LEVEL_INF;
    if (ls->fx) ls->c->fxSymCalls++;
    for (size_t i = 0; i < ls->tbl.len; i++) {
        if (ls->fx) ls->c->fxSymCmp++;
        SymLevel *e = (SymLevel *)vecAt(&ls->tbl, i);
        if (e->sym == sy) return e->lv;
    }
    return LEVEL_INF;
}

/**
 * @brief 调低一个绑定的层级，并报告是否真的动了。
 *
 * 只有调低这一个方向：层级是要求，要求只会累积。
 *
 * @param ls  电平求解状态。
 * @param sy  绑定。
 * @param lv  新层级。
 * @return    层级因此变小时 true。
 */
static bool setSymLevel(LvlState *ls, Sym *sy, int lv) {
    if (!ls || !sy || lv >= LEVEL_INF) return false;
    if (ls->fx) ls->c->fxSymCalls++;
    for (size_t i = 0; i < ls->tbl.len; i++) {
        if (ls->fx) ls->c->fxSymCmp++;
        SymLevel *e = (SymLevel *)vecAt(&ls->tbl, i);
        if (e->sym != sy) continue;
        if (lv < e->lv) {
            e->lv = lv; return true;
        }
        return false;
    }
    SymLevel *e = (SymLevel *)vecPush(&ls->tbl);
    e->sym = sy;
    e->lv  = lv;
    return true;
}

/**
 * @brief 读取一个被存值从它命名的绑定上带来的层级。
 *
 * 这是 `levelOfValue` 的另一半：那一个把值往下走到它里面的分配站点，这一个把值往上走到构成
 * 它的绑定；两者一起迭代到数字不再移动。
 *
 * @param c     checker。
 * @param ls    电平求解状态。
 * @param val   值表达式。
 * @param hops  递归守卫跳数。
 * @return      该值携带的层级；无要求时为 `LEVEL_INF`。
 */
static int valueLevel(Checker *c, LvlState *ls, Expr *val, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    /* `dest` is the level of the place this value is being handed to, and it takes part in
     * every minimum below.
     *
     * Where two arms of an `if` write the same binding, the binding holds one value or the
     * other and must live long enough for both. That value is a join node, and one arm of
     * it can be a literal -- `h = g` in one arm and `h = zero` in the other leaves one arm
     * with no level of its own at all. The destination is the number that covers the case
     * the arm has nothing to say about, so leaving it out makes the join look like it
     * demands nothing, and the site behind the other arm is never told it has to outlive
     * the block. */
    switch (val->kind) {
    case EX_IDENT:  return symLevel(ls, identBindOf(val));
    /* `dyn Trait(x)`: the carrier chain continues into the payload (it is copied into the pool,
     * so it has to live as long as this value does). */
    case EX_DYN:    return valueLevel(c, ls, val->u.dynv.payload, hops + 1);
    /* The join node of an `if`: `h = g` in one arm and `h = zero` in the other is one
     * value with two operands. Since either operand can be what the binding ends up
     * holding, the requirement is the smaller of the two, compared with `dest` as well. */
    case EX_BIN: {
        int a = valueLevel(c, ls, val->u.bin.left, hops + 1);
        int b = valueLevel(c, ls, val->u.bin.right, hops + 1);
        return a < b ? a : b;
    }
    case EX_SIGN:   return valueLevel(c, ls, val->u.sign.operand, hops + 1);
    case EX_FIELD:  return valueLevel(c, ls, val->u.field.obj, hops + 1);
    case EX_INDEX:  return valueLevel(c, ls, val->u.index.obj, hops + 1);
    case EX_COALESCE: {
        int a = valueLevel(c, ls, val->u.coalesce.main, hops + 1);
        int b = valueLevel(c, ls, val->u.coalesce.fallback, hops + 1);
        return a < b ? a : b;
    }
    case EX_STRUCTLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.lit.inits.len; i++) {
            int x = valueLevel(c, ls, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value,
                               hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ARRAYLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++) {
            int x = valueLevel(c, ls, *(Expr **)vecAt(&val->u.arraylit.elems, i), hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ENUMVAL: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.enumval.args.len; i++) {
            int x = valueLevel(c, ls, *(Expr **)vecAt(&val->u.enumval.args, i), hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_NEW:
    case EX_GENCALL:
        return anMinAt(val) >= 0 ? anMinAt(val) : anLexicalLevel(val);
    default:
        return LEVEL_INF;
    }
}

/**
 * @brief 跑电平遍历：折叠检查函数体时记录的发布。
 *
 * 它**不**取数据流的不动点：电平求解读自己的表（由函数体遍历时的发布播种），并在 DFA 的数字
 * 被写回 `Sym.refDepth` **之前**跑。
 *
 * @param c  checker。
 * @param f  要跑电平遍历的函数。
 */
static void levelPass(Checker *c, FuncDef *f) {
    LvlState ls;
    vecInit(&ls.tbl, c->arena, sizeof(SymLevel));
    vecInit(&ls.path, c->arena, sizeof(LvlVisit));
    ls.steps = 0;
    ls.loud = false;
    ls.fn = f;
    ls.c  = c;
    ls.fx = c->fxOn;
    if (ls.fx) c->fxLvlPasses++;

    /* Step one: how long does each binding have to live?
     *
     * A binding's level is the smallest level among the values stored into it and the
     * stores that publish it onwards, and a value read from a binding depends on that level
     * in turn, so this is a fixed point of its own. It converges before anything is
     * decided, which is what keeps the decision independent of the order the records are
     * visited in.
     *
     * Only the lowering direction exists, and a store only lowers the binding it names: a
     * value that arrives from somewhere shallower makes the binding live longer, never
     * shorter. */
    for (int round = 0; round < 64; round++) {
        bool moved = false;
        if (ls.fx) c->fxLvlRounds1++;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (ls.fx) { c->fxStoresSeen++; if (st && st->fn == f) c->fxStoresOwn++; }
            if (!st || st->fn != f) continue;   /* another body's publication */
            if (!st->target || st->target->kind != EX_IDENT) continue;
            Sym *d = identBindOf(st->target);
            if (!d) continue;
            int at = st->at;
            int v  = valueLevel(c, &ls, st->value, 0);
            if (v < at) at = v;
            if (setSymLevel(&ls, d, at)) { moved = true; if (ls.fx) c->fxLvlMoved++; }
        }
        if (!moved) break;
    }

    /* Step two: how long does each allocation site have to live?
     *
     * For every publication, the level to reach is the tighter of the level the store was
     * accounted at and the level of the destination -- the destination may be a binding
     * found in step one to live longer than the block the store happened to sit in, and
     * `out = h` recorded at level 1 with `out` returned at level 0 is exactly that case.
     *
     * When the destination is tighter, the requirement reaches everything the value was
     * built from, which is what the walk below does. That extra step is deliberately
     * narrow: a store into a place that does not outlive the store itself says nothing
     * new about the value, and walking it anyway moves the `new` inside a
     * loop from the loop arena to the frame arena, where it is not reclaimed each
     * round -- the per-block refinement the arena tests exist to protect. */
    for (int round = 0; round < 64; round++) {
        bool moved = false;
        if (ls.fx) c->fxLvlRounds2++;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (ls.fx) { c->fxStoresSeen++; if (st && st->fn == f) c->fxStoresOwn++; }
            if (!st || st->fn != f) continue;   /* another body's publication */
            int at = st->at;
            Sym *dst = (st->target && st->target->kind == EX_IDENT)
                       ? identBindOf(st->target) : NULL;
            int dl = dst ? symLevel(&ls, dst) : LEVEL_INF;
            if (dl < at) at = dl;
            int v = valueLevel(c, &ls, st->value, 0);
            if (v < at) at = v;
            bool carrying = typeContainsRef(c->tt, tsub(c, st->value->type))
                            || mentionsParam(st->value->type);
            /* Numbers inside a value still name a place (`cell.value = n` reaches `cell`),
             * so a walk of a value that carries no reference would drag bindings along with
             * it for no reason. */            if (!carrying) continue;
            levelOfValue(c, &ls, st->value, at, 0);
            /* A binding can be assigned more than once, and it then holds one of those
             * values. `Sy`->origin` remembers only the last assignment, so a walk that
             * followed it would stop at whichever arm happened to be written last --
             * on `C3_if_join_wholevalue`, `h = g` is followed by
             * `h = zero`, so the origin names the empty box and the
             * allocation behind `g` is unreachable from every later store of `h`.
             *
             * Every assigned value is in the record table, so walking them all is what
             * covers "one of these". Each is walked at the destination's level, which is
             * the level anything `h` holds has to reach. */            if (dst && dl < LEVEL_INF) {
                for (size_t k = c->storeBase; k < c->stores.len; k++) {
                    StoreSite *alt = *(StoreSite **)vecAt(&c->stores, k);
                    if (ls.fx) { c->fxAliasWalks++; if (alt && alt->fn == f) c->fxStoresOwn++; }
                    if (!alt || alt->fn != f) continue;
                    if (alt == st || !alt->target) continue;
                    if (alt->target->kind != EX_IDENT) continue;
                    if (identBindOf(alt->target) != dst) continue;
                    if (!typeContainsRef(c->tt, tsub(c, alt->value->type))
                        && !mentionsParam(alt->value->type)) continue;
                    /* The recorded value, not its origin. `originOf` flattens a chain of
                     * bindings down to the expression at the root of the chain, and that
                     * expression belongs to whichever binding the chain happened to end
                     * at: `out = h` following `h = zero` flattened to the literal that
                     * initializes `zero`, so descending through origins landed in an
                     * unrelated binding's empty box. The record holds the expression that
                     * was actually assigned, which is the arm itself. */
                    levelOfValue(c, &ls, alt->value, dl, 0);
                }
            }
            if (dst && dl < st->at && setSymLevel(&ls, dst, at)) { moved = true; if (ls.fx) c->fxLvlMoved++; }
        }
        if (!moved) break;
    }
    if (ls.fx) {
        c->fxTblSum += (long)ls.tbl.len;
        if ((long)ls.tbl.len > c->fxTblMax) c->fxTblMax = (long)ls.tbl.len;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (st && st->fn == f) c->fxTailRecords++;
        }
        if (getenv("EXTC_DBG_FX_VERBOSE"))
            fprintf(stderr, "[fx] %-24s tbl=%-5zu seen=%-9ld own=%-9ld symCmp=%-12ld"
                            " rounds=%ld/%ld moved=%ld\n",
                    f->name ? f->name : "?", ls.tbl.len, c->fxStoresSeen, c->fxStoresOwn,
                    c->fxSymCmp, c->fxLvlRounds1, c->fxLvlRounds2, c->fxLvlMoved);
    }
}



/* ------------------------------------------------ opened and never closed (warn)
 *
 * A file descriptor is an operating-system resource, not memory: nothing closes it on
 * the program's behalf, and the process holds it until it ends. The compiler cannot see
 * the kernel's table, but there is one shape of leak it *can* prove -- a handle that
 * never leaves the function and is never closed -- and that shape is a **warning** here
 * (decision 79). Everything else stays quiet on purpose: a handle handed to another
 * function, returned, or stored may well be closed there, and guessing would reject
 * correct programs.
 *
 * Why a warning and not an error: keeping a handle open until the process ends is a
 * legal choice (a log file, a pipe the program writes to and lets the OS reap), and the
 * compiler has no way to tell that intention from a mistake -- what it has is proof that
 * *nobody* closes it. So the proof is reported, loudly and with a position, and the
 * decision stays with the program. `-w` turns it off like any other warning.
 *
 * What makes a type a resource is the library's own protocol: **a struct that declares a
 * `close` method**. No library name appears in the compiler, the same way `slice` is a
 * protocol (`data` + `len`) and not a name codegen recognizes. `std::io`'s `writer` has
 * only `flush` and its `reader` has no `close`, so neither is a resource -- opting in is
 * a one-word decision the library makes.
 */
typedef struct {
    const char *cname;     /* the generated-C name: unique inside one function */
    const char *name;      /* the name the user wrote, for the diagnostic */
    int         line;
    bool        closed;    /* some `close` call can reach it */
    bool        escaped;   /* handed on, returned or stored: not our business */
    Vec         aliases;   /* const char*: `let g = f` shares one obligation */
} FdOblig;

/**
 * @brief 判断类型是否声明了 `close` 方法（资源协议）。
 *
 * @param t  类型。
 * @return   其 struct 上声明了 `close` 时 true。
 */
static bool typeIsResource(Type *t) {
    Type *b = t ? ttBase(t) : NULL;
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return false;
    for (size_t i = 0; i < b->sdef->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&b->sdef->methods, i);
        if (m && m->name && strcmp(m->name, "close") == 0) return true;
    }
    return false;
}

/**
 * @brief 在资源义务表里按生成名找义务（别名也算）。
 *
 * @param obs    义务表（`FdOblig *`）。
 * @param cname  生成 C 名；NULL 返回 NULL。
 * @return       找到的义务；没有则 NULL。
 */
static FdOblig *obligFind(Vec *obs, const char *cname) {
    if (!cname) return NULL;
    for (size_t i = 0; i < obs->len; i++) {
        FdOblig *o = *(FdOblig **)vecAt(obs, i);
        if (o->cname && strcmp(o->cname, cname) == 0) return o;
        for (size_t j = 0; j < o->aliases.len; j++)
            if (strcmp(*(const char **)vecAt(&o->aliases, j), cname) == 0) return o;
    }
    return NULL;
}

/**
 * @brief 把所有义务标成“已交出”。
 *
 * 用于本遍历不认识的形状：漏报只是少一条诊断，误报会拒绝正确程序，所以未知方向选择沉默。
 *
 * @param obs  义务表。
 */
static void obligEscapeAll(Vec *obs) {
    for (size_t i = 0; i < obs->len; i++) (*(FdOblig **)vecAt(obs, i))->escaped = true;
}

/**
 * @brief 在表达式里追踪资源义务：被关闭还是被交出。
 *
 * `f.close()` 是履行；接收者本身不算交出（`f.put(x)` 只是通过句柄写入）。
 *
 * @param c       checker。
 * @param e       表达式；NULL 忽略。
 * @param obs     义务表。
 * @param escape  该位置的值离开本函数时为 true。
 */
static void obligExpr(Checker *c, Expr *e, Vec *obs, bool escape) {
    if (!e) return;
    switch (e->kind) {
    case EX_IDENT: {
        FdOblig *o = obligFind(obs, planCName(e));
        if (o && escape) o->escaped = true;
        return;
    }
    case EX_METHOD: {
        /* `f.close()` is the discharge. The receiver is not an escape: `f.put(x)` writes
         * through the handle and leaves the file exactly as open as it was. */
        Expr *recv = e->u.method.recv;
        if (recv && recv->kind == EX_IDENT && e->u.method.name &&
            strcmp(e->u.method.name, "close") == 0) {
            FdOblig *o = obligFind(obs, planCName(recv));
            if (o) o->closed = true;
        }
        obligExpr(c, recv, obs, false);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.method.args, i), obs, true);
        return;
    }
    case EX_FIELD:
        /* `f.fd`: reading a field of the handle does not hand the handle on. */
        obligExpr(c, e->u.field.obj, obs, false);
        return;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.call.args, i), obs, true);
        if (e->u.call.callee) obligExpr(c, e->u.call.callee, obs, false);
        return;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.assoc.args, i), obs, true);
        return;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.gencall.args, i), obs, true);
        return;
    case EX_DYN:
        obligExpr(c, e->u.dynv.payload, obs, true);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.enumval.args, i), obs, true);
        return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            obligExpr(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, obs, true);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            obligExpr(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value, obs, true);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.arraylit.elems, i), obs, true);
        return;
    case EX_INDEX:
        obligExpr(c, e->u.index.obj, obs, true);
        obligExpr(c, e->u.index.index, obs, true);
        return;
    case EX_SLICE:
        obligExpr(c, e->u.slice.obj, obs, true);
        obligExpr(c, e->u.slice.lo, obs, true);
        obligExpr(c, e->u.slice.hi, obs, true);
        return;
    /* The value passes straight through these, keeping whatever the position means. */
    case EX_BIN:      obligExpr(c, e->u.bin.left, obs, escape);
                      obligExpr(c, e->u.bin.right, obs, escape); return;
    case EX_UN:       obligExpr(c, e->u.un.operand, obs, escape); return;
    case EX_REF:      obligExpr(c, e->u.ref.operand, obs, escape); return;
    case EX_EXT: obligExpr(c, e->u.ext_.call, obs, escape); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:    obligExpr(c, e->u.deref.operand, obs, escape); return;
    case EX_SIGN:     obligExpr(c, e->u.sign.operand, obs, escape); return;
    case EX_TRY:      obligExpr(c, e->u.try_.operand, obs, escape); return;
    case EX_CONV:     obligExpr(c, e->u.conv.operand, obs, escape); return;
    case EX_COALESCE: obligExpr(c, e->u.coalesce.main, obs, escape);
                      obligExpr(c, e->u.coalesce.fallback, obs, escape); return;
    case EX_NEW:      obligExpr(c, e->u.new_.count, obs, true); return;
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL: return;
    default:          obligEscapeAll(obs); return;   /* an unknown shape: stay quiet */
    }
}

/**
 * @brief 在语句里追踪资源义务，走遍其表达式。
 *
 * @param c    checker。
 * @param s    语句；NULL 忽略。
 * @param obs  义务表。
 */
static void obligStmt(Checker *c, Stmt *s, Vec *obs) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        obligExpr(c, s->u.trap_.msg, obs, false);
        return;
    case ST_YIELD:
        obligExpr(c, s->u.yield_.value, obs, true);
        return;
    case ST_VAR: {
        /* A local of a resource type is where the obligation is born. An initializer that
         * is another handle makes this one an alias: one `close` serves both, because
         * both name the same descriptor. */
        if (typeIsResource(s->type)) {
            const char *cn = planCName(s) ? planCName(s) : s->u.var.name;
            Expr *init = s->u.var.init;
            FdOblig *same = (init && init->kind == EX_IDENT)
                          ? obligFind(obs, planCName(init)) : NULL;
            if (same) {
                *(const char **)vecPush(&same->aliases) = cn;
            } else {
                FdOblig *o = (FdOblig *)arenaAllocZero(c->arena, sizeof *o);
                o->cname = cn;
                o->name  = s->u.var.name;
                o->line  = s->line;
                vecInit(&o->aliases, c->arena, sizeof(const char *));
                *(FdOblig **)vecPush(obs) = o;
            }
        }
        obligExpr(c, s->u.var.init, obs, false);
        return;
    }
    case ST_ASSIGN: {
        /* Overwriting a handle loses the old descriptor, which is a leak as well -- but
         * the old value may have been closed by whoever handed the new one over, so the
         * safe direction here is silence. The value being stored is out of our hands. */
        if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
            FdOblig *o = obligFind(obs, planCName(s->u.assign.target));
            if (o) o->escaped = true;
        }
        obligExpr(c, s->u.assign.value, obs, true);
        return;
    }
    case ST_DOMAIN: obligExpr(c, s->u.domain_.callee, obs, false);
                    obligStmt(c, s->u.domain_.body, obs); return;
    case ST_IF:    obligExpr(c, s->u.ifs.cond, obs, false);
                   obligStmt(c, s->u.ifs.thenBody, obs);
                   obligStmt(c, s->u.ifs.elseBody, obs); return;
    case ST_WHILE: obligExpr(c, s->u.whiles.cond, obs, false);
                   obligStmt(c, s->u.whiles.body, obs); return;
    case ST_RETURN: obligExpr(c, s->u.ret.value, obs, true); return;   /* handed to the caller */
    case ST_EXPR:  obligExpr(c, s->u.expr.expr, obs, false); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            obligStmt(c, *(Stmt **)vecAt(&s->u.block.stmts, i), obs);
        return;
    case ST_MATCH:
        obligExpr(c, s->u.match.scrutinee, obs, true);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            obligStmt(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body, obs);
        return;
    case ST_BREAK: case ST_CONTINUE:
    default: return;
    }
}

/**
 * @brief 警告函数里打开却从未关闭的每个描述符。
 *
 * 刻意做成流不敏感：函数里任何一处 `close` 都算数。要证明某条**路径**会关闭需要路径敏感分析，
 * 而拒绝一个在两条路径之一关闭文件的程序是误报。第二次 `close` 在库里是空操作，所以接受的
 * 形状在运行期确实已关闭。
 *
 * @param c  checker。
 * @param f  要检查的函数。
 */
static void checkOpenHandles(Checker *c, FuncDef *f) {
    if (!f || !f->body || f->isExtern) return;
    Vec obs; vecInit(&obs, c->arena, sizeof(FdOblig *));
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        obligStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i), &obs);
    for (size_t i = 0; i < obs.len; i++) {
        FdOblig *o = *(FdOblig **)vecAt(&obs, i);
        if (o->closed || o->escaped) continue;
        ckWarn(c, o->line,
               "This is a resource the operating system hands out, not memory: nothing gives it"
               " back for you, and the process holds it until it ends -- a descriptor, a"
               " terminal left in raw mode. Close it where its life ends (`close()` on the"
               " value), or hand it to a function that takes over. (`-w` turns this warning"
               " off; keeping it until the process ends is a legal choice, it just has to be a"
               " visible one.)",
               "`%s` is opened here and nothing in this function closes it", o->name);
    }
}

/* name -> symbols, built incrementally, for the sweep at the end of checkFunc.
 *
 * That sweep walks *every* symbol of the module for *every* function (the vecAt read histogram:
 * 21,102,050 reads, ~6,400 per function at N=3200 -- the checker's x4.3). Its effect is "each
 * symbol's refDepth = max(refDepth, dataflow depth of its name)", which can be produced the other
 * way round if the symbols of a name can be found quickly.
 *
 * The first attempt rebuilt a flat index per module and was 2.6x SLOWER: `allSyms` grows while
 * checking, so a length-guarded full rebuild turned the O(N) build into O(N^2). This one appends
 * only the symbols added since the last call (amortised O(1) each, arena-backed Vec), and falls
 * back to the original loop -- permanently, for the rest of the run -- the moment anything is not
 * exactly the list it indexed (table window full, list shrank, different arena/first symbol). */
typedef struct { const char *name; Vec syms; int used; } SymBucket;
static SymBucket *g_sb;
static size_t g_sbCap, g_sbBuilt;
static Arena *g_sbArena;
static const Sym *g_sbFirst;
static int g_sbBad;

/**
 * @brief 用 FNV-1a 哈希一个 C 名字。
 *
 * @param p  以 NUL 结尾的名字。
 * @return   哈希值。
 */
static size_t sgHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (const char *q = p; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211u; }
    return h;
}
/**
 * @brief 在符号索引里找一个桶，必要时新建。
 *
 * @param name    要查找的生成名。
 * @param create  没有时是否新建。
 * @return        桶；16 步线性探测内没有空位时 NULL，调用方退回线性查找。
 */
static SymBucket *symBucket(const char *name, int create) {
    size_t h = sgHash(name) & (g_sbCap - 1);
    for (int step = 0; step < 16; step++, h = (h + 1) & (g_sbCap - 1)) {
        SymBucket *b = &g_sb[h];
        if (b->used) {
            if (strcmp(b->name, name) == 0) return b;
            continue;
        }
        if (!create) return NULL;
        b->used = 1; b->name = name;
        vecInit(&b->syms, g_sbArena, sizeof(Sym *));
        return b;
    }
    return NULL;                                   /* window full: caller falls back */
}
/**
 * @brief 把自上次以来新增的符号追加进符号索引。
 *
 * 只增量追加（每次摊还 O(1)）。一旦发现索引不再与 `c->allSyms` 完全一致（表窗口满、列表缩小、
 * arena 或首符号不同），就把 `g_sbBad` 置位，此后整轮永久退回线性查找。
 *
 * @param c  checker。
 */
static void symIdxSync(Checker *c) {
    const Sym *first = c->allSyms.len ? *(Sym **)vecAt(&c->allSyms, 0) : NULL;
    if (g_sbArena != c->arena || g_sbFirst != first || c->allSyms.len < g_sbBuilt) {
        size_t cap = 4096;
        g_sb = arenaAllocZero(c->arena, cap * sizeof *g_sb);
        g_sbCap = cap; g_sbBuilt = 0; g_sbBad = 0;
        g_sbArena = c->arena; g_sbFirst = first;
    }
    if (g_sbBad) return;
    for (size_t i = g_sbBuilt; i < c->allSyms.len; i++) {
        Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
        if (!sy || sy->line < 0 || !sy->cname) continue;
        SymBucket *b = symBucket(sy->cname, 1);
        if (!b) { g_sbBad = 1; return; }           /* incomplete: never use it again */
        *(Sym **)vecPush(&b->syms) = sy;
    }
    g_sbBuilt = c->allSyms.len;
}

/**
 * @brief 为一个函数做协程设置：句柄协议（每程序一次），以及该函数调用会产生的帧类型及其协议方法。
 *
 * 对**迟到创建的实例**也要跑：泛型体里的调用会被推迟，实例的设置在函数体检查之后才发生。
 * 为那个实例跑一遍，`<name>$frame` 才会被定义，`value()` 也才落在代入后的 `T` 上。
 *
 * @param c  checker。
 * @param f  函数定义。
 */
static void coroSetup(Checker *c, FuncDef *f) {

        /* ---- The **handle** protocol, once per program ----
         * `coroutine<T>` is a storage type too (one surface type; the representation is chosen by
         * escape). Driving a handle is an ordinary method call on the marker type, with `T`
         * substituted by the existing instantiation machinery -- so `value()` returns each instance's
         * own `T`. Same shape as the frame's methods below; the difference is only that `self` is the
         * handle, which is why the two get different `coroProto` values and codegen dispatches on
         * `kind`. */
        StructDef *hsd = structOf(f->ret);
        if (hsd && hsd->methods.len == 0) {
            /* The protocol methods name the marker's **own** type parameters: substitution matches on
             * the parameter *name* (`ttSubstitute` compares `param`), so a synthetic `T` would stop
             * matching the moment the declaration is spelled `coroutine<A, B>`. `A` is what `send`
             * takes in, `B` is what `value` hands back; a one-argument `coroutine<T>` leaves `B`
             * unbound, and substitution falls back to `A` (types.c). */
            const char *an = hsd->typeParams.len > 0
                ? *(const char **)vecAt(&hsd->typeParams, 0) : "A";
            const char *bn = hsd->typeParams.len > 1
                ? *(const char **)vecAt(&hsd->typeParams, 1) : an;
            Type *tpA = arenaAllocZero(c->arena, sizeof *tpA);
            tpA->kind = TY_PARAM; tpA->name = an; tpA->param = an;
            vecInit(&tpA->targs, c->arena, sizeof(Type *));
            Type *tpB = arenaAllocZero(c->arena, sizeof *tpB);
            tpB->kind = TY_PARAM; tpB->name = bn; tpB->param = bn;
            vecInit(&tpB->targs, c->arena, sizeof(Type *));
            Type *hinst = arenaAllocZero(c->arena, sizeof *hinst);
            hinst->kind = TY_GENERIC;      /* an *instance* with targs: that is what `coroutine<A,B>` is */
            hinst->name = hsd->name;
            hinst->sdef = hsd;
            vecInit(&hinst->targs, c->arena, sizeof(Type *));
            *(Type **)vecPush(&hinst->targs) = tpA;
            *(Type **)vecPush(&hinst->targs) = tpB;
            const int hw[3] = { 3, 4, 6 };    /* next / value / send on the handle */
            for (int hi = 0; hi < 3; hi++) {
                const int which = hw[hi];
                FuncDef *pm = arenaAllocZero(c->arena, sizeof *pm);
                pm->name      = which == 3 ? "next" : which == 4 ? "value" : "send";
                pm->owner     = hsd;
                pm->modName   = f->modName;
                pm->line      = f->line;
                planSetCoroProto(pm, which);
                pm->ret       = which == 4 ? tpB : ttFromName(c->tt, "bool");
                vecInit(&pm->params, c->arena, sizeof(Param *));
                Param *self = arenaAllocZero(c->arena, sizeof *self);
                self->name = self->cname = "self";
                self->line = f->line;
                Type *rt = arenaAllocZero(c->arena, sizeof *rt);
                rt->kind = TY_REF;
                rt->inner = hinst;
                rt->mut = which == 3;      /* `next` advances the state, `value` only reads it */
                self->type = rt;
                *(Param **)vecPush(&pm->params) = self;
                if (which == 6) {
                    Param *vp = arenaAllocZero(c->arena, sizeof *vp);
                    vp->name = vp->cname = "v";
                    vp->type = tpA;            /* `send(v: A)`: what goes **into** the coroutine */
                    vp->line = f->line;
                    *(Param **)vecPush(&pm->params) = vp;
                }
                *(FuncDef **)vecPush(&hsd->methods) = pm;
            }
        }
        /* A call to a coroutine produces **that coroutine's own frame**, a concrete value type the
         * compiler synthesizes here -- before the body is checked, so call sites and the callee
         * agree. `coroutine<T>` stays what it always was: how a body declares "I am a coroutine and
         * I yield T". */
        StructDef *cfd = arenaAllocZero(c->arena, sizeof *cfd);
        /* A C name of its own: the frame is emitted through the type channel now, so it must not
         * collide with the function's own name. */
        const char *frameOwner = (planInstName(f) && planInstName(f)[0]) ? planInstName(f)
                                : (f->name ? f->name : "coro");
        cfd->name = arenaPrintf(c->arena, "%s$frame", frameOwner);
        planSetCoroOf(cfd, f);
        vecInit(&cfd->typeParams, c->arena, sizeof(const char *));
        vecInit(&cfd->fields, c->arena, sizeof(FieldDef *));
        vecInit(&cfd->methods, c->arena, sizeof(FuncDef *));
        Type *cft = arenaAllocZero(c->arena, sizeof *cft);
        cft->kind = TY_STRUCT;
        cft->name = cfd->name;
        cft->sdef = cfd;
        vecInit(&cft->targs, c->arena, sizeof(Type *));
        /* The protocol, as **real methods** on that type: `next(self: mut ref F) -> bool` and
         * `value(self: ref F) -> T`. Driving a coroutine is then ordinary method resolution -- no
         * special case in the checker -- and `fn drive<C>(c: mut ref C)` works at instantiation
         * through the existing deferred-method machinery. Codegen emits the two inline. */
        for (int which = 1; which <= 3; which++) {
            FuncDef *pm = arenaAllocZero(c->arena, sizeof *pm);
            pm->name     = which == 1 ? "next" : which == 2 ? "value" : "send";
            pm->owner    = cfd;
            pm->modName  = f->modName;
            pm->line     = f->line;
            planSetCoroProto(pm, which == 3 ? 5 : which);   /* 5 = `send` on the frame (the handle's is 6) */
            pm->ret      = which == 2 ? planYieldType(f) : ttFromName(c->tt, "bool");
            vecInit(&pm->params, c->arena, sizeof(Param *));
            Param *self = arenaAllocZero(c->arena, sizeof *self);
            self->name = self->cname = "self";
            self->line = f->line;
            Type *rt = arenaAllocZero(c->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = cft;
            rt->mut = which != 2;              /* `next`/`send` advance the state, `value` reads */
            self->type = rt;
            *(Param **)vecPush(&pm->params) = self;
            if (which == 3) {
                /* `send(self: mut ref F, v: T) -> bool`: hand a value to the next resume, which the
                 * coroutine reads through a `var x = yield e` binding (the frame's `in` slot). */
                Param *vp = arenaAllocZero(c->arena, sizeof *vp);
                vp->name = vp->cname = "v";
                /* `send(v: A)`: A is the marker's **first** type argument (`coroutine<A, B>`); the
                 * one-argument shorthand leaves A = B = the yield type, which is why a corpus file
                 * that only ever yielded keeps type-checking unchanged. */
                vp->type = f->ret->targs.len > 0
                    ? *(Type **)vecAt(&f->ret->targs, 0) : planYieldType(f);
                vp->line = f->line;
                *(Param **)vecPush(&pm->params) = vp;
            }
            *(FuncDef **)vecPush(&cfd->methods) = pm;
        }
        planSetCoroFrameType(f, cft);
        f->ret = cft;
    
}

/**
 * @brief 检查一个函数：签名、home arena 需求与函数体。
 *
 * 既分配又交回有用东西（引用、view、经出参的写入）的函数需要 home arena；只返回 `i32` 的不
 * 需要。逃逸集合必须在遍历函数体之前算出，因为遍历用它选择每个调用的 arena。`extern!` 只查
 * 形状：C 函数收标量与单指针，`slice<T>` 会变成两个 C 实参，struct 没有冻结布局。
 *
 * @param c  checker。
 * @param f  要检查的函数。
 */
static void checkFunc(Checker *c, FuncDef *f) {
    /* `@builtin` 声明只有签名、没有函数体（与 `extern!` 同款），而这条路后面会直接遍历
     * `f->body` ⇒ 无体声明一进来就段错误。
     * 内建的实现由编译器在**调用点**给出，所以这里什么都不用查；对它的调用由 check_expr 里的
     * "尚未实现" 守卫负责拒绝。只挡内建，不动 extern 的既有行为。 */
    if (f->isBuiltin && !f->body) return;
    /* A lambda's `call` method: its body was checked where the lambda was written, because that is
     * where the capture set comes from (check_expr.c, checkLambda). */
    if (anLamChecked(f)) return;

    /* The coroutine protocols (`next`/`value`/`send`) have no body: they are checked inline at
     * their call sites. Checking one here walked a null body. */
    if (planCoroProto(f)) return;
    /* More parameters than the effect summary has bits would silently lose the high ones. That is
     * exactly the failure mode seen before: the same store shape with 4 parameters was rejected while its
     * 40-parameter twin was accepted, because parameter 40's `addrMask` bit fell off the end of a
     * 32-bit mask. The mask is 64 bits now, and this is the contract on top of it: refusing loudly
     * beats ignoring a parameter. A function that needs more has to be split (an array-backed
     * summary is the deliberate later step if a real program ever asks for it). */
    if (f->params.len > EFF_MAX_PARAMS) {
        ckError(c, f->line,
                "The effect summary indexes parameters by position in a 64-bit mask, so the"
                " compiler refuses more than 64 of them instead of silently ignoring the rest"
                " (). Split the function.",
                "`%s` has %zu parameters; the compiler's limit is %d",
                f->name ? f->name : "this function", f->params.len, (int)EFF_MAX_PARAMS);
        return;
    }
    /* An extern declaration has no body, so only its signature is checked: the parameter
     * types were already resolved elsewhere, and no arena is involved, since the function
     * takes no arena parameter and needs no home arena (the arena the caller passes). Its
     * summary comes from the signed clause, or is the worst case (see `collectEffects`
     * below). */
    if (f->isExtern) {
        planSetMayUseArena(f, false);
        anSetNeedsHome(f, false);
        /* Only scalars and single pointers may cross the boundary. A `slice<T>` would
        * become two C arguments (data and length), so the names would not match, and a
        * struct has no frozen layout; the stdlib wrappers pass `s.data` and `s.len`
        * explicitly.
        *
        * The predicate itself lives in `ttCrossesC`: one spelling, shared with the `fn` type
        * (`ttResolve`), which is the other way a C function is reached. Spelling it out here
        * as well would be a second copy that can fall out of step with `ttCrossesC`. */
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (!ttCrossesC(p->type, false))
                ckError(c, p->line,
                        "A C argument is one machine word: a scalar, a pointer, or a function"
                        " pointer; a `slice<T>` becomes two C arguments (data + len), and a struct"
                        " crosses **by value** only when its layout is promised (`@frozen` on its"
                        " declaration). Otherwise wrap it in an extC function that passes `s.data` /"
                        " `s.len` explicitly.",
                        "`extern!` argument %zu has type `%s`, which cannot cross the C boundary",
                        i + 1, typeStr(c, p->type));
        }
        {
            /* `void` is a C return type like any other -- `exit`, `free`, `srand` and
             * `cfmakeraw` all have it, and refusing it left the privileged layer unable to
             * declare them. What has no C-level representation is a struct, a slice (two
             * arguments), or a view. */
            /* A missing return type is a different mistake from a type that cannot cross, so the
             * two have separate messages: a NULL return type is reported as missing rather than
             * printed as `void`. */
            if (!f->ret)
                ckError(c, f->line,
                        "A declaration has to say what it returns; `-> void` if it returns nothing.",
                        "`extern!` declaration `%s` has no return type", f->name);
            else if (!ttCrossesC(f->ret, true))
                ckError(c, f->line,
                        "A C return value is one machine word -- a scalar, a pointer, or a function"
                        " pointer -- or a `@frozen` struct, whose layout the declaration promises.",
                        "`extern!` return type `%s` cannot cross the C boundary",
                        typeStr(c, f->ret));
        }
        double tE = ctNow();
        collectEffects(c, f);
        ctPhase("cf-effects", tE);            /* summary = the signed clause, or the worst case */
        return;
    }
    /* `main(args)`: the entry point may take the command line, and the shape is fixed
     *: one parameter, `slice<slice<u8>>`. C hands the entry
     * point `argc` and `argv`, and codegen wraps those two into the view; any other
     * parameter has no C-level meaning, because C's `main` takes integers and pointers and
     * nothing else. Saying so here keeps the diagnostic in extC instead of letting the C
     * compiler complain about the wrapper.
     *
     * The return type is C's `int`, so it may be absent or `i32`; `-> i64` would be
     * truncated silently by the C compiler, which is exactly the kind of thing that must not
     * be silent. */
    bool isEntryFn = !f->owner &&
                     (f->isModuleEntry || strcmp(f->name, "main") == 0);
    if (isEntryFn) {
        /* 一个 `@module` 入口与 `main` 过同一道门：分发器把命令行原样交给它，所以返回值
         * 与参数形状的要求一模一样。消息里叫它用户写的那个名字，不叫 `main`。 */
        const char *what = f->isModuleEntry ? "an `@module` entry" : "`main`";
        Type *r = f->ret ? ttBase(f->ret) : NULL;
        bool retOk = !r || r->kind == TY_VOID ||
                     (r->kind == TY_BUILTIN && r->name && strcmp(r->name, "i32") == 0);
        if (!retOk)
            ckError(c, f->line,
                    "C's entry point returns an `int`, so an entry point may return `i32`"
                    " (or nothing at all). Any other type would be converted by the C compiler"
                    " without a word.",
                    "%s cannot return `%s`", what, typeStr(c, f->ret));
        if (f->params.len > 1)
            ckError(c, f->line,
                    "The command line is one view of the arguments. Wrap several pieces of"
                    " information in a struct if the entry point needs more.",
                    "%s takes at most one parameter, `slice<slice<u8>>`; %zu were declared",
                    what, f->params.len);
        else if (f->params.len == 1) {
            Param *p = *(Param **)vecAt(&f->params, 0);
            /* The shape is judged by shape and not through `ttBase`: resolving a view
             * lands on the struct behind it, which does not say `slice`. */
            Type *eo = ttIsViewType(p->type) ? *(Type **)vecAt(&p->type->targs, 0) : NULL;
            Type *ei = eo && ttIsViewType(eo) ? ttBase(*(Type **)vecAt(&eo->targs, 0)) : NULL;
            bool ok = ei && ei->kind == TY_BUILTIN && ei->name && strcmp(ei->name, "u8") == 0;
            if (!ok)
                ckError(c, p->line,
                        "The arguments arrive as bytes, the way the operating system hands them"
                        " over, so the entry point takes `slice<slice<u8>>`: the outer view is"
                        " `args`, and `args[0]` is the program name, again as C defines it.",
                        "%s's parameter must be `slice<slice<u8>>`, found `%s`",
                        what, typeStr(c, p->type));
        }
    }
    /* Every diagnostic inside this body has to name the file the body came from. The
     * loader records that context on each declaration (`FuncDef.ctx`), and this pass runs
     * every unit's functions in one go, so the context is switched here: without it, a
     * mistake inside a module came back pointing at a line of the **entry** file -- the
     * line was the module's, the file was not. */
    Ctx *savedCtx = c->ctx;
    if (f->ctx) c->ctx = f->ctx;

    FuncDef *savedFunc = c->curFunc;
    /* A function that allocates and hands back something useful (a reference or a view,
    * or a store through an out-parameter) needs a home arena. One that returns `i32`
    * does not: its allocations stay in its own block. */
    if (!f->isExtern && stmtHasNew(f->body) &&
        ((f->ret && typeContainsRef(c->tt, f->ret)) || stmtStoresThroughDeref(c, f->body, f)))
        anSetNeedsHome(f, true);

    Vec     *savedParams = c->curParams;

    /* A coroutine: the declared return type is `coroutine<T>` (a prototype in the prelude). The
     * declaration is the marker -- `yield` is legal only here, and the frame the checker lays out
     * for this function is a value. */
    /* `coroutine<T>`（简写：请求与应答同型）与 `coroutine<A, B>` 都算协程：前者一个类型参数，
     * 后者两个。协议方法各自取哪个参数见下面合成那段。 */
    planSetIsCoro(f, f->ret && (isProtoType(f->ret, "coroutine", 1)
                           || isProtoType(f->ret, "coroutine", 2)));
    planSetCoroRetProto(f, planIsCoro(f) ? f->ret : NULL);
    /* `coroutine<A, B>`：`B`（最后一个参数）是 yield 出来的类型。简写 `coroutine<T>` 只有一个
     * 参数，那个就是它。 */
    planSetYieldType(f, planIsCoro(f)
        ? *(Type **)vecAt(&f->ret->targs, f->ret->targs.len - 1) : NULL);
    /* What a coroutine yields is a **value**. Its frame is released when the task ends, so a
     * reference handed out from it can outlive the storage it points into -- the consumer has no
     * way to know the referent is gone. A pointer type is also not a spelling the per-type
     * helper names can be built from. Refusing the type here, at the declaration, reports the
     * mistake against the protocol instead of inside the generated C. */
    if (planIsCoro(f) && planYieldType(f) && typeContainsRef(c->tt, planYieldType(f)))
        ckError(c, f->line,
                "A coroutine hands out values. Its frame dies with the task, so a reference"
                " yielded from it can point at storage the consumer cannot tell is gone. Yield the"
                " value itself, or an index the consumer resolves against something that outlives"
                " the task.",
                "`%s` yields `%s`, which carries a reference",
                f->name, typeStr(c, planYieldType(f)));
    if (planIsCoro(f)) coroSetup(c, f);
    /* A parameter typed `coroutine<T>` is legal: the unified handle, not a
     * marker, is what the type stands for, so no check rejects such a parameter here. */

    c->curFunc = f;
    /* Instances share their template's body, so the scope of a body's results is the
     * template, not the instance: `wrap<i32>` and `wrap<i64>` write the same nodes. */
    resultsEnterOwner(planTemplate(f) ? planTemplate(f) : f);
    /* The publications of this body start here, and the level pass below folds exactly
     * this slice of `c->stores` -- see `Checker.storeBase`. Nothing recorded before this
     * point belongs to this body, so the pass does not even look at it; the owner field
     * (`StoreSite.fn`) is the second half of the same filter, for the records a nested
     * body appends inside this slice. */
    size_t savedStoreBase = c->storeBase;
    c->storeBase = c->stores.len;
    /* The arena of each call is chosen with the escape set, so it has to be computed
     * before the body is checked. */
    computeEscapes(c, f);
    c->curParams = funcTParams(f);
    /* While walking the body, record every site whose arena is decided only after the
     * analysis closes. */
    vecInit(&c->curArenaSites, c->arena, sizeof(Expr *));
    pushScope(c);
    /* Each function gets its own set of C names, so an `a` in one function cannot affect
     * an `a` in another; they end up in different C function bodies anyway. Checking a
     * generic instance walks the same body a second time, but in the same order, so the
     * generated names are identical and do not drift. */
    c->nameUses.len = 0;

    anSetParamSymCount(f, 0);                    /* a generic body may be walked more than once */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* A parameter is mutable: it is a local copy handed over by the caller, as in C,
         * so `ref real` is allowed inside `fn f(real: Board)`. */
        Sym *sym = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sym->cname;
        /* 引用型形参指向的存储是**调用方的**（借用）：`(*p).f = v` 的目的地要按可能被指物的
         * 最浅深度算，参数就是 0（活过本帧）。 */
        if (p->type && tsub(c, p->type)->kind == TY_REF) sym->minPointee = 0;
        /* Record the binding now, while the scope exists: the effect summary runs later, with no
         * scope at all, and needs the identity to attribute a store to the right parameter. */
        if (anParamSymCount(f) < EFF_MAX_PARAMS) {
            int at = anParamSymCount(f);
            anParamSyms(f)[at] = sym;
            anSetParamSymCount(f, at + 1);
        }
    }
    /* The body does not open a scope of its own: parameters and body locals share one.
     * Shadowing a parameter with `let a = ...` is therefore only a new name, which is
     * legal and becomes `a__2` in the generated C, while `var a = ...` declares a second
     * piece of storage and is rejected (see `declare`). */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

    /* The types of the declarations are final by now, so this is where a coroutine's frame can be
     * laid out -- and where rule 2 is enforced on it. */
    coroFrameLay(c, f);

    /* The types of the declarations are final by now, so this is the place to ask
     * whether every handle the function opened also gets closed. */
    checkOpenHandles(c, f);

    /* A parameter the body never reads. extC lets the program keep it and the generated C marks it
     * `unused` so the C compiler stays quiet, but the reader deserves to hear about dead weight in
     * a signature they wrote. `self` is exempt: a method that ignores its receiver is ordinary. */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->name || !p->cname || strcmp(p->name, "self") == 0) continue;
        if (stmtUsesCname(f->body, p->cname)) continue;
        ckWarn(c, p->line,
               "a parameter belongs to the signature, so it is kept and marked `unused` in the"
               " generated C",
               "parameter `%s` is never used", p->name);
    }

    collectEffects(c, f);      /* compute the effect summary; it is only recorded here */
    anSetArenaSites(f, c->curArenaSites);   /* handed to the pass that runs after the analysis closes */

    /* Reference depths, recomputed as a monotone data-flow fixed point over the body.
     *
     * The checking walk above maintains a binding's depth by destructive assignment and
     * has no join at control-flow merges, so at an `if` the arm visited last simply wins.
     * That cannot produce the upper bound the escape check needs, and it is the reason a
     * struct holding an allocation could come back pointing into a block arena. The walk
     * below derives the same numbers from the whole control-flow structure instead:
     * stores raise a depth, merges take the maximum, and loops are iterated to a fixed
     * point. It reads the tree and writes only the depths, so every later reader sees a
     * value that does not depend on the order branches were visited.
     *
     *3, item 3-a, and `src/check_dataflow.c`. */
    {
        DfResult dfr;
        dfAnalyze(c, f, &dfr);
        /* Publications recorded during this body settle the sites they reach. Running
         * here, after the depth fixed point, is the point of the whole split: the
         * decision sees the final depths instead of the numbers that happened to be
         * true while the body was being walked. */
        /* No `dfr`: the level solve reads its own table and runs before the write-back below (see
         * the note on `levelPass`). `EXTC_NO_LEVELPASS=1` skips it -- and, as `--help` now says,
         * that **changes the output**. */
        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, f);
        /* Ask the same question again with the numbers the passes settled on. Reporting
         * stays with the check; this is what makes the two answers comparable. */
        if (dbgOn("EXTC_DBG_DEFER")) recheckLevelRejections(c);
        if (dbgOn("EXTC_DBG_STORES")) {
            int n0 = 0;
            for (size_t i = 0; i < c->stores.len; i++) {
                StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
                fprintf(stderr, "[store] %s: line=%d at=%d kind=%d dst=%s\n",
                        f->name ? f->name : "?", st->line, st->at, (int)st->value->kind,
                        (st->target && st->target->kind == EX_IDENT) ? st->target->u.ident.name
                                                                     : (st->target ? "?" : "none"));
                n0++;
            }
            (void)n0;
        }
        if (dbgOn("EXTC_DBG_DFA")) {
            fprintf(stderr, "[dfa] %s: %d vars%s\n", f->name ? f->name : "?",
                    dfr.nvars, dfr.overflow ? " (OVERFLOW: result unused)" : "");
            for (int i = 0; i < dfr.nvars; i++) {
                fprintf(stderr, "       %-6s depth=%d", dfr.vars[i].cname, dfr.vars[i].depth);
                for (int k = 0; k < dfr.vars[i].nfields; k++)
                    fprintf(stderr, "  .%s=%d", dfr.vars[i].fields[k].name,
                            dfr.vars[i].fields[k].depth);
                fprintf(stderr, "\n");
            }
        }
        /* When the DFA gives up (`dfr.overflow`: a fifth ref-carrying field of one binding, or its
         * round cap) its **whole result is dropped** and the depths stay whatever the checking walk
         * left behind -- the walk this file calls unsound at control-flow merges. So the fallback is
         * real and it is weaker; what it is *not* is rare: `stdlib/stl/string.extc`'s own
         * `string::create` trips it, so a per-function warning here would be ~100% noise and would
         * break the "no false positives on the positive corpus" rule that `tests/warnings` enforces.
         * It is therefore documented rather than announced; `EXTC_DBG_DFA=1` still prints
         * "(OVERFLOW: result unused)" next to the body it happened in. */
        if (!dfr.overflow) {
            symIdxSync(c);
            if (g_sbBad) {
                for (size_t i = 0; i < c->allSyms.len; i++) {      /* exactly the old loop */
                    Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
                    if (!sy || sy->line < 0) continue;
                    int d = dfLookup(&dfr, sy->cname);
                    if (d > anRefDepth(sy)) anSetRefDepth(sy, d);
                }
            } else {
                for (int i = 0; i < dfr.nvars; i++) {
                    const char *nm = dfr.vars[i].cname;
                    int d = dfr.vars[i].depth;
                    if (!nm || d < 0) continue;
                    SymBucket *b = symBucket(nm, 0);
                    if (!b) continue;
                    for (size_t k = 0; k < b->syms.len; k++) {
                        Sym *sy = *(Sym **)vecAt(&b->syms, k);
                        if (sy && sy->line >= 0 && d > anRefDepth(sy)) anSetRefDepth(sy, d);
                    }
                }
            }
        }
        /* 目的地深度现在定了：DFA 已写回，`Sym.minPointee` 也在 walk 里收全了所有改指。
         * 复判 walk 里推迟的 `(*cur).f = v` —— 词序不再影响答案（作用域还开着，`lookup` 能用）。 */
        recheckPendingRefStores(c, !dfr.overflow);
    }
    popScope(c);
    c->curFunc = savedFunc;
    resultsEnterOwner(savedFunc);
    c->curParams = savedParams;
    c->ctx = savedCtx;
    c->storeBase = savedStoreBase;
}

/**
 * @brief 判断初始化表达式是否为常量表达式。
 *
 * 没有常量折叠器，所以看形状：字面量、取负的字面量、枚举常量、以及它们之间的算术；调用一律
 * 拒绝。
 *
 * @param e  初始化表达式；可为 NULL。
 * @return   是常量表达式时 true。
 */
bool isConstInit(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: return true;
    case EX_ENUMVAL: return true;                       /* `status.ok` is a constant */
    case EX_BIN:                                         /* `15 * 15`: C folds this */
        return isConstInit(e->u.bin.left) && isConstInit(e->u.bin.right);
    case EX_UN:                                          /* `-1` */
        return e->u.un.operand && isConstInit(e->u.un.operand);
    /* A struct literal whose every field is a constant is one too: nothing in it has
     * to run, and C has always spelled exactly this as a static initializer
     * (`static pt ORIGIN = { .x = 0, .y = 0 };`). An omitted field is the zero value,
     * which is a constant as well.
     *
     * Leaving it out made the one thing a library cannot do otherwise impossible:
     * `let cout: out = out { fd: 1 }` -- the stream object at the top of `std::io`.
     * The rule it was caught by exists for a real reason (a global is a C static
     * object, so its initializer has to be one C can fold), and a literal of
     * constants satisfies that reason; a call does not, and is still rejected. */
    case EX_STRUCTLIT: {
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
            if (fi && fi->value && !isConstInit(fi->value)) return false;
        }
        return true;
    }
    /* A function's **address** is a constant: the linker knows it before the program runs, and C
     * spells it `&f` in a static initializer without any code running. It has to be here for the
     * C-ABI line's table: a table of `fn` fields cannot be zero-initialized
     * (a `fn` has no zero value, which is the whole point), so it must be initialized where it is
     * declared -- and a table belongs at the top level, because the host hands it to a module that
     * may keep it. Without this the only way to build a table was to assign the slots in a function,
     * which put the storage in a frame: exactly the lifetime a table must not have. */
    case EX_IDENT: return planCallee(e) != NULL;   /* the checker set it: this name is a function */
    /* `null` is C's `(void *)0`: nothing runs to produce it. It matters for the same reason the
     * line above does -- a table has a `ctx: ?ref void` field, and the slot beside it holds a
     * function address. */
    case EX_NULL:  return true;
    /* A **lossless** conversion of a constant is one: `i64(3)` is `(int64_t)3` in C, which folds,
     * and a table's `size: i64(3)` field is written exactly that way. A conversion the checker marked
     * as needing a runtime check is **not** a constant -- it comes out as a call to a helper
     * (`((uint64_t)extc_narrowU((uint64_t)(4096), ...))`, as for `let PAGE: u64 = u64(4096)`),
     * and gcc refuses that in a static initializer. Saying so here means the user gets "must be
     * initialized with a constant" instead of a gcc error in the product. */
    case EX_CONV: return !planConvCheck(e) && isConstInit(e->u.conv.operand);
    default: return false;
    }
}
/**
 * @brief 检查顶层的 `let` 与 `var` 声明。
 *
 * 全局量是深度 0，活过每一帧；由此不需要任何专门规则就有两个后果：逃逸规则已经拒绝把局部量
 * 存进全局量，固定大小的全局量是 C 静态对象、不需要 arena。初始化式必须是常量，因为 C 的静态
 * 初始化式只能含常量表达式。
 *
 * @param c  checker。
 */
static void checkGlobals(Checker *c) {
    Module *m = c->m;
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&m->globals, i);

        /* A global may not share a name with another global or with a function. */
        for (size_t j = 0; j < c->globals.len; j++)
            if (strcmp((*(Sym **)vecAt(&c->globals, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a global", g->name);
        for (size_t j = 0; j < m->funcs.len; j++)
            if (strcmp((*(FuncDef **)vecAt(&m->funcs, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a function", g->name);

        if (g->ann) g->ann = ttResolve(c->tt, c->ctx, g->ann, g->line, NULL);
        if (ttIsError(g->ann)) g->ann = NULL;

        if (!g->init) {
            /* Zero initialization: C clears static storage duration on its own, which is
             * what an uninitialized global means. A type containing a reference has no zero
             * value, though, and a global is depth 0, so there is nothing to borrow. */
            if (g->ann && typeContainsRef(c->tt, g->ann))
                ckError(c, g->line,
                        "a global is depth 0, so a reference inside it has nothing to "
                        "borrow from without an initializer.",
                        "cannot zero-initialize the global `%s`: it contains a reference",
                        g->name);
            if (!g->ann) g->ann = ttError(c->tt);
        } else {
            /* `noHoist` is deliberately not set for a global: its initializer is already
             * governed by the more fundamental rule that it must be a constant (C static
             * initialization; a global is a C static object). Reporting that rule is much
             * clearer than letting `??` report that there is nowhere to put a temporary.
             * The `get()` in `get() ?? -1` is not a constant anyway, so `??` is
             * irrelevant. */
            Type *it = checkValue(c, g->init);
            Type *declT = g->ann ? g->ann : it;
            if (g->ann) checkAssignable(c, g->ann, it, g->init, "initializer");

            /* Must run after the type check: `color.empty` is only rewritten into an enum
             * constant node (EX_ENUMVAL) by then, and before that it is still an
             * EX_FIELD. */
            if (!isConstInit(g->init)) {
                ckError(c, g->line,
                        "A global exists before any function runs, so its initializer has to be "
                        "a constant (a literal, an enum constant, or arithmetic on those). "
                        "Anything computed belongs inside a function.",
                        "the global `%s` must be initialized with a constant", g->name);
                continue;
            }

            /* Depth 0, so the escape rule applies on its own: storing something local into
             * a global is rejected. */
            checkEscape(c, g->init, 0, g->line, "this global");

            g->ann = ttIsError(declT) ? ttError(c->tt) : declT;
        }

        Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
        s->name  = g->name;
        s->type  = g->ann;
        s->mut   = g->mut;
        s->depth = 0;                     /* a global is depth 0 */
        s->line  = g->line;
        s->modName = g->modName;          /* the module that declared it, for qualified names */
        *(Sym **)vecPush(&c->globals) = s;
    }
}

/* Create, or find, the concrete instance of a generic function.
 *
 * Why:
 *   An instance has to be born at the call site: a type instance such as
 *   `varArray<i32>` is decided by the type, while the type arguments of a free function
 *   are known only where it is called.
 *
 * Params:
 *   c     - checker, used for the arena and for substitution
 *   tmpl  - the template function
 *   targs - one type argument per template parameter
 *   line  - source line of the instantiation; currently unused
 *
 * Returns:
 *   One instance per template and set of type arguments: a match is reused, otherwise a
 *   new instance is allocated. NULL when `tmpl` is NULL.
 *
 * Notes:
 *   - The instance is a FuncDef of its own: `tmpl` points back at the template, `targs`
 *     and `instName` are filled in, and the parameter and return types are substituted,
 *     so codegen emits it like any other function. Entering it still has to open the
 *     substitution of the template's type parameters.
 *   - The instance is appended to `funcInsts` for reuse, and to the module's function
 *     list for codegen.
 */
#define FUNC_INST_PREFIX "__extc_fi_"

/* How many concrete instances of free generic functions one compilation may build.
 *
 * A template can be instantiated inside its own body with a bigger type -- `grow<T>(x)` calling
 * `grow(box<T>)` -- and nothing stopped it: the compiler kept creating instances without
 * bound. This is a limit of the compiler, not of the language, so the message says so. */
#define FUNC_INST_LIMIT 4096

/* How deeply a **created** type may nest. The runaway in E3 does not just add instances: each
 * step instantiates with a type one level deeper (`box<T>` again), so by step N the type is N
 * levels deep and every copy of it costs O(N) -- the total is O(N^2) and the compiler was
 * OOM-killed after 54 seconds. Depth is the quantity that has to be bounded; the instance count
 * above is only a backstop. 64 is far above anything a source file can write by hand
 * (the attack suite's A4 uses 10) and far below anything that can exhaust memory. */
#define TYPE_DEPTH_LIMIT 64

/**
 * @brief 数类型实例化参数的嵌套深度（普通类型为 0）。
 *
 * 递归防御性截断：它跑在编译器自己造出来的类型上。
 *
 * @param t  类型。
 * @return   嵌套深度。
 */
static int typeDepth(Type *t) {
    if (!t) return 0;
    int best = 0;
    for (size_t i = 0; i < t->targs.len; i++) {
        int d = typeDepth(*(Type **)vecAt(&t->targs, i));
        if (d > best) best = d;
    }
    return best + (t->targs.len ? 1 : 0);
}

/**
 * @brief 创建或找到自由泛型函数的具体实例。
 *
 * 类型实例（`varArray<i32>`）由类型决定，而自由函数的类型实参是从调用点读出来的，所以实例
 * 只能在那里创建。同一个 `targs` 列表总是映射到同一个实例。
 *
 * @param c      checker；实例追加进 `c->funcInsts` 与模块。
 * @param tmpl   泛型模板；NULL 得到 NULL。
 * @param targs  具体类型实参。
 * @param line   调用的源码行；未使用，保持调用形状。
 * @return       独立的 `FuncDef`：`tmpl` 指回模板，`targs` 与 `instName` 填好，参数与返回
 *               类型已代入；codegen 像普通函数一样发出它。
 */
FuncDef *funcInstance(Checker *c, FuncDef *tmpl, Vec *targs, int line) {
    if (!tmpl) return NULL;
    for (size_t j = 0; targs && j < targs->len; j++) {
        int d = typeDepth(*(Type **)vecAt(targs, j));
        if (d > TYPE_DEPTH_LIMIT) {
            ctxError(c->ctx, line, 1,
                     "This is a limit of the compiler, not something wrong with the program;"
                     " please report it together with the program that triggered it.",
                     "internal: a generic instance was requested with a type nested %d levels"
                     " deep (a template instantiated inside its own body with a growing type?)", d);
            return NULL;
        }
    }
    if (c->funcInsts.len >= FUNC_INST_LIMIT) {
        ctxError(c->ctx, line, 1,
                 "This is a limit of the compiler, not something wrong with the program;"
                 " please report it together with the program that triggered it.",
                 "internal: more than %d generic function instances were created"
                 " (a template instantiated inside its own body with a growing type?)",
                 FUNC_INST_LIMIT);
        return NULL;
    }
    /* Already built? One template plus one set of type arguments is one instance, so a
     * match is returned instead of allocating a second one. */
    for (size_t i = 0; i < c->funcInsts.len; i++) {
        FuncDef *in = *(FuncDef **)vecAt(&c->funcInsts, i);
        if (planTemplate(in) != tmpl || in->targs.len != targs->len) continue;
        bool same = true;
        for (size_t j = 0; j < targs->len; j++)
            if (!ttEquals(*(Type **)vecAt(&in->targs, j), *(Type **)vecAt(targs, j))) { same = false; break; }
        if (same) return in;
    }
    FuncDef *in = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    *in = *tmpl;                        /* shallow copy: shares the body (as method instances do) */
    planSetTemplate(in, tmpl);          /* the back pointer lives in the plan, not on the node */
    planSetUsed(in, false);
    vecInit(&in->targs, c->arena, sizeof(void *));
    for (size_t j = 0; j < targs->len; j++)
        *(Type **)vecPush(&in->targs) = *(Type **)vecAt(targs, j);
    /* Substitute the parameter and return types.
     *
     * Each `Param` must be copied first: the pointers were shared with the template, so
     * substituting in place changed the template too, and the second inference saw a
     * template that already held concrete types. That silent corruption showed up as a
     * segfault in testing. */
    {
        Vec newParams;
        vecInit(&newParams, c->arena, sizeof(void *));
        for (size_t j = 0; j < tmpl->params.len; j++) {
            Param *src = *(Param **)vecAt(&tmpl->params, j);
            Param *copy = (Param *)arenaAllocZero(c->arena, sizeof(Param));
            *copy = *src;
            copy->type = ttSubstitute(c->tt, src->type, &tmpl->typeParams, targs);
            *(Param **)vecPush(&newParams) = copy;
        }
        in->params = newParams;
    }
    if (in->ret) in->ret = ttSubstitute(c->tt, in->ret, &tmpl->typeParams, targs);
    if (planCoroRetProto(tmpl)) {
        in->ret = ttSubstitute(c->tt, planCoroRetProto(tmpl), &tmpl->typeParams, targs);
        planSetCoroFrameType(in, NULL);
        planSetIsCoro(in, false);
    }
    /* C name: `max_i32`, built by mangling the type arguments onto the name. */
    Buf b;
    bufInit(&b, c->arena);
    bufPuts(&b, tmpl->name);
    for (size_t j = 0; j < targs->len; j++) {
        bufPutc(&b, '_');
        bufPuts(&b, ttMangle(c->tt, *(Type **)vecAt(targs, j)));
    }
    planSetInstName(in, bufCstr(&b));
    in->typeParams.len = 0;             /* an instance has no type parameters left */
    *(FuncDef **)vecPush(&c->funcInsts) = in;
    *(FuncDef **)vecPush(&c->m->funcs) = in;   /* codegen emits the definition from this list */
    (void)line;
    return in;
}

/**
 * @brief 从实参类型反推并填入类型参数。
 *
 * 对 `f<T>(x)` 这样的调用，从实参类型推断 `T`，就是把 `slice<T>` 与 `slice<u8>` 匹配起来。
 *
 * @param tt     类型表。
 * @param tp     类型参数名，顺序与 `targs` 一致。
 * @param targs  每个类型参数一格，推断过程中填入；已填的格必须与新类型一致。
 * @param want   参数类型，可以提到类型参数。
 * @param got    实参的类型。
 * @return       推断不出时为 false（例如 `T` 只出现在返回类型里），此时调用必须写明 `f<i32>`。
 */
bool unifyTParams(TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got) {
    if (!want || !got) return true;
    if (want->kind == TY_PARAM) {
        for (size_t i = 0; i < tp->len; i++) {
            if (strcmp(*(const char **)vecAt(tp, i), want->param) != 0) continue;
            Type *cur = *(Type **)vecAt(targs, i);
            if (cur) return ttEquals(cur, got);
            *(Type **)vecAt(targs, i) = got;
            return true;
        }
        return true;
    }
    if (want->kind == TY_REF && got->kind == TY_REF)
        return unifyTParams(tt, tp, targs, want->inner, got->inner);
    if (want->kind == TY_GENERIC && got->kind == TY_GENERIC) {
        if (want->sdef != got->sdef || want->targs.len != got->targs.len) return false;
        for (size_t i = 0; i < want->targs.len; i++)
            if (!unifyTParams(tt, tp, targs, *(Type **)vecAt(&want->targs, i),
                              *(Type **)vecAt(&got->targs, i))) return false;
        return true;
    }
    if (ttHasParam(want)) return false;   /* mentions T but the shapes disagree => cannot infer */
    return true;
}

/**
 * @brief 为一个具体实例重查一条被推迟的运算符使用。
 *
 * 推迟到实例化的两批检查（本运算符批与引用规则批）对类型实例和自由函数实例走同一条路，所以
 * 共用辅助函数，而不是为 `fn f<T>` 再写一份。
 *
 * @param c         checker。
 * @param ec        记录下来的检查，其操作数类型仍提到类型参数。
 * @param params    实例的类型参数。
 * @param targs     实例的类型实参，与 `params` 位置对应。
 * @param instName  诊断里用的实例名。
 *
 * 调用方必须先把 `c.substParams` / `c.substArgs` 设为该实例（`runRefCheck` 经 `tsub` 读它们）。
 * 诊断报的是记录下来的运算符，不是固定的 `==`。
 */
static void runOpCheck(Checker *c, OpCheck *ec, Vec *params, Vec *targs, const char *instName) {
    TypeTable *tt = c->tt;
    Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type, params, targs);
    Type *rt2 = ttSubstitute(tt, ec->node->u.bin.right->type, params, targs);
    /* A `T op T` in the template is an instance's `A op B` here. The equality of the two is
     * **not** this function's business: `typeSupportsOp` below is the authority on "can this
     * type be compared with that one" -- it accepts native numerics, judges an array by its
     * element type **and the same array type on the other side**, and finds an operator method
     * for structs. Asking "is the left operand a struct?" first skips all of that for
     * every other type, so a `[1]i32 == i32` slips through and the emitted descriptor comparison
     * reads 8 bytes out of a 4-byte operand (ASan: stack-buffer-overflow in `extc_eq`, found by
     * tools/fuzz.py in a mutation of examples/generic-free-fn.extc). Let the authority decide. */
    if (!typeSupportsOp(tt, lt, ec->op, rt2)) {
        const char *op = ec->op;
        Type *b = ttBase(lt);
        /* An instance the concrete-type path rejects for a better reason than "define a
         * method", so this does not tell the reader to go and write one:
         *   - `%` on a float instance: `%` is for integers;
         *   - an ordering operator on `bool` or a payload-free enum: ordering is for
         *     numbers (the concrete path reports the same thing).
         * Both messages mirror check_expr.c, which is where the concrete rules live. */
        if ((strcmp(op, "%") == 0 && ttIsNumeric(lt)) || (!isEqualityOp(op) && cmpIsNative(lt))) {
            ckError(c, ec->node->line, NULL,
                    "cannot apply `%s` to `%s`", op, typeStr(c, lt));
            return;
        }
        /* An enum with a payload has no `==` in C, and no method could fix that; the
         * concrete path points at `match` and so does this. */
        if (b && b->kind == TY_ENUM && enumHasPayload(b->edef) && isEqualityOp(op)) {
            ckError(c, ec->node->line,
                    "an enum with a payload is a tagged union -- compare it with `match`,"
                    " or write a method that does",
                    "`%s` is an enum with a payload, so it has no `%s`", typeStr(c, lt), op);
            return;
        }
        Buf note;
        bufInit(&note, c->arena);
        bufPrintf(&note,
                  "`%s` inside a generic is checked at instantiation, not on the template --"
                  " the price of having no traits. Add a `fn %s` to that type.", op, op);
        ckError(c, ec->node->line, bufCstr(&note),
                "`%s` needs `%s` to define `%s`", instName, typeStr(c, lt), op);
        return;
    }

    /* The instance answers with a method, and a method of an instance is emitted by code
     * generation only when it is marked used. This is the one place that knows it: the
     * template could not, `T` was opaque there, and the call site cannot, because code
     * generation resolves the operator again on the substituted type and finds the same
     * method without recording anything.
     *
     * Not marking it would let `T == T` instantiated at a slice call the prelude's
     * `slice<T>::==` without anybody emitting it, so the generated C would hold a call to a
     * function that does not exist. The node itself is not touched on purpose: the template
     * has one node and many instances, so a `func` stored here would be the wrong method
     * for every other instance. */
    FuncDef *m = findOp(tt, ttBase(lt), ec->op, rt2,
                        strcmp(ec->op, "!=") == 0 ? "==" : NULL);
    if (m) planSetUsed(m, true);
}

/**
 * @brief 复核一条延迟调用结果的 USE 记录。
 *
 * 模板当时接受它，是因为值的类型就是错误类型；这里接收者类型已经具体，方法真正的返回类型
 * 可知，记录在使用点上的期望类型终于能落实。`instName` 是实例名，诊断因此读作
 * “`f_i32` expects `i8`, found `i64`”——在实例化时做、而不是在模板上做，意义就在这里。
 *
 * @param c         检查器。
 * @param du        记录下来的使用点。
 * @param params    实例的类型参数名。
 * @param targs     实例的类型实参。
 * @param instName  实例名，进诊断。
 */
static void runDeferredUse(Checker *c, DeferredUse *du, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    /* 不是延迟方法的节点自带类型：把表达式类型代换后比较。类型参数类型的值走的就是这条路
     * （`fn run<T>` 里的 `s = s + c.value()` 记下的是和，它在模板上的类型是 `T`）。这种节点上
     * 读 `u.method.recv` 会读错：那个 union 成员不是 `EX_METHOD`。 */
    if (du->call->kind != EX_METHOD || planCallee(du->call)) {
        Type *vt = ttSubstitute(tt, du->call->type, params, targs);
        /* **Both** sides: `want` is very often the template's own parameter too (an argument checked
         * against `gen<T>`'s parameter, say), and substituting only the value reported a bogus
         * `` `gen_i64` expects `T`, found `i64` `` for every generic call in the corpus. */
        Type *wt = ttSubstitute(tt, du->want, params, targs);
        if (ttIsError(vt) || ttIsError(wt)) return;
        checkAssignable(c, wt, vt, du->call, arenaPrintf(c->arena, "`%s`", instName));
        return;
    }
    Type *rt = ttSubstitute(tt, du->call->u.method.recv->type, params, targs);
    FuncDef *f = findMethod(ttBase(rt), du->call->u.method.name);
    /* A missing method is reported by runMethodCheck; nothing to add here. */
    if (!f || !f->ret) return;
    Type *ret = f->ret;
    if (rt && rt->kind == TY_GENERIC && f->owner == rt->sdef)
        ret = ttSubstitute(tt, f->ret, &f->owner->typeParams, &rt->targs);
    if (ttIsError(ret)) return;
    checkAssignable(c, du->want, ret, du->call, arenaPrintf(c->arena, "`%s`", instName));
}

/**
 * @brief 为一个具体实例重新检查一条延迟的方法调用。
 *
 * 模板上接收者类型是类型参数，找不到方法，于是把这次调用记下来；这里接收者类型已经具体：
 * 方法存在就标成已用，好让代码生成发出它；不存在就是这次延迟存在的意义——报出该实例
 * 不支持这次调用。
 *
 * 节点**故意不记 `func`**：一个模板体有多个实例，记下的 `func` 对其它实例都是错的方法；
 * 代码生成会在代换后的接收者类型上重新解析，和解析运算符一样。实参个数在这里查，因为
 * 别处看不见它：模板没有签名可比，放过去只会在生成的 C 里炸开。
 *
 * @param c         检查器。
 * @param mc        延迟的调用：节点、方法名、实参个数。
 * @param params    实例的类型参数名。
 * @param targs     实例的类型实参。
 * @param instName  实例名，进诊断。
 */
static void runMethodCheck(Checker *c, MethodCheck *mc, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    Type *rt = ttSubstitute(tt, mc->node->u.method.recv->type, params, targs);
    Type *rb = ttBase(rt);
    FuncDef *f = findMethod(rb, mc->name);
    if (!f) {
        Buf note;
        bufInit(&note, c->arena);
        bufPrintf(&note,
                  "`%s` inside a generic is checked at instantiation, not on the template --"
                  " the price of having no traits. Add a `fn %s` to that type.",
                  mc->name, mc->name);
        ckError(c, mc->node->line, bufCstr(&note),
                "`%s` needs `%s` to define `%s`", instName, typeStr(c, rb), mc->name);
        return;
    }
    size_t want = f->params.len > 0 ? f->params.len - 1 : 0;     /* minus the receiver */
    if (mc->nargs != want) {
        ckError(c, mc->node->line, NULL,
                "`%s` takes %zu argument%s, but %zu %s written",
                mc->name, want, want == 1 ? "" : "s", mc->nargs, mc->nargs == 1 ? "was" : "were");
        return;
    }
    planSetUsed(f, true);
}

/**
 * @brief 判断一条被推迟的引用检查是否属于这个自由函数实例。
 *
 * @param rc  从模板体推迟下来的检查。
 * @param fi  正在重放的实例。
 * @return    该检查来自本实例自己的模板、且模板是泛型时 true；方法走类型实例那条路，返回
 *            false。
 */
static bool refCheckApplies(RefCheck *rc, FuncDef *fi) {
    FuncDef *tmpl = planTemplate(fi);
    if (!fi || !tmpl) return false;
    if (rc->func != tmpl) return false;
    return tmpl->typeParams.len > 0;
}

/**
 * @brief 为一个具体实例重查一条被推迟的引用规则。
 *
 * 泛型体在模板上只检查一次，那时 `T` 不透明，所以结果依赖 `T` 的规则都推迟到这里按实例重放。
 *
 * @param c         checker。
 * @param rc        推迟的检查：值、它的深度、它必须装进多深、以及引用规则作用于什么。
 * @param instName  实例名，用于诊断。
 *
 * 模板期记录的深度是电平求解器之后会改变的世界快照，所以对**分配站点决定深度**的形状在这里
 * 重算，且只往下调；重算绑定不允许——它的站点还没定，答 0 会接受真正的悬空指针。
 */
static void runRefCheck(Checker *c, RefCheck *rc, const char *instName) {
    TypeTable *tt = c->tt;
        /* Does that `T` carry a reference in this instance? If not, this rule does not
         * apply here at all: `box<i64>::set` stays legal while `boxT<slice<u8>>::stash`
         * is rejected. That is exactly why `T` cannot simply be treated as
         * reference-carrying in every instance. */
        if (rc->isNewSize) {
            /* Still a type parameter (or `void`) after instantiation => the size is
             * still unknown => reject, naming the instance. */
            Type *nt = tsub(c, rc->declType);
            c->substParams = NULL;
            c->substArgs   = NULL;
            if (nt->kind == TY_PARAM || ttHasParam(nt) || ttIs(nt, "void"))
                ckError(c, rc->line,
                        "`new` needs a concrete type: its size is decided at compile time,"
                        " and this one is still not concrete after instantiation.",
                        "in instance `%s`: cannot `new` `%s` -- its size is not known"
                        " even here", instName, typeStr(c, nt));
            return;
        }

        if (rc->isZero) {
            /* Zero-value kind: does this instance's `T` have a zero value? */
            Type *zt = tsub(c, rc->declType);
            c->substParams = NULL;
            c->substArgs   = NULL;
            if (typeLacksZeroValue(tt, zt))
                ckError(c, rc->line,
                        "A generic body is checked once on the template, where `T` is"
                        " opaque -- so this is re-checked for every concrete instance."
                        " Give the local an initializer.",
                        "in instance `%s`: `%s` has no zero value (it contains a"
                        " reference)", instName, rc->what);
            return;
        }

        /* Same reasoning: a function that is never called needs no per-instance recheck. */
        if (rc->func && !planUsed(rc->func)) return;
        Type *vt = tsub(c, rc->val->type);
        c->substParams = NULL;
        c->substArgs   = NULL;
        if (!typeContainsRef(tt, vt)) return;

        /* Recompute with the level the solver settled on: take the smaller value,
         * it can only get tighter. */
        if (depthComesFromAlloc2(c, rc->val, 0)) {
            int now = solvedDepth(rc->val);
            if (now < rc->depth) rc->depth = now;
        }
        if (dbgOn("EXTC_DBG_AT"))
            fprintf(stderr, "[at] %s what=%s depth=%d at=%d kind=%d dca=%d svd=%d\n",
                    instName, rc->what, rc->depth, rc->at, (int)rc->val->kind,
                    depthComesFromAlloc2(c, rc->val, 0)?1:0, solvedDepth(rc->val));
        if (rc->depth > rc->at) {
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: %s would hold a reference to something that"
                    " dies first (depth %d, but this can only hold up to %d)",
                    instName, rc->target ? "this assignment" : rc->what,
                    rc->depth, rc->at);
        } else if (rc->target && rc->at == 0 && anBorrowed(rc)
                   && !valTracesToParam(c, rc->func, rc->val)) {   /* not parameter-backed */
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: cannot store a borrowed value into something"
                    " that outlives this call", instName);
        }
}

/**
 * @brief 把函数体里作为**局部声明类型**出现的类型实例 intern 一遍。
 *
 * 只需语句遍历：出问题的形状是泛型函数里的 `var p: pair<T>`，而 interning 是代入本身的副
 * 作用。
 *
 * @param c  checker。
 * @param f  函数定义。
 */
static void internLocalTypes(Checker *c, FuncDef *f) {
    if (!f || !f->body) return;
    Vec *params = f->typeParams.len ? &f->typeParams
                                   : (planTemplate(f) ? &planTemplate(f)->typeParams : NULL);
    Vec *targs  = f->targs.len ? &f->targs : NULL;
    if (!params || !targs) return;
    Vec stack;
    vecInit(&stack, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&stack) = f->body;
    while (stack.len) {
        Stmt *st = *(Stmt **)vecAt(&stack, stack.len - 1);
        stack.len--;
        if (!st) continue;
        /* The checker writes its results back into the AST (`Stmt.type`, `Expr.type`, ...), so a
         * statement's own type is often where an instance hides -- `let sl = arr[..]` has type
         * `slice<box<T>>` and nothing else in the body mentions that slice. Substituting it here
         * interns it (`ttGeneric`), early enough for the unit list: without this the generated C
         * named an instance nobody emitted (`'slice_box_i64' undeclared`). */
        if (st->type) (void)ttSubstitute(c->tt, st->type, params, targs);
        switch (st->kind) {
        case ST_VAR:
            if (st->u.var.ann) (void)ttSubstitute(c->tt, st->u.var.ann, params, targs);
            break;
        case ST_BLOCK:
            for (size_t i = 0; i < st->u.block.stmts.len; i++)
                *(Stmt **)vecPush(&stack) = *(Stmt **)vecAt(&st->u.block.stmts, i);
            break;
        case ST_IF:
            *(Stmt **)vecPush(&stack) = st->u.ifs.thenBody;
            *(Stmt **)vecPush(&stack) = st->u.ifs.elseBody;
            break;
        case ST_WHILE:
            *(Stmt **)vecPush(&stack) = st->u.whiles.body;
            break;
        case ST_MATCH:
            for (size_t i = 0; i < st->u.match.arms.len; i++)
                *(Stmt **)vecPush(&stack) = (*(MatchArm **)vecAt(&st->u.match.arms, i))->body;
            break;
        default:
            break;
        }
    }
}

/**
 * @brief 判断一个实例是**临时占位**还是真实实例。
 *
 * 泛型体里的泛型调用必须造一个占位，外层模板体才能继续类型检查（`T` 在那里不透明）。实例化
 * 时每个真实调用点都会重指到具体实例，占位是 checker 自己拥有的东西，codegen 从不发出它；
 * 被推迟的重查也要跳过它——向占位问“这个类型有 `<` 吗”正是模板上答不了的那个问题，由具体
 * 实例来答。
 *
 * @param targs  类型实参。
 * @return       任一实参仍提到类型参数时 true。
 */
static bool provisionalInstance(Vec *targs) {
    for (size_t i = 0; i < targs->len; i++)
        if (ttHasParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/**
 * @brief 在外层实例的代换下，把一条延迟调用点重新解析成具体实例。
 *
 * 三步：代换类型实参、把那个具体实例驻留（或创建）出来、把调用表达式的 `func` 指向它
 * （`cc->node->func`）。实例是驻留的（`funcInstance` 去重），所以一种实参组合只有一个实例。
 *
 * @param c             检查器。
 * @param cc            记录下来的调用点，其类型实参里还带着类型参数。
 * @param enclosing     外层实例的函数定义。
 * @param enclosingInst 外层实例的类型；作为代替解析结果回落到指针。
 * @param params        外层实例的类型参数名。
 * @param targs         外层实例的类型实参，与 `params` 按位置对应。
 */
static void resolveDeferredCall(Checker *c, CallCheck *cc, FuncDef *enclosing,
                                const Type *enclosingInst, Vec *params, Vec *targs) {
    if (!targs || !params) return;
    Vec concrete;
    vecInit(&concrete, c->arena, sizeof(void *));
    for (size_t i = 0; i < cc->targs.len; i++) {
        Type *a = *(Type **)vecAt(&cc->targs, i);
        /* `a` can be NULL (a slot the inference could not fill), and `ttSubstitute` is
         * not guaranteed to be NULL-safe, so bail out first: a failed inference should
         * have been reported on the template already, so here it only means this
         * instance is not ready yet and is skipped. */
        if (!a) return;
        Type *sub = ttSubstitute(c->tt, a, params, targs);
        if (!sub || ttHasParam(sub)) return;  /* still carries `T` => wait for an outer instance */
        *(Type **)vecPush(&concrete) = sub;
    }
    FuncDef *inst = funcInstance(c, cc->tmpl, &concrete, cc->node->line);
    if (!inst) return;
    planSetUsed(inst, true);
    /* The site may be shared by every instance of the enclosing body (`wrap<i32>` and
     * `wrap<meter>` share one `pick(x)`), and the answer differs per instance: record it
     * against the enclosing instance as well. The plain pointer stays as the fallback
     * answer for a site that belongs to no instance. */
    planSetCallee(cc->node, inst);
    planSetAltCallee(cc->node, enclosing, enclosingInst, inst);
}

/**
 * @brief `makesPool` 闭包的一轮。
 *
 * 泛型 struct 的**模板**那一轮刻意跳过：模板与实例共用同一份 `FuncDef`，模板被检查时
 * `k.hash()` 还解析不出来，在那里标 true 会被冻结，实例那一轮再也改不回来。extern 没有函数
 * 体可走——`stmtMakesPool` 在调用点找被调者，`extc_pool_new` 正是这种情况。
 *
 * @param cx       关闭上下文。
 * @param k        性质（`REACH_POOL`）。
 * @param changed  本轮有改动时置 true。
 */
static void roundMakesPool(CloseCtx *cx, ReachKind k, bool *changed) {
    Module *m = cx->m;
    TypeTable *tt = cx->tt;

    {
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (getReach(f, k)) continue;
            /* An extern has no body to walk; `stmtMakesPool` finds the callee at the call
             * site (`extc_pool_new` itself is declared extern and is exactly that case). */
            if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
        }
        /* **泛型 struct 的模板轮要跳过**：模板与实例**共用同一批 FuncDef**
         * （`template find` 与 `hashMap$hashMap.find` 就是同一个指针），而模板轮的
         * `k.hash()` 解析不出来 ⇒ 只能保守标真，一旦标上，实例轮 `if (getReach(f, k)) continue`
         * 就永远跳过它，精确解析**没机会生效**。泛型结构体的方法体本来就只在实例上跑，
         * 所以交给下面那一轮按实例解析：解析不出来时那一轮仍旧保守（`resolveOnInstance`
         * 返回 NULL ⇒ 视为会建池），多个实例之间是**取或**（任何一个实例要建池 ⇒ 整条链
         * 保守标真），方向仍然是安全的。非泛型结构体没有这个问题，照旧。 */
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (getReach(f, k)) continue;
                if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
            }
        }
        /* 实例那一轮：泛型方法的实例是模板的浅拷贝（共享 body），不在 `m->structs` 里，
         * 而在类型表的 `tt->instances` 上。这一轮**按实例解析**协议方法，
         * 代入方式与 `runMethodCheck` 完全一致。 */
        for (size_t i = 0; i < tt->instances.len; i++) {
            Type *inst = *(Type **)vecAt(&tt->instances, i);
            StructDef *sd = inst ? inst->sdef : NULL;
            if (!sd) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (getReach(f, k)) continue;
                instResTT = tt; instResParams = &sd->typeParams; instResTargs = &inst->targs;
                setPoolCalleeResolver(resolveOnInstance);
                bool mp = bodyReaches(f, k);
                setPoolCalleeResolver(NULL);
                instResTT = NULL;
                if (mp) { setReach(f, k, true); *changed = true; }
            }
        }
    }
}


/**
 * @brief 收集 arena 分析要走的函数名单：自由函数、各 struct 的方法、以及 `impl` 块挂上的方法。
 *
 * `impl` 组也算在内，即便 `impl` 块通常住在与被扩展类型**不同**的模块（`impl ostream` 在
 * `stl::stringio`，类型在 `std::io`）：携带该块的模块才是入口模块。名单只在这一处拼写。
 *
 * @param arena  分配用的 arena。
 * @param m      模块。
 * @param out    输出：追加各 `FuncDef *`。
 */
static void allFunctions(Arena *arena, Module *m, Vec *out) {
    vecInit(out, arena, sizeof(FuncDef *));
    for (size_t i = 0; i < m->funcs.len; i++)
        *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&m->funcs, i);
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&sd->methods, j);
    }
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        for (size_t j = 0; j < im->methods.len; j++)
            *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&im->methods, j);
    }
}

/* ------------------------------------------------------------------
 * 「本帧不会销账」：整张调用图的**最小不动点**（`planRcNoDec`）。
 *
 * 省掉局部引用绑定（`var p = t`）的 `inc` 只在一种情况下不健全：`p` 活着时某个 `dec`
 * 把这个对象的 `hold` 打到 0 —— `reclaim.c` 的 `freeSlot` 当场释放，`p` 悬垂。而 `dec` 全部
 * 来自**引用槽的存储**（`storeRefOwned` / `storeRef` / 覆盖赋值）。所以：
 *
 *     rcNoDec(F) = ¬写引用槽(F) ∧ ¬外部/未知调用(F) ∧ ¬并发(F) ∧ ∀g∈callees(F): rcNoDec(g)
 *
 * 这是最小不动点（从"全部成立"往下收紧）：**互相递归的一组纯读函数仍然成立** —— 这正是
 * `bt` 的 `check` 需要的形状（它只读、只调自己）。自环在这里不构成保守理由：只要环里没有
 * 谁写引用槽，环上就没有 `dec`。
 *
 * 不置位的两类：泛型模板/实例（一个 AST 槽被所有实例共享，一个实例成立不代表另一个）、
 * 以及任何没被标上的函数（默认 `false` = 照常计数，安全方向）。
 *
 * **依赖的一条语言规则（协程）**：本扫描只看 AST。协程帧字段的初始化在 resume 时以**覆盖语义**
 * （`extc_rc_storeRef`，含 `dec`）重跑，那是扫描看不见的写 —— 但它要求"引用跨 `yield` 活着"，
 * 而那是**编译期拒绝**的（`p lives across a yield and carries a reference`）。所以协程帧字段里
 * 不会有引用 ⇒ 不存在看不见的 `dec`。按此扫过 78 个程序（37 个协程形状 + 全部回收语料）：
 * 没有一个函数同时出现 `initRefNoHold` 与覆盖语义的 `storeRef`。
 * **若那条语言规则哪天放松，这里必须显式排除协程函数。**
 */
/**
 * @brief `rcNoDec` 扫描的上下文：检查器 + "已经确定不合格"的旗标。
 *
 * 两个人一起看这个 typedef 与下面两条前置声明：`rcScanStmt` 与 `rcScanExpr` 互相递归
 * （语句里含表达式、表达式里含语句），所以至少要一条前置声明。
 */
typedef struct {
    Checker *c;      /**< 检查器（读类型表、读调用点解析结果）。 */
    bool     bad;    /**< 已经发现"会销账"的来源 ⇒ 这个函数不置位。 */
} RcScan;

/**
 * @brief 语句回调：这个语句会不会造成一次 `dec`（写引用槽 / 静态引用槽 / 并发域）。
 *
 * @param ctx  扫描上下文（`RcScan *`）。
 * @param s    被访问的子语句。
 * @return     `false` 停下整趟走。
 */
static bool rcScanStmt(void *ctx, Stmt *s);

/**
 * @brief 表达式回调：这里有没有解析不到的被调者 / extern / 起并发任务。
 *
 * @param ctx  扫描上下文（`RcScan *`）。
 * @param e    被访问的子表达式。
 * @return     `false` 停下整趟走。
 */
static bool rcScanExpr(void *ctx, Expr *e);

/**
 * @brief 走一层子节点：只给语句时走语句子节点，只给表达式时走表达式子节点。
 *
 * @param s   语句；为 NULL 时改用 `e`。
 * @param e   表达式；`s` 非 NULL 时忽略。
 * @param sc  扫描上下文（命中即把 `bad` 置位）。
 * @return    走完了返回 true；被回调提前停下返回 false。
 */
static bool rcScanChildren(Stmt *s, Expr *e, RcScan *sc) {
    AstVisit v = { rcScanExpr, rcScanStmt, sc };
    return s ? astWalkStmtChildren(s, &v) : astWalkExprChildren(e, &v);
}

/**
 * @brief 语句回调：这个语句会不会造成一次 `dec`（写引用槽 / 静态引用槽 / 并发域）。
 *
 * @param ctx  扫描上下文（`RcScan *`）。
 * @param s    被访问的子语句。
 * @return     `false` 停下整趟走（已经确定这个函数不合格）。
 */
static bool rcScanStmt(void *ctx, Stmt *s) {
    RcScan *sc = ctx;
    if (!s) return true;
    switch (s->kind) {
    case ST_ASSIGN:
        /* 往"含引用"的槽里写 = 一次可能的 dec（覆盖旧值）。类型不明也当坏。 */
        if (!s->u.assign.target || !s->u.assign.target->type
            || typeContainsRef(sc->c->tt, s->u.assign.target->type))
            sc->bad = true;
        break;
    case ST_VAR:
        /* 静态存储里的引用槽：别处（含被调者）随时可以写它 ⇒ 保守当坏。 */
        if (s->u.var.isGlobal && (!s->type || typeContainsRef(sc->c->tt, s->type)))
            sc->bad = true;
        break;
    case ST_DOMAIN:
        sc->bad = true;                       /* 调度域：并发/交错 */
        break;
    default:
        break;
    }
    if (sc->bad) return false;
    return rcScanChildren(s, NULL, sc);
}

/**
 * @brief 表达式回调：这里有没有**解析不到**的被调者 / extern / 起并发任务。
 *
 * @param ctx  扫描上下文（`RcScan *`）。
 * @param e    被访问的子表达式。
 * @return     `false` 停下整趟走。
 */
static bool rcScanExpr(void *ctx, Expr *e) {
    RcScan *sc = ctx;
    if (!e) return true;
    if (e->kind == EX_EXT) { sc->bad = true; return false; }      /* 起并发任务 */
    if (e->kind == EX_DYN) { sc->bad = true; return false; }      /* vtable 派发：被调者解析不到 */
    if (e->kind == EX_CALL || e->kind == EX_METHOD) {
        FuncDef *g = planCallee(e);
        if (!g || g->isExtern) { sc->bad = true; return false; }  /* 未知 / C 黑盒 */
    }
    return rcScanChildren(NULL, e, sc);
}

/**
 * @brief 求整张调用图的 `rcNoDec` 最小不动点，并把结论写进计划侧。
 *
 * 必须**在所有函数体检查完之后**调用：`FuncDef.callees` 那时才填好。泛型模板/实例不置位
 * （一个 AST 槽被所有实例共享，一个实例成立不代表另一个），默认 `false` = 照常计数。
 *
 * @param c  检查器。
 */
static void computeRcNoDec(Checker *c) {
    Module *m = c->m;
    if (!m || m->funcs.len == 0) return;
    /* ① 自判：写引用槽 / 未知调用 / 并发 / 泛型 ⇒ 不置位 */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        bool ok = f && f->body && !planTemplate(f) && f->typeParams.len == 0 && !f->isExtern;
        if (ok) {
            RcScan sc = { c, false };
            rcScanChildren(f->body, NULL, &sc);
            ok = !sc.bad;
        }
        planSetRcNoDec(f, ok);
    }
    /* ② 被调者不成立 ⇒ 自己也不成立；迭代到不动点（互相递归自动收敛） */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (!planRcNoDec(f)) continue;
            for (size_t k = 0; k < f->callees.len; k++) {
                if (!planRcNoDec(*(FuncDef **)vecAt(&f->callees, k))) {
                    planSetRcNoDec(f, false);
                    changed = true;
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------
 * 区域无环判定：`planFnAcyclic` / `planFnRefsCounted`（整张调用图上的两个不动点）。
 *
 * 目的：给"事件档（计数）可以取代清扫"一个**静态前提**。判据 v1（健全优先，宁保守勿漏）：
 *
 *   (i)   每个 ref 类型字段的 store，值是**本帧新分配对象**：直接 `new T` / `alloc<T>(n)`，
 *         或经一次调用返回、且被调函数**无环**且其返回值可证是**新分配**的（`retFresh`）；
 *   (ii)  值是 `null`；
 *   (iii) 值的层**严格大于**宿主的层（值活得更短 ⇒ 这一条边不可能是回边）。只对层下界可靠的
 *         形状下这个结论（`noteAcyclicStore` 里判）：拿偏大的上界去放行是判据的反方向；
 *   (iv)  所有"能丢引用"的位置都是**被计数的 rc 槽**：函数里没有对引用型局部量/参数的非声明
 *         初始化赋值（引用只住在堆对象的 ref 字段里），也没有看不清的调用（黑盒调用涉及引用
 *         ⇒ 判否）。这一条是必须的 —— 只有 (i)(ii)(iii) 而无 (iv) 时，引用住在局部量里，
 *         对象的 `hold` 永远不归零，事件档不会释放任何东西。
 *
 * (i)(ii)(iii) 的逐站点分类在查体时做（`noteAcyclicStore`：层号比较需要作用域）；这里只做
 * 语法层扫描（找 (iv) 的违反、收集调用依赖）与两个不动点：
 *   · `retFresh(F)`：F 的每个 `return` 都交付一个新分配的对象；
 *   · `acyc` / `iv` 沿调用图向下收紧（互相递归的一组函数可以同时成立）。
 * 泛型模板与实例、extern、没有函数体的函数一律不置位（默认 false = 保留清扫，安全方向）。
 */

/* 一条调用依赖：被调者（NULL = 解析不到）与它在源码里的行，诊断用。 */
typedef struct { FuncDef *fn; int line; } AcDep;

/* 一个函数这一趟扫出来的东西。 */
typedef struct {
    Arena      *a;
    Vec         acycDeps;   /* AcDep*：SR_CALL 站点（(i') 要看被调者无环且返回新分配） */
    Vec         ivDeps;     /* AcDep*：能看见函数体的调用点（(iv) 要看被调者也成立） */
    Vec         retDeps;    /* AcDep*：`return <调用>` 的被调者（retFresh 的闭包） */
    bool        acycBad;    /* 出现了 (i)(ii)(iii)(i') 站不住的站点 */
    bool        ivBad;      /* (iv) 的本地部分站不住 */
    const char *acycWhy;    /* 第一条 (i)(ii)(iii)(i') 的理由 */
    const char *ivWhy;      /* 第一条 (iv) 的理由 */
    /* ---- ②「回收无事可做」的另一趟扫描（判据 (a)(b)(c)） ----------------------------
     *
     * 与上面那两趟**独立**：无环判据问的是"引用图有没有回边"，这一趟问"有没有活引用被提前
     * 丢掉"。两者互不蕴含（见 `idleNote` 的说明），所以分开存、分开判。 */
    bool        idleBad;    /* 出现了 (a)(b)(c) 站不住的站点 */
    const char *idleWhy;    /* 第一条理由（`EXTC_DBG_RCIDLE=1` 直接打） */
    Vec         idleFresh;  /* IdleFresh*：`var x = new T` 的 C 名 + 分配点循环深度/arena 层 */
    Vec         idleAsgn;   /* const char*：被赋值的标识符 C 名（宿主不许在里面） */
    Vec         idleAsgnLoop; /* int：与 idleAsgn 平行 —— 该赋值所在的**最内层循环体作用域**（0 = 循环外）*/
    Vec         idleWrites; /* IdleWrite*：每一次往引用槽写值 —— 判据的唯一事实来源 */
    Vec         idleBindLvl;   /* IdleLvl*：绑定 -> 它所指存储的 arena 层（(b′) 的同尺事实） */
    Vec         idleFieldLvl;  /* IdleLvl*：字段名 -> 写进该字段的值的 arena 层（取最浅） */
    Vec         idleFieldStores; /* IdleFieldStore*：字段写的原始值（固定点求解的输入） */
    Vec         idleOrig;    /* IdleOrig*：`name` 的取值都是 `origin` 的投影（flavor ii） */
    Vec         idleKeep;    /* const char*：保留根（历次持有的对象都还可达） */
    Vec         idleFreshSet; /* const char*：每个取值都是本帧新分配（或 null）的绑定 */
    int         idleSeq;    /* 走查里的语句序号（1 起）：判"分配点在站点之前" */
} AcInfo;

/* 一个引用槽的种类。判据只看这几种：别的形状不存在"覆盖丢根"的问题。 */
typedef enum {
    IDLE_DEST_COMPOUND,     /* 复合赋值（`x += …`）：不是"覆盖"，但同样会丢引用 */
    IDLE_DEST_SLOT_NAMED,   /* 栈上具名 ref 槽（局部量/参数）：推迟到 (b′) 的合取 */
    IDLE_DEST_SLOT_OTHER,   /* 栈上其它位置（局部聚合的字段等）：直接判否 */
    IDLE_DEST_HEAP_FIELD,   /* 具名对象的具名字段：(a) 的逐站点判据 */
} IdleDestKind;

/* 一次"往引用槽写值"的规范化描述 —— 判据的**唯一**输入。
 *
 * 三种槽（具名栈槽、其它栈位置、堆字段）过去各有自己的结构与台账；现在合成一份，
 * 于是"加规则"改的是判据那一处，而不是再添一个台账。
 *
 * 字段名用 `fname` 不用 `field`：`field` 是 AST 节点的成员名，`tools/check_ast_freeze.py`
 * 是文本扫描，会把这个描述符成员赋值误报成"parser 之外写 AST"。
 *
 * `host` 一词两用：`HEAP_FIELD` 是宿主名，`SLOT_NAMED` 是槽名（两者判据里都按名字比）。
 * `isAssign` 区分"赋值站点"与"声明初始化"：(b′) 的合取两种都看，候选站点只取赋值那一半。
 * `allocDepth` 只在堆字段站点上有意义（宿主那条 `new` 的 arena 层，诊断用）。 */
typedef struct {
    IdleDestKind kind;
    Expr        *value;
    const char  *host;       /* HEAP_FIELD：宿主名；SLOT_NAMED：槽名 */
    const char  *fname;      /* HEAP_FIELD：字段名 */
    int          line;
    int          depth;      /* 站点所在循环深度 */
    int          allocDepth; /* 宿主那条 `new` 的 arena 层（诊断用） */
    int          loopScope;  /* 最内层循环体作用域层（(b′) 的阈值） */
    int          seq;        /* 走查序号（判"分配点在站点之前"） */
    int          condDepth;  /* 这个写入点被几层条件语句包住（0 = 本迭代必然执行） */
    bool         seqElem;    /* 最外一层是元素槽、且下标是本循环体里被赋值的标识符 */
    bool         isAssign;   /* 是赋值站点（不是声明初始化） */
} IdleWrite;

/* 判据的结论：`IDLE_DEFER` = 这一期答不了（事实还没齐），换一期再判。 */
typedef enum { IDLE_ACC, IDLE_REJ, IDLE_DEFER } IdleJudgment;

/* 判据的两期：走查期（只有语法与已走查到的声明）与后处理期（台账与不动点都齐了）。 */
enum { IDLE_STAGE_WALK = 0, IDLE_STAGE_POST = 1 };

/* 一条"本帧新局部量"台账：(a) 的候选宿主，也是 (a′) 的"宿主是本轮新建"依据。
 *
 * `depth` 是**分配点**所在的循环深度 + 1（1 起：0 留给"不在台账里"）；`arenaLevel` 是那条
 * `new` 的存储**最后被放**在哪一层 arena（`arenaDepthOf(planArenaLevel)`，逃逸提权之后的值）。
 * 两者都要 ≥ 站点所在循环深度：(a) 问的是"每对象最多写一次"，所以循环嵌套（`depth`）是主判据；
 * arena 层是收窄（宿主被提到更长寿的层时更保守）。`seq` 保证分配在站点之前。 */
typedef struct { const char *name; int depth; int arenaLevel; int seq; } IdleFresh;

/* 一条"这个名字所指的存储落在哪一层 arena"的台账（细化 (b′) 的"同尺事实"）。
 *
 * 层号与 (a′) 侧的 `arenaDepthOf(planArenaLevel(new))` 是**同一把尺**（越小 = 活得越久；
 * 0 = 本帧家区域 / 证不出）。同一名字多条时取**最浅**（活得最久）的那条，方向保守。
 * `null` 不进台账：它不指任何对象，覆盖掉它不丢东西（见 `IDLE_LVL_NULL`）。 */
typedef struct { const char *name; int lvl; } IdleLvl;

/* 一次"把引用写进某个具名对象字段"：字段读（`p.next`）沿它取"这个字段里可能是什么层"。
 * 同一字段名取最浅（活得最久），方向保守。
 * `seq` 是走查序号：细化 P 要问"这次字段写发生在槽覆盖之**前**、且中间没人动过那个槽"。 */
typedef struct { const char *fname; const char *host; Expr *value; int seq; } IdleFieldStore;

/* "这次写入丢的东西是 null（没有对象）"的哨兵层号：它比任何真实 arena 层都"长寿",
 * 但按语义它不丢对象，所以 `idleStoreQualified` 直接算合格。 */
#define IDLE_LVL_NULL 0x3fffffff

/* ② 那一趟的遍历上下文：多带**循环嵌套深度**、**作用域深度**与**最内层循环体的作用域层**。
 *
 * `depth` 是判据 (a)/(a′) 的词法状态：站点在深度 d 的循环里时，只有"本迭代新建"的宿主才能
 * 满足"每对象最多写一次"。`scope` 是块嵌套深度（函数体 1、每进一个块加一，循环体那个块也加
 * 一）—— 与 arena 层同一把尺（都是 checker 的作用域栈深度）。`minLoopScope` 是**当前所在的
 * 所有循环体中作用域层最浅的那个**（b′ 用它当"本迭代随哪一次释放一起死"的阈值；0 = 不在任何
 * 循环里）。语句回调只拿到子节点，三个数都由回调自己往下传。 */
typedef struct { Checker *c; AcInfo *ac; int depth; int scope; int minLoopScope; int condDepth; } IdleWalk;

/* 扫描上下文：检查器（类型表）与这个函数的记录。 */
typedef struct { Checker *c; AcInfo *ac; } AcScan;

static bool acScanStmt(void *ctx, Stmt *s);
static bool acScanExpr(void *ctx, Expr *e);
static bool idleWalkStmtCb(void *ctx, Stmt *s);
static bool idleWalkExprCb(void *ctx, Expr *e);
static bool idleWalkStmt(Checker *c, AcInfo *ac, Stmt *s, int depth, int scope, int minLoopScope, int condDepth);
static bool idleWalkExpr(Checker *c, AcInfo *ac, Expr *e, int depth, int minLoopScope);
static bool idleWalkExprFlag(Checker *c, AcInfo *ac, Expr *e, int depth, bool bound, int minLoopScope);


/**
 * @brief 这个函数算得出区域无环结论吗（有函数体、不是泛型体/实例、不是 extern）。
 *
 * 泛型体（含**属主结构体带类型参数**的方法，如 `vector<T>::grow`）不置位：一个 AST 槽被所有
 * 实例共享，模板这一趟的类型是不透明的。算不出结论 ≠ 判否：调用点按参数敏感规则处理
 * （实参不带引用 ⇒ 它拿不到我们的引用）。
 *
 * @param g  函数定义。
 * @return   算得出则 true。
 */
static bool acAnalyzable(const FuncDef *g) {
    if (!g || !g->body || g->isExtern || planTemplate(g)) return false;
    if (g->typeParams.len > 0) return false;
    if (g->owner && g->owner->typeParams.len > 0) return false;
    return true;
}

/**
 * @brief 这个函数是不是某个 `impl Trait for T` 的方法（代码生成会为 vtable 发射它）。
 *
 * `codegen.c` 的 `inTraitTable` 的镜像：一个方法**没被点名调用**也可能因为出现在
 * `impl Trait` 里而被发射，于是 `unitAllAcyclic` 会要求它被判过。诊断要能解释
 * "为什么没生效"，就得按同一张表算"哪些函数会被发射"。
 *
 * @param m  模块。
 * @param f  函数定义。
 * @return   在 trait 表里则 true。
 */
static bool acTraitTable(const Module *m, const FuncDef *f) {
    if (!f || !f->owner) return false;
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt((Vec *)&m->impls, i);
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            if (mth == f) return true;
            if (mth->name && f->name && strcmp(mth->name, f->name) == 0) return true;
        }
    }
    return false;
}

/**
 * @brief 代码生成会把这个函数发出去吗（`collectEmitted` 的口径，诊断对齐 `unitAllAcyclic`）。
 *
 * `unitAllAcyclic` 要求的是**被发射的每个函数**都 `planFnAcyclic`，而"被发射"比"被调用"宽：
 * extern 声明（`planUsed` 即发射）与 `impl Trait` 的方法都算。泛型模板本身不发射（`T` 还是
 * 形参，发出来编不过）；泛型结构体的模板方法只有它的**实例**被发射时才算。
 *
 * @param m  模块。
 * @param f  函数定义。
 * @return   会被发射则 true。
 */
static bool acRequired(const Module *m, const FuncDef *f) {
    if (!f) return false;
    bool isMain = f->name && strcmp(f->name, "main") == 0;
    if (!planUsed(f) && !isMain && !f->isExport && !acTraitTable(m, f)) return false;
    if (f->typeParams.len > 0) return false;                 /* 模板不发射 */
    if (!planTemplate(f) && f->owner && f->owner->typeParams.len > 0) return false;  /* 泛型结构体的模板方法 */
    return true;
}

/**
 * @brief 一个"会被发射、但没算过"的函数的原因（`unproven` 行用）。
 *
 * @param g  函数定义。
 * @return   原因串（静态字符串）。
 */
static const char *acUnprovenWhy(const FuncDef *g) {
    if (planTemplate(g)) return "a generic instance (not analysed)";
    if (g->isExtern) return "extern: emitted, but it has no body to analyse";
    if (!g->body) return "no body (a declaration only)";
    return "not analysed";
}


/**
 * @brief 一个**栈槽**（局部量/参数/局部聚合的字段）持有被计数的 rc 引用吗。
 *
 * 任何 `ref` 都算：`ref i64` 也可能指向 `new i64` 这种 rc 对象，局部量丢它就没人销账。
 * 视图句柄不算（`typeHoldsCountedRef`）。
 *
 * @param c  检查器（类型表）。
 * @param t  槽的类型；NULL 按不持有算。
 * @return   持有则 true。
 */
static bool acSlotHoldsCountedRef(Checker *c, Type *t) {
    if (!t) return false;
    if (mentionsParam(t)) return true;
    return typeHoldsCountedRef(c->tt, tsub(c, t));
}

/**
 * @brief 一个**实参/接收者**把引用交给黑盒了吗（黑盒测试专用，比槽判据宽一档）。
 *
 * 只有"牵得住一个 rc 对象"的引用才算：`ref X` 且 `X` 自己还能装引用（黑盒可能把它存进堆槽，
 * 而那个对象还可能牵住别的对象）。`ref i64` / `ref bool` 这种指向标量的引用**不算** —— 调用点
 * 的逃逸规则本来就不许黑盒留住它，它也不牵住任何 rc 对象；`fill(buf)` / `flush()` 这类 I/O
 * 助手的签名里全是这种（`mut slice<u8>` + `mut ref i64`），拿它们判否会把整程序钉成有环。
 *
 * @param c  检查器（类型表）。
 * @param t  实参或接收者的类型；NULL 按不带引用算。
 * @return   可能把 rc 引用交给黑盒则 true。
 */
static bool acTypeMaybeRef(Checker *c, Type *t) {
    if (!t) return false;
    if (mentionsParam(t)) return true;
    Type *b = tsub(c, t);
    if (b && b->kind == TY_REF) return typeHoldsCountedRef(c->tt, b->inner);
    return typeHoldsCountedRef(c->tt, b);
}

/**
 * @brief 一个投影的基表达式命名的是**堆对象**吗。
 *
 * 只看语法与节点上的类型（后一趟没有作用域，不能 `lookup`）：引用型绑定作基 ⇒ 宿主是被指物；
 * 解引用 ⇒ 宿主是被指物；普通绑定作基 ⇒ 宿主在栈上。(iv) 判的就是"槽在不在堆上"。
 *
 * @param c    检查器（类型表）。
 * @param obj  投影的基表达式。
 * @return     基是堆对象则 true。
 */
static bool acBaseIsHeap(Checker *c, Expr *obj) {
    if (!obj) return false;
    switch (obj->kind) {
    case EX_DEREF: return true;
    case EX_IDENT: {
        Type *t = obj->type ? tsub(c, obj->type) : NULL;
        return t && t->kind == TY_REF;
    }
    case EX_FIELD: return acBaseIsHeap(c, obj->u.field.obj);
    case EX_INDEX: return acBaseIsHeap(c, obj->u.index.obj);
    case EX_SLICE: return acBaseIsHeap(c, obj->u.slice.obj);
    default:       return false;
    }
}

/**
 * @brief 赋值的写目标命名的引用槽在堆对象里吗。
 *
 * `EX_IDENT`（改指本帧的绑定）与"根是普通绑定的投影"都在栈上：判据 (iv) 判否的那一栏。
 *
 * @param c  检查器（类型表）。
 * @param t  写目标。
 * @return   槽在堆对象里则 true。
 */
static bool acTargetSlotIsHeap(Checker *c, Expr *t) {
    if (!t) return false;
    switch (t->kind) {
    case EX_DEREF: return true;
    case EX_FIELD: return acBaseIsHeap(c, t->u.field.obj);
    case EX_INDEX: return acBaseIsHeap(c, t->u.index.obj);
    case EX_SLICE: return acBaseIsHeap(c, t->u.slice.obj);
    default:       return false;
    }
}

/**
 * @brief 一个调用点会不会把引用送进黑盒（看不清的被调者）。
 *
 * 参数或接收者可能携带引用时算"涉及引用"：黑盒可能把它存进任何地方，这就不再是"引用只住在
 * 被计数的堆槽里"。
 *
 * @param c  检查器（类型表）。
 * @param e  调用表达式。
 * @return   涉及引用则 true。
 */
static bool acCallTouchesRef(Checker *c, Expr *e) {
    switch (e->kind) {
    case EX_METHOD:
        if (e->u.method.recv && acTypeMaybeRef(c, e->u.method.recv->type)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.method.args, i))->type)) return true;
        return false;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.call.args, i))->type)) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.assoc.args, i))->type)) return true;
        return false;
    default: return false;
    }
}

/* ---- ② 「回收无事可做」：判据 (a)(b)(c) 的逐站点扫描 --------------------------------
 *
 * 目的：给「整段不发射 rc 运行期」一个静态前提。一个**区域**
 * （= 一个 arena 层）不需要回收，当且仅当它里面不会有对象**在原地还没死的时候失去最后一个
 * 根**。静态上可判的三条：
 *
 *   (a) 每个堆对象的每个 ref 字段**每对象最多被写一次**（初始化，不是覆盖）。这里按形状判：
 *       该写站点的宿主是本帧 `var x = new T` 出来的局部量、这个绑定在整具函数体里从不被重新
 *       赋值、站点**不在任何循环里**，且同一个（宿主声明, 字段）在这个函数里只有**一处**站点。
 *       前两条把"宿主对象"钉死在这条 `new` 上（递归的每层调用各有自己的 `x` ⇒ 各有自己的动态
 *       对象），第三条保证那一处站点每对象只执行一次。
 *   (b) **ref 类型的局部量 / 参数不被重新赋值**：写进栈上槽（标识符、局部聚合的字段）的引用
 *       赋值都是丢引用。嵌套块里**声明**一个 ref 局部量不算 —— 它的对象随块 arena 整片释放
 *       （`bt` 的 `let t = bottomUp(d)` 正是这一档）。
 *   (c) 没有丢引用的操作：`@overwrite` 复用同一格、`domain` 块、`ext` / `dyn`（看不清的并发与
 *       派发）、以及**看不清的被调者**带着引用（黑盒可能把它丢掉）。看得清的被调者不在这里
 *       追 —— 判定按**整单元合取**（`codegen.c` 的 `unitReclaimIdle`），被调者自己会被判一遍。
 *
 * **与无环判据 (i)(ii)(iii)(iv) 的关系**：互不蕴含。②允许回边（`a.next = b  b.next = a`
 * 两条站点各自满足 (a)），而无环判据允许"每对象写一次"以外的丢引用（计数能兜住）。两条判据
 * 服务两个不同的机制：无环 → 事件档（计数代替清扫），② → 干脆不发射 rc。②的安全网是**峰值
 * RSS 有界**（判错 = 少收 ⇒ 内存涨，不是悬垂）。
 *
 * **失败方向**：任何一条站不住 ⇒ 整具函数判否 ⇒ 整单元判否 ⇒ 照常发射 rc（保守）。
 */

/** @brief 记下 ② 的第一个判否理由（只记第一条，与 (iv) 同一约定）。 */
static void idleNote(AcInfo *ac, const char *why) {
    if (ac->idleBad) return;
    ac->idleBad = true;
    ac->idleWhy = why;
}

/** @brief `planCName` 的首选写法：它答不出时退回源码里的名字。 */
static const char *idleBindName(const void *node, const char *fallback) {
    const char *n = planCName(node);
    return n ? n : fallback;
}

/**
 * @brief 一个类型里有没有**回收管辖的引用槽**（② 的"带引用的值聚合"那一问）。
 *
 * 与 `codegen.c` 的 `rcScanType` 同一条口径：**视图**（`slice<T>`，包括嵌套在结构体字段里的）
 * 不是 rc 对象——它指向 arena/板的内存，丢弃它不释放任何 rc 对象；枚举载荷静态展不开 ⇒ 保守；
 * 泛型实参里有引用槽 ⇒ 枚举不出来 ⇒ 保守；新 kind 可能藏着引用 ⇒ 保守。
 *
 * 不直接用 `typeHoldsCountedRef`：它在**结构体字段递归**那一层不套视图规则，于是
 * `reader` / `writer` 这类"只装一个视图"的值类型会被误判成带引用（实测把 stdlib 的
 * `readerOf` / `writerOf` / `inSync` 判否，进而整单元判否）。口径必须与 rc 记账那一侧一致：
 * 只有**会被 `storeRef*` 记账**的槽才算。
 *
 * @param tt     类型表。
 * @param t      要问的类型；NULL 保守答有。
 * @param depth  递归深度，防类型环。
 * @return       有（或展不开）则 true。
 */
static bool idleCarriesRef(TypeTable *tt, Type *t, int depth) {
    if (!t || depth > 16) return true;                       /* 保守 */
    if (ttIsViewType(t)) return false;                       /* 视图不是 rc 对象 */
    switch (t->kind) {
    case TY_REF: return true;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return false;
    case TY_ENUM:
        /* 变体载荷逐个看：**这里问的是"丢了这个值会不会松开一个 rc 对象"**，所以按载荷精确
         * 回答（`rcScanType` 那一侧要的是"能不能枚举出根表"，才必须保守）。 */
        if (!t->edef) return true;
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (idleCarriesRef(tt, payloadType(tt, t, va, i), depth + 1)) return true;
        }
        return false;
    case TY_ARRAY: return idleCarriesRef(tt, t->inner, depth + 1);
    case TY_STRUCT: {
        StructDef *sd = structOf(t);
        if (!sd) return true;
        for (size_t i = 0; i < sd->fields.len; i++)
            if (idleCarriesRef(tt, (*(FieldDef **)vecAt(&sd->fields, i))->type, depth + 1))
                return true;
        return false;
    }
    case TY_GENERIC: {
        if (!t->sdef) return true;
        for (size_t i = 0; i < t->targs.len; i++)
            if (idleCarriesRef(tt, *(Type **)vecAt(&t->targs, i), depth + 1)) return true;
        StructDef *sd = t->sdef;
        Vec *sp = (t->targs.len == sd->typeParams.len) ? &sd->typeParams : NULL;
        Vec *sa = sp ? &t->targs : NULL;
        for (size_t i = 0; i < sd->fields.len; i++)
            if (idleCarriesRef(tt, ttSubstitute(tt, (*(FieldDef **)vecAt(&sd->fields, i))->type, sp, sa),
                               depth + 1))
                return true;
        return false;
    }
    default: return true;                                    /* 新 kind 可能藏着引用 */
    }
}

/** @brief 名字在不在那个向量里（小向量，线性查）。 */
static bool idleNameIn(Vec *v, const char *nm) {
    if (!nm) return false;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), nm) == 0) return true;
    return false;
}

/**
 * @brief 写目标的最外宿主绑定（`t.f` / `(*p).f` / `h.a.f` / `(*p).a.f` → `t` / `p` / `h` / `p`）。
 *
 * 只沿**具名**的一路往外走：字段取它的基，解引用取它的操作数（必须是标识符）。元素、切片、
 * 调用结果作基一律答 NULL ⇒ 站点判否（保守）。判据 (a) 要的正是"这个对象是哪条 `new` 出来的"，
 * 所以宿主必须是一个绑定。
 *
 * @param t  写目标表达式。
 * @return   绑定表达式（`EX_IDENT`），或 NULL。
 */
static Expr *idleHostIdent(Expr *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case EX_FIELD: return idleHostIdent(t->u.field.obj);
    case EX_INDEX: return idleHostIdent(t->u.index.obj);
    case EX_DEREF: {
        Expr *o = t->u.deref.operand;
        return (o && o->kind == EX_IDENT) ? o : NULL;
    }
    case EX_IDENT: return t;
    default:       return NULL;
    }
}

/**
 * @brief 一条语句是不是**循环**（循环深度要加一）。
 *
 * extC 的四种循环写法（`while`、`loop`、`for (init; cond; step)`、`for x in …`）在解析器里
 * **全部落成 `ST_WHILE`**（见 `src/front/parser.c` 的 `parseFor` / `parseWhile`），`StmtKind` 里
 * 没有 `ST_FOR` ⇒ 这里只需要认这一个 kind。写成函数是为了让"哪些 kind 算循环"只有一个来源。
 *
 * @param s  语句。
 * @return   是循环则 true。
 */
static bool idleLoopStmt(Stmt *s) {
    return s && s->kind == ST_WHILE;
}

/**
 * @brief 一条语句是不是**开新词法作用域**的语句（作用域深度要加一）。
 *
 * 只有块。循环体在 extC 里就是一个 `ST_BLOCK`，所以它由这一条覆盖，不需要另加。
 *
 * @param s  语句。
 * @return   是块则 true。
 */
static bool idleScopedStmt(Stmt *s) {
    return s && s->kind == ST_BLOCK;
}

/**
 * @brief 细化 E：这个名字是不是在**当前循环体内**被赋过值。
 *
 * 元素槽序列（`(*root)[i] = …`）能放行的理由是"每轮写的是**不同叶槽**"。这只在下标**随循环推进**
 * 时成立，所以要求下标的绑定**在本循环体内被赋值**（`i = i + 1`）；循环外赋一次就固定的下标
 * （`j = 0` 之后 `arr[j] = x`）会被这条挡住 —— 那才是"同一个槽被每轮改写"。
 *
 * @param ac          当前函数的 ② 事实。
 * @param name        下标绑定的 C 名。
 * @param minLoopScope 站点所在的最内层循环体作用域。
 */
static bool idleNameAssignedInLoop(AcInfo *ac, const char *name, int minLoopScope) {
    if (!name || minLoopScope == 0) return false;
    for (size_t j = 0; j < ac->idleAsgn.len && j < ac->idleAsgnLoop.len; j++) {
        const char *nm = *(const char **)vecAt(&ac->idleAsgn, j);
        int sc = *(int *)vecAt(&ac->idleAsgnLoop, j);
        if (nm && strcmp(nm, name) == 0 && sc >= minLoopScope) return true;
    }
    return false;
}

/**
 * @brief 细化 A：被覆盖的旧值是否"**另有根**"。
 *
 * 同一个（宿主, 叶路径）被写两处时，原来一律判否（怕旧值被丢成垃圾）。但如果旧值是 **一个形参的
 * 投影**（`prev.l` / `*p` / `q[i]`），而且本帧**没有写那个宿主的任何字段**（写了就可能把那条根
 * 改掉），那么旧值在覆盖之后**仍然从那个形参可达** ⇒ 覆盖没有丢根，放行。
 *
 * 窄在哪里：只认"形参的投影 + 本帧不写该宿主"；不是"两处写互斥"（那是另一条，语料里兑现 0），
 * 也不是"旧值一定还活着"的通用证明 —— 后者要跨过程/别名分析，本帧给不了。
 *
 * @param ac 当前函数的 ② 事实。
 * @param v  被覆盖那处站点写入的值表达式。
 * @return   true = 旧值另有根（放行）；false = 证不出（仍判否）。
 */
static bool idleValueOtherRoot(AcInfo *ac, Expr *v) {
    int guard = 0;
    while (v && (v->kind == EX_FIELD || v->kind == EX_DEREF || v->kind == EX_INDEX) && guard++ < 16)
        v = v->kind == EX_FIELD ? v->u.field.obj
          : (v->kind == EX_DEREF ? v->u.deref.operand : v->u.index.obj);
    if (!v || v->kind != EX_IDENT) return false;
    Sym *b = identBindOf(v);
    if (!b || b->depth != 0) return false;      /* 只认形参（depth 0） */
    for (size_t j = 0; j < ac->idleFieldStores.len; j++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, j);
        if (fs->host && strcmp(fs->host, v->u.ident.name) == 0) return false;
    }
    return true;
}

/**
 * @brief 细化 B：写目的地的**逐叶路径**（`s.kids[0]` → `kids[0]`、`(*root)[i]` → `[i]`）。
 *
 * 原来的 B 只认 `EX_FIELD`（具名对象的具名字段）。这里把目的地**求值一次**、逐叶落账：路径当站点键，
 * 下标只认**整数字面量**（`kids[0]` 与 `kids[1]` 是不同叶槽）；**非字面量下标也收**（键记成 `base[]`），
 * 因为同叶槽两处写仍然由 A（含细化 A）判 —— 收下标不放过"同一个槽被写两次"。
 *
 * **根上的字段用裸字段名**（`(*p).f` → `f`）：字段台账的读一侧（`idleValueLvl` 的 EX_FIELD 分支）
 * 按 `field.name` 查，键必须对得上；早期版本在这里加了前导点，把已 idle 的 `rebuild` 家族打了回去。
 *
 * @param a  分配路径字符串的 arena。
 * @param t  写目标表达式。
 * @return   路径；说不清叶槽（带调用的基、切片等）时 NULL（调用方判否）。
 */
static const char *idleDestPath(Arena *a, Expr *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case EX_IDENT: return "";
    case EX_FIELD: {
        const char *base = idleDestPath(a, t->u.field.obj);
        if (!base) return NULL;
        return *base ? arenaPrintf(a, "%s.%s", base, t->u.field.name) : t->u.field.name;
    }
    case EX_INDEX: {
        const char *base = idleDestPath(a, t->u.index.obj);
        Expr *ix = t->u.index.index;
        if (!base || !ix) return NULL;
        if (ix->kind != EX_INT) return arenaPrintf(a, "%s[]", base);
        return arenaPrintf(a, "%s[%lld]", base, (long long)ix->u.ival);
    }
    case EX_DEREF: {
        Expr *o = t->u.deref.operand;
        return (o && o->kind == EX_IDENT) ? "" : NULL;
    }
    default: return NULL;
    }
}

/**
 * @brief 语句回调：把子节点按当前深度继续往下走。
 *
 * `astWalkStmtChildren` 的回调看的是**子**节点，两个深度由回调自己往下传：进循环体时循环深度
 * +1（见 `idleLoopStmt`），进块时作用域深度 +1（见 `idleScopedStmt`）。
 *
 * @param ctx  `IdleWalk *`，其 `depth` / `scope` 是**子**节点所在的深度。
 * @param s    被访问的子语句。
 * @return     总是 true。
 */
static bool idleWalkStmtCb(void *ctx, Stmt *s) {
    IdleWalk *w = ctx;
    return idleWalkStmt(w->c, w->ac, s, w->depth, w->scope, w->minLoopScope, w->condDepth);
}

/** @brief 表达式回调：表达式不改变循环深度，原样往下走。 */
static bool idleWalkExprCb(void *ctx, Expr *e) {
    IdleWalk *w = ctx;
    return idleWalkExpr(w->c, w->ac, e, w->depth, w->minLoopScope);
}

/**
 * @brief 在"本帧新局部量"台账里找一个名字（(a) 的候选宿主）。
 *
 * 台账是函数级的线性表，同名可能有多条（不同分支里的同名绑定由 C 名区分）。`seq` 保证分配在
 * 站点**之前**（写站点不可能先于它宿主的那条 `new`）。同一名字取**循环最深**的那条（最有利
 * 的一条），但仍然要求它自己满足顺序。不在台账里返回 NULL。
 *
 * @param ac    这个函数的记录。
 * @param name  宿主的 C 名。
 * @param seq   站点的走查序号。
 * @return      台账里最有利的一条；没有则 NULL。
 */
static IdleFresh *idleFreshAt(AcInfo *ac, const char *name, int seq) {
    if (!name) return NULL;
    IdleFresh *best = NULL;
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (strcmp(fr->name, name) != 0) continue;
        if (fr->seq >= seq) continue;                  /* 分配点必须在站点之前 */
        if (!best || fr->depth > best->depth) best = fr;
    }
    return best;
}

/**
 * @brief 台账里某个名字的 arena 层（同名的多条取**最浅** = 活得最久的那条，方向保守）。
 *
 * @param v     台账（`IdleLvl*` 的向量）。
 * @param name  名字。
 * @return      层号；没有记录或"不可知"返回 -1。
 */
static int idleLvlName(Vec *v, const char *name) {
    if (!name) return -1;
    int best = -1;
    for (size_t i = 0; i < v->len; i++) {
        IdleLvl *e = *(IdleLvl **)vecAt(v, i);
        if (strcmp(e->name, name) != 0) continue;
        if (e->lvl < 0) return -1;                 /* 不可知优先（保守） */
        if (best < 0 || e->lvl < best) best = e->lvl;
    }
    return best;
}

/**
 * @brief 往台账里并一条"这个名字的存储在某层"。同名取最浅；有一条不可知 ⇒ 不可知。
 *
 * `null`（`IDLE_LVL_NULL`）不进台账：它不指对象。
 *
 * @param v     台账。
 * @param a     分配用的 arena。
 * @param name  名字。
 * @param lvl   层号（-1 = 不可知）。
 */
static void idleLvlMerge(Vec *v, Arena *a, const char *name, int lvl) {
    if (!name) return;
    if (lvl == IDLE_LVL_NULL) return;
    for (size_t i = 0; i < v->len; i++) {
        IdleLvl *e = *(IdleLvl **)vecAt(v, i);
        if (strcmp(e->name, name) != 0) continue;
        if (lvl < 0) { e->lvl = -1; return; }
        if (e->lvl < 0) return;                    /* 不可知优先 */
        if (lvl < e->lvl) e->lvl = lvl;
        return;
    }
    IdleLvl *e = arenaAllocZero(a, sizeof *e);
    e->name = name;
    e->lvl  = lvl;
    *(IdleLvl **)vecPush(v) = e;
}

/**
 * @brief 一个被写的引用值落在**哪一层 arena**（细化 (b′) 的同尺事实）。
 *
 * 只用一条尺：`arenaDepthOf(planArenaLevel(...))` —— 与 (a′) 侧、与逃逸分析自己的 placement
 * 同一个数（越小 = 活得越久，0 = 本帧家区域 / 证不出）。
 *   · `null`：`IDLE_LVL_NULL`（不指对象）；
 *   · `new`/`alloc`：这条分配点最后被放在哪一层（逃逸提权之后的值）；
 *   · 标识符：查**绑定台账**（由 `idleSolveLvls` 从本函数所有写入求出来）；
 *   · 字段读：查**字段台账**（同一函数里写进该字段的所有值的层，取最浅）。
 * 其余形状（调用结果、`*p`、下标、聚合字面量……）答 -1 ⇒ 调用方判否。
 *
 * **不用** `anRefDepth` / `Sym.fields[].depth` / 词法嵌套计数 / `lookup()->depth` —— 那几把尺
 * 与 arena 层互不一致（实测 `rebuild` 的 `h = c` 上它们分别给出 1 / 3 / 6 / 4，arena 层是 3）。
 *
 * @param bind   绑定台账。
 * @param field  字段台账。
 * @param v      被写的值。
 * @return       层号（`IDLE_LVL_NULL` 表示 null）；证不出返回 -1。
 */
static int idleValueLvl(Vec *bind, Vec *field, Expr *v) {
    if (!v) return -1;
    if (v->kind == EX_NULL) return IDLE_LVL_NULL;
    if (v->kind == EX_NEW || v->kind == EX_GENCALL)
        return arenaDepthOf(planArenaLevel(v));
    if (v->kind == EX_IDENT)
        return idleLvlName(bind, idleBindName(v, v->u.ident.name));
    if (v->kind == EX_FIELD)
        return idleLvlName(field, v->u.field.name);
    return -1;
}

/**
 * @brief 一次写入"丢的东西"是不是**随本迭代的 arena 一起死**（细化 (b′) 的唯一门槛）。
 *
 * 真条件是：写进去的是 `null`（不丢对象），或者它的存储层不比**本次写入所在的最内层循环体**
 * 更长寿（`lvl >= loopScope`）。后者说的是"这个对象的 arena 就是/就在这次迭代里释放的"，所以
 * 之后覆盖掉这个槽只是少一个根，不会攒下界随轮数涨的垃圾。不在任何循环里（`loopScope == 0`）
 * 或层证不出（`lvl < 0`）⇒ 判否（保守）。
 *
 * @param lvl        被写值的 arena 层。
 * @param loopScope  写入点所在的最内层循环体作用域层。
 * @return           合格则 true。
 */
static bool idleStoreQualified(int lvl, int loopScope) {
    if (lvl == IDLE_LVL_NULL) return true;
    if (lvl < 0) return false;
    return loopScope > 0 && lvl >= loopScope;
}

/** @brief 两个台账是不是逐条相同（名字 + 层号），用于固定点的收敛判断。 */
static bool idleLvlSame(Vec *x, Vec *y) {
    if (x->len != y->len) return false;
    for (size_t i = 0; i < x->len; i++) {
        IdleLvl *a = *(IdleLvl **)vecAt(x, i);
        IdleLvl *b = *(IdleLvl **)vecAt(y, i);
        if (strcmp(a->name, b->name) != 0 || a->lvl != b->lvl) return false;
    }
    return true;
}

/**
 * @brief 求"每个绑定/字段所指的存储落在哪一层 arena"的**最小不动点**（细化 (b′) 的事实）。
 *
 * 为什么要迭代：写入的值可以依赖别的绑定/字段的层，而走查顺序不保证"先定义后使用" ——
 * `rebuild` 的 `c.next = h` 就在 `h = c` **之前**，`p = p.next` 更在两个循环之后。按走查顺序
 * 记一次会得到"不可知"，进而把该 idle 的形状判否（实测）。
 *
 * 怎么迭代：每一轮从**上一轮**的台账重算一遍（`idleValueLvl` 只读上一轮的结果），直到台账逐条
 * 相同或到轮数上限。方向单调：不可知只可能变已知、已知层只取更小 ⇒ 只会更保守。上限 8 轮
 * （普通链两三轮回合；到上限仍不可知 ⇒ 读的一方答 -1 ⇒ 判否）。
 *
 * @param ac  这个函数的记录；结果写回 `idleBindLvl` / `idleFieldLvl`。
 */
static void idleSolveLvls(AcInfo *ac) {
    for (int round = 0; round < 8; round++) {
        Vec nb, nf;
        vecInit(&nb, ac->a, sizeof(void *));
        vecInit(&nf, ac->a, sizeof(void *));
        for (size_t i = 0; i < ac->idleWrites.len; i++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
            if (w->kind != IDLE_DEST_SLOT_NAMED) continue;
            idleLvlMerge(&nb, ac->a, w->host,
                         idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, w->value));
        }
        for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
            IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
            idleLvlMerge(&nf, ac->a, fs->fname,
                         idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, fs->value));
        }
        if (idleLvlSame(&nb, &ac->idleBindLvl) && idleLvlSame(&nf, &ac->idleFieldLvl)) break;
        ac->idleBindLvl  = nb;
        ac->idleFieldLvl = nf;
    }
}

/* flavor (ii) 的来源事实：`name` 的每个取值都是 `origin` 沿**稳定字段**的投影。
 * 最大不动点求（只删不增）：初始化时所有对都在，被"某次写入不是投影"否掉的对才删掉。 */
typedef struct { const char *name; const char *origin; } IdleOrig;

/** @brief 细化 P（flavor i）的声明（定义在下面）。 */
static bool idleLinksOld(AcInfo *ac, const IdleWrite *w);

/**
 * @brief 细化 P（flavor ii）：这次槽覆盖**没丢根** —— 槽里的取值都是某个"保留根"的投影。
 *
 * 语义：`cur = (*cur).next` 这类**游标**。旧的值并没有被丢掉：它仍然是 `head`（或别的保留根）
 * 沿 `next` 这条**只会被初始化、不会被改指**的链上的一个结点。要成立需要三件事：
 *
 *   1. **槽里的每个取值都是同一个根 `origin` 的投影**（表 `ac->idleOrig`：由"这个名字的每次写入
 *      都是 `origin` 的投影"这个合取按最大不动点算出来）；
 *   2. `origin` 是**保留根**（`ac->idleKeep`：它自己历次持有的对象都还可达 —— 每次非空写入要么
 *      被新值链住（细化 P），要么是它自己的投影，要么是唯一一次 `new` 初始化且此后不再赋值）；
 *   3. 投影沿途的字段都是**稳定字段**（`idleFieldStable`：写它的宿主全是本帧 fresh 分配 ⇒ 只会
 *      被创建，不会被改指）。
 *
 * 三条合起来：旧值从 `origin` 沿一条不会被断开的路径仍然可达 ⇒ 覆盖这个槽不丢根。
 * 方向保守：任何一条证不出 ⇒ 不成立（判否）。
 *
 * @param ac 这个函数的记录。
 * @param w  这次槽写入。
 * @return   成立则 true。
 */
static bool idleProjRetained(AcInfo *ac, const IdleWrite *w) {
    if (!w->host) return false;
    for (size_t i = 0; i < ac->idleOrig.len; i++) {
        IdleOrig *o = *(IdleOrig **)vecAt(&ac->idleOrig, i);
        if (!o->name || strcmp(o->name, w->host) != 0) continue;
        for (size_t j = 0; j < ac->idleKeep.len; j++) {
            const char *k = *(const char **)vecAt(&ac->idleKeep, j);
            if (k && strcmp(k, o->origin) == 0) return true;
        }
    }
    return false;
}

/* 表里有没有 (name, origin) 这对。 */
/** @brief 表里有没有 (name, origin) 这对投影。 */
static bool idleOrigHas(Vec *orig, const char *name, const char *origin) {
    for (size_t i = 0; i < orig->len; i++) {
        IdleOrig *o = *(IdleOrig **)vecAt(orig, i);
        if (o->name && o->origin && strcmp(o->name, name) == 0 && strcmp(o->origin, origin) == 0)
            return true;
    }
    return false;
}

/** @brief 名字在不在保留根集合里。 */
static bool idleKeepHas(Vec *keep, const char *name) {
    for (size_t i = 0; i < keep->len; i++) {
        const char *k = *(const char **)vecAt(keep, i);
        if (k && strcmp(k, name) == 0) return true;
    }
    return false;
}

/* 字段 fname 是不是稳定：写它的宿主全是本帧 fresh 分配（没写过 ⇒ 也稳定：没人能改指）。 */
/** @brief 宿主在站点所在的循环里被**推进**过：有一次写入发生在同一循环深度或更深、且写进去的是
 *  本帧新分配的对象。没有它，"宿主对象是新的"只说明它在循环**外**建过一次 —— 循环里反复写的是
 *  **同一个**对象（`fixed_slot_overwrite`：峰值随轮数涨，实测 2.99 → 7.85 MB）。 */
static bool idleAdvancesInLoop(AcInfo *ac, const IdleWrite *w) {
    if (!w->host) return false;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, w->host) != 0) continue;
        if (o->depth < w->depth) continue;                       /* 要在同一循环里或更深 */
        if (!o->value) continue;
        if (o->value->kind == EX_NEW && !o->value->u.new_.count) return true;
        if (o->value->kind == EX_IDENT) {
            const char *vn = idleBindName(o->value, o->value->u.ident.name);
            if (vn && idleNameIn(&ac->idleFreshSet, vn)) return true;
        }
    }
    return false;
}

/** @brief 这个字段名在本函数里只有一处写站点。
 *  细化 P 的宿主事实要它：两个**不同名字**的游标（`tail`/`prev2`）可能是同一个对象，
 *  只按名字查"同（宿主, 字段）两处站点"（规则 A）看不出来。 */
static bool idleLoneFieldSite(AcInfo *ac, const char *fname) {
    if (!fname) return false;
    size_t n = 0;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (fs->fname && strcmp(fs->fname, fname) == 0) n++;
    }
    return n == 1;
}

/** @brief 字段是否稳定：写它的宿主对象都是**本帧新分配**的（只会被创建、不会被改指）。
 *  判"宿主对象是新的"与 E/C/B 三条宿主条款用的是**同一条事实**（见 `idleHostFresh`）。 */
static bool idleFieldStable(AcInfo *ac, const char *fname) {
    if (!fname) return false;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->fname || strcmp(fs->fname, fname) != 0) continue;
        if (idleFreshAt(ac, fs->host, fs->seq)) continue;
        if (idleNameIn(&ac->idleFreshSet, fs->host) && idleLoneFieldSite(ac, fs->fname)) continue;
        return false;
    }
    return true;
}

/** @brief L1「链接入结构」：`vname` 指向的那个对象，是否被某次字段写**链进了 `origin` 的结构**。
 *
 * 形式规格 §3 的那条传递：对 `h.f := v`，若 `h` 是 `origin` 的投影且字段 `f` 稳定
 * ⇒ `v` 这个对象从 `origin` 可达 ⇒ 它成为 `origin` 的投影。缺了它，不动点就比 `A*` 小
 * （`rg_grow_tail` 的 `tail = p` 正是这一条）。 */
static bool idleLinkedInto(AcInfo *ac, Vec *orig, const char *vname, const char *origin,
                           int seq) {
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->value || fs->value->kind != EX_IDENT) continue;
        const char *vn = idleBindName(fs->value, fs->value->u.ident.name);
        if (!vn || strcmp(vn, vname) != 0) continue;
        if (seq > 0 && fs->seq >= seq) continue;                 /* 链入发生在这条写之前 */
        if (!idleFieldStable(ac, fs->fname)) continue;
        if (fs->host && idleOrigHas(orig, fs->host, origin)) return true;
    }
    return false;
}

/* `v` 是不是 `origin` 沿稳定字段的投影（`self` 是正在判的那个名字：自引用按"假设成立"处理，
 * 这是最大不动点的常规做法）。 */
/** @brief `v` 是不是 `origin` 沿稳定字段的投影（自引用按假设成立，配合最大不动点）。
 *  `seq` = 这条写的走查序号（判 L1 的"链入在前"；0 = 不要求顺序）。 */
static bool idleProjOf(AcInfo *ac, Vec *orig, Vec *keep, Expr *v, const char *origin,
                       const char *self, int seq) {
    if (!v) return false;
    if (v->kind == EX_NULL) return true;                    /* 不指对象 ⇒ 不破坏投影这条不变量 */
    if (v->kind == EX_IDENT) {
        const char *n = idleBindName(v, v->u.ident.name);
        if (!n) return false;
        if (strcmp(n, origin) == 0) return idleKeepHas(keep, origin);
        if (self && strcmp(n, self) == 0) return true;      /* 自引用：假设成立 */
        if (idleOrigHas(orig, n, origin)) return true;
        return idleLinkedInto(ac, orig, n, origin, seq);     /* L1：被链进 origin 的结构 */
    }
    if (v->kind == EX_FIELD || v->kind == EX_DEREF) {
        Expr *base = (v->kind == EX_FIELD) ? v->u.field.obj : v->u.deref.operand;
        const char *fld = (v->kind == EX_FIELD) ? v->u.field.name : "[]";
        if (!idleFieldStable(ac, fld)) return false;
        return idleProjOf(ac, orig, keep, base, origin, self, seq);
    }
    return false;                                           /* `new`/调用结果/下标：不是谁的投影 */
}

/* `r` 是不是保留根：它历次持有的对象都还可达。 */
/** @brief `r` 是不是保留根（历次持有的对象都还可达）。 */
static bool idleRetains(AcInfo *ac, Vec *orig, Vec *keep, const char *r) {
    size_t nonzero = 0;
    bool loneNew = false;
    int declDepth = 0;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, r) != 0) continue;
        if (!o->value || o->value->kind == EX_NULL) continue;
        nonzero++;
        loneNew = (nonzero == 1) && (o->value->kind == EX_NEW) && !o->isAssign;
        declDepth = o->depth;
    }
    if (nonzero == 0) return false;                         /* 从没被赋过值：不是可依赖的根 */
    /* 只有一次 `new` 声明初始化 ⇒ 它持有的那个对象一直有根。**但必须不在任何循环里**：
     * 循环内每轮新建的绑定，旧实例在作用域退出后无人持有（存储还可能被提升）⇒ 会累积。 */
    if (nonzero == 1 && loneNew && declDepth == 0) return true;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, r) != 0) continue;
        if (!o->value || o->value->kind == EX_NULL) continue;
        if (idleLinksOld(ac, o)) continue;                  /* 被新值链住 ⇒ 旧值仍可达 */
        if (idleProjOf(ac, orig, keep, o->value, r, r, o->seq)) continue;   /* 还在自己的结构上 */
        return false;
    }
    return true;
}

/**
 * @brief 求 flavor (ii) 的两张表（`idleOrig` / `idleKeep`）的**最大不动点**。
 *
 * 单调收紧（只删）：初始化时所有候选对与所有候选根都在，然后每一轮把"被某次写入否掉"的对与
 * "不再保留"的根删掉，直到不动点。轮数上限 8（普通链两三轮回合）。表建在 `idleWrites` 与
 * `idleFieldStores` 上，规模是函数级的（名字数 × 名字数），不做跨函数外推。
 *
 * @param ac 这个函数的记录；结果写回 `idleOrig` / `idleKeep`。
 */
static void idleSolveProj(AcInfo *ac) {
    Vec names;
    vecInit(&names, ac->a, sizeof(const char *));
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
        if (!idleKeepHas(&names, w->host)) *(const char **)vecPush(&names) = w->host;
    }
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (fr->name && !idleKeepHas(&names, fr->name))
            *(const char **)vecPush(&names) = fr->name;
    }
    for (size_t i = 0; i < names.len; i++) {
        const char *n = *(const char **)vecAt(&names, i);
        for (size_t j = 0; j < names.len; j++) {
            const char *o = *(const char **)vecAt(&names, j);
            IdleOrig *p = arenaAllocZero(ac->a, sizeof *p);
            p->name = n;
            p->origin = o;
            *(IdleOrig **)vecPush(&ac->idleOrig) = p;
        }
        *(const char **)vecPush(&ac->idleKeep) = n;
    }
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        Vec next, keep;
        vecInit(&next, ac->a, sizeof(void *));
        vecInit(&keep, ac->a, sizeof(void *));
        for (size_t i = 0; i < ac->idleOrig.len; i++) {
            IdleOrig *p = *(IdleOrig **)vecAt(&ac->idleOrig, i);
            bool ok = true;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
                if (strcmp(w->host, p->name) != 0) continue;
                if (!idleProjOf(ac, &ac->idleOrig, &ac->idleKeep, w->value, p->origin, p->name, w->seq))
                    ok = false;
            }
            if (ok) *(IdleOrig **)vecPush(&next) = p;
            else changed = true;
        }
        for (size_t i = 0; i < ac->idleKeep.len; i++) {
            const char *k = *(const char **)vecAt(&ac->idleKeep, i);
            if (idleRetains(ac, &next, &ac->idleKeep, k)) *(const char **)vecPush(&keep) = k;
            else changed = true;
        }
        ac->idleOrig = next;
        ac->idleKeep = keep;
        if (!changed) break;
    }
}

/**
 * @brief 细化 P（flavor i）：这次槽覆盖**没丢根** —— 写进去的新值已经把旧值链住了。
 *
 * 形状（头插/尾插/桶插入）：`v.f = <这个槽此刻的值>` 先发生，随后 `槽 = v`。旧值由 `v.f` 可达，
 * 而 `v` 又被写进了槽 ⇒ 覆盖没有切断任何路径。与槽的种类无关（栈槽与字段写都成立），
 * 与 (b′) 的"值随本迭代死"是**两条独立的**合格路径。
 *
 * **B 维的条件不能少**（第一版就是漏了它而不健全）：这次存储必须**本迭代必然执行**
 * （`condDepth == 0`）。否则会有"只有部分轮次把新值存下来"的形状 —— `scale_growth` 的
 * `if i == n-1 { saved = p }`：那些没被存下来的实例被提升到家区域、在作用域退出时丢下，
 * 峰值随轮数涨（实测关档峰值比 3.0×）。**A 维（旧值还有根）不能替代 B 维。**
 *
 * 其余条件：被写的是**标识符** `v`（不是字段读、调用结果等看不清的来源）；台账里存在一次字段
 * 写，宿主正是 `v`，写进去的值是**同一个槽名**；该字段写先于这次覆盖；两者之间这个槽
 * 没被写过（否则链住的是更早的值，不是这次覆盖掉的旧值）。
 *
 * @param ac 这个函数的记录。
 * @param w  这次槽写入。
 * @return   成立则 true。
 */
static bool idleLinksOld(AcInfo *ac, const IdleWrite *w) {
    if (w->condDepth != 0) return false;
    if (!w->value || w->value->kind != EX_IDENT || !w->host) return false;
    const char *v = idleBindName(w->value, w->value->u.ident.name);
    if (!v) return false;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->host || strcmp(fs->host, v) != 0) continue;      /* 宿主就是那个新值 */
        if (fs->seq >= w->seq) continue;                          /* 字段写在覆盖之前 */
        if (!fs->value || fs->value->kind != EX_IDENT) continue;
        const char *held = idleBindName(fs->value, fs->value->u.ident.name);
        if (!held || strcmp(held, w->host) != 0) continue;        /* 链住的正是这个槽 */
        bool clean = true;                                        /* 中间没人写过这个槽 */
        for (size_t j = 0; j < ac->idleWrites.len && clean; j++) {
            IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
            if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host) continue;
            if (strcmp(o->host, w->host) != 0) continue;
            if (o->seq > fs->seq && o->seq < w->seq) clean = false;
        }
        if (clean) return true;
    }
    return false;
}

/**
 * @brief 求"哪些绑定的每个取值都是**本帧新分配**（或 `null`）"的最大不动点。
 *
 * 这是细化 P 的**宿主事实**：E/C/B 三条宿主条款与"字段稳定"都问同一个问题 ——
 * "这次写的那个对象是不是本帧刚建出来的"。字段没被写过 ⇒ 写它不丢根、也不留旧值。
 *
 * 关系是传递的（`prev2 = tail`，而 `tail` 的取值都是新建的）⇒ 迭代到不动点。只删不增：
 * 初始化时所有候选都在，被某次写入否掉的名字才删掉。候选 = 作为具名槽被写过的名字 ∪ 本帧
 * fresh 的名字。轮数上限 8（与别的台账同一约定）。
 *
 * @param ac 这个函数的记录；结果写回 `idleFreshSet`。
 */
static void idleSolveFreshSet(AcInfo *ac) {
    Vec cand;
    vecInit(&cand, ac->a, sizeof(const char *));
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
        if (!idleNameIn(&cand, w->host)) *(const char **)vecPush(&cand) = w->host;
    }
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (fr->name && !idleNameIn(&cand, fr->name)) *(const char **)vecPush(&cand) = fr->name;
    }
    ac->idleFreshSet = cand;
    for (int round = 0; round < 8; round++) {
        Vec next;
        vecInit(&next, ac->a, sizeof(void *));
        bool changed = false;
        for (size_t i = 0; i < ac->idleFreshSet.len; i++) {
            const char *n = *(const char **)vecAt(&ac->idleFreshSet, i);
            bool ok = true, any = false;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, n) != 0) continue;
                any = true;
                if (!o->value || o->value->kind == EX_NULL) continue;
                if (o->value->kind == EX_GENCALL) continue;
                if (o->value->kind == EX_NEW) {
                    if (o->value->u.new_.count) { ok = false; break; }   /* 带长度＝视图/数组 */
                    continue;
                }
                if (o->value->kind == EX_IDENT) {
                    const char *vn = idleBindName(o->value, o->value->u.ident.name);
                    if (vn && (strcmp(vn, n) == 0 || idleNameIn(&ac->idleFreshSet, vn)
                               || idleFreshAt(ac, vn, o->seq)))
                        continue;
                }
                ok = false;
            }
            if (ok && any) *(const char **)vecPush(&next) = n;
            else changed = true;
        }
        ac->idleFreshSet = next;
        if (!changed) break;
    }
}


/**
 * @brief ② 的**唯一判据**：这一次往引用槽写值，会不会让垃圾不再有界。
 *
 * **判据其实算两维**（flavor (i) 的负结果把这一点钉死了：只算第一维会在
 * `if i == n-1 { slot = p }` 这类形状上把累积判掉）：
 *
 *   · **A 维（不丢根）** —— 覆盖一个槽不丢根 ⟺ 旧值还能从**别的**活根到达；
 *   · **B 维（不累积）** —— 写进槽 / 被覆盖掉的值，其**存储**不会在后续轮次里无主地留下。
 *
 * 下面这组规则是这两维的可靠充分条件（近似），按顺序逐条判 —— 顺序就是"第一条命中的理由"，
 * 所以顺序也是行为的一部分。
 *
 * 走查期（`IDLE_STAGE_WALK`，只有语法与已走查到的声明）：
 *   1. 复合赋值 ⇒ 判否（**A**：旧值被就地改写）；
 *   2. 具名栈槽 ⇒ 推迟：`(b′)` 要"写进这个槽的**全部**值都合格"，整趟走完才看得全；
 *   3. 其它栈位置 ⇒ 判否（**A**：引用写进了一个没人追踪的栈位置）；
 *   4. 堆字段：目标不是"具名对象的具名字段" ⇒ 判否；
 *   5. 堆字段：站点在循环里、宿主不是本迭代新建、也不是本循环体内被赋值的元素槽 ⇒ 判否
 *      （细化 E；**B**：非本迭代的宿主会被反复覆盖）；
 *   6. 其余堆字段 ⇒ 推迟到后处理（`(a)` 的逐站点部分）。
 * 后处理期（`IDLE_STAGE_POST`，台账与不动点都齐了）：
 *   7. 堆字段站点：宿主不是本帧 `new` ⇒ 判否（**B**：写进别的对象会累积）；宿主被重新赋值
 *      ⇒ 判否（**B**：宿主可能被换成另一个对象，"每对象写一次"论证失效）；同一（宿主, 字段）
 *      的**更早**站点存在且旧值没有别的根 ⇒ 判否（细化 A；**A**：旧值被第二次覆盖掉）；
 *   8. 具名栈槽站点：`(b′)` 合取 —— 写进这个槽的全部值（含声明初始化）都必须合格
 *      （**B**：值随本迭代的 arena 一起死 ⇒ 不留累积）。
 *
 * 失败方向：任何一条判否 ⇒ 整具函数判否 ⇒ 整单元判否 ⇒ 照常发射回收运行期（保守）。
 *
 * @param ac    这个函数的记录（判据的全部事实来源）。
 * @param w     这次写入的规范化描述。
 * @param idx   `w` 在 `ac->idleWrites` 里的下标（后处理期判"更早站点"用；走查期传 0）。
 * @param why   判否时的理由。**文案与合并前逐字相同**：挡路者分类工具按它认规则。
 * @param stage `IDLE_STAGE_WALK` / `IDLE_STAGE_POST`。
 * @return      `IDLE_ACC` 合格；`IDLE_REJ` 判否（`*why` 已填）；`IDLE_DEFER` 换一期再判。
 */
static IdleJudgment idleWriteOk(AcInfo *ac, const IdleWrite *w, size_t idx,
                                const char **why, int stage) {
    if (stage == IDLE_STAGE_WALK) {
        if (w->kind == IDLE_DEST_COMPOUND) {
            *why = arenaPrintf(ac->a,
                "line %d: a compound assignment to a reference slot overwrites it", w->line);
            return IDLE_REJ;
        }
        if (w->kind == IDLE_DEST_SLOT_NAMED) return IDLE_DEFER;
        if (w->kind == IDLE_DEST_SLOT_OTHER) {
            *why = arenaPrintf(ac->a,
                "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one)",
                w->line);
            return IDLE_REJ;
        }
        if (!w->host || !w->fname || !*w->fname) {
            *why = arenaPrintf(ac->a,
                "line %d: the destination is not a named field of a named object", w->line);
            return IDLE_REJ;
        }
        /* 宿主是不是"本帧新建的对象"要等整趟走完才判得全（`idleFreshSet` 是不动点）⇒ 这里只筛
         * 语法，E（循环里写字段）与 C（宿主不是本帧新建）都推迟到后处理期，按同一条事实判。 */
        return IDLE_DEFER;
    }
    if (w->kind == IDLE_DEST_SLOT_NAMED) {
        /* (b′)：写进这个槽的**全部**值都得合格 —— 同一名字可能有多处写入，任一处不合格 ⇒
         * 这个槽的每一次覆盖都判否（保守）。合格有**两条独立路径**：值随本迭代死（(b′)，B 维），
         * 或这次覆盖被新值链住且本迭代必然执行（细化 P / flavor i，A 维 + B 维）。 */
        if (idleStoreQualified(idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, w->value),
                               w->loopScope)
            || idleLinksOld(ac, w)) {
            bool ok = true;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (o->kind != IDLE_DEST_SLOT_NAMED) continue;
                if (!o->host || !w->host || strcmp(o->host, w->host) != 0) continue;
                if (!idleStoreQualified(idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, o->value),
                                        o->loopScope)
                    && !idleLinksOld(ac, o)) ok = false;
            }
            if (ok) return IDLE_ACC;
        }
        if (idleProjRetained(ac, w)) return IDLE_ACC;   /* flavor (ii)：整槽取值都是保留根的投影 */
        *why = arenaPrintf(ac->a,
            "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one)",
            w->line);
        return IDLE_REJ;
    }
    /* 堆字段站点的后处理：(E)/(C)/(B)/(A)。四问都落在**同一条事实**上 ——
     * "这次写的那个宿主对象是不是本帧刚建出来的"（`idleHostFresh`）：
     *   · E：(a′)——**本迭代**新建（分配深度/arena 层两个条件）；细化 P 的宿主事实另加一条路
     *     （宿主绑定的每个取值都是本帧新分配 + 该字段只有一处站点）；
     *   · C：放宽到"本帧"（台账里有这个名字，或细化 P 的宿主事实）；
     *   · B：宿主绑定被重新赋值 ⇒ 只按名字判"每对象写一次"失效；但若走细化 P 那条路（宿主对象
     *     每次都是新的），重赋值本身不构成问题。
     * 规则 A 只看**更早**的站点，所以按记录顺序判。 */
    IdleFresh *fr = idleFreshAt(ac, w->host, w->seq);
    bool hostThisIter = fr && fr->depth - 1 >= w->depth && fr->arenaLevel >= w->depth;
    bool hostCursor = idleNameIn(&ac->idleFreshSet, w->host) && idleLoneFieldSite(ac, w->fname)
                      && idleAdvancesInLoop(ac, w);
    if (w->depth > 0 && !hostThisIter && !hostCursor && !w->seqElem) {
        *why = arenaPrintf(ac->a,
            "line %d: a reference field is written inside a loop, so one object can be overwritten",
            w->line);
        return IDLE_REJ;
    }
    if (!idleFreshAt(ac, w->host, w->seq) && !hostCursor && !w->seqElem) {
        *why = arenaPrintf(ac->a,
            "line %d: the host object is not a local created by `new` in this frame", w->line);
        return IDLE_REJ;
    }
    if (idleNameIn(&ac->idleAsgn, w->host) && !hostCursor) {
        *why = arenaPrintf(ac->a,
            "line %d: the host binding is reassigned in this function", w->line);
        return IDLE_REJ;
    }
    for (size_t j = 0; j < idx; j++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
        if (o->kind != IDLE_DEST_HEAP_FIELD) continue;
        if (o->host == w->host && o->fname && w->fname && strcmp(o->fname, w->fname) == 0) {
            /* 细化 A：旧值另有根 ⇒ 覆盖没丢根 ⇒ 不是冲突。 */
            if (!(o->value && idleValueOtherRoot(ac, o->value))) {
                *why = arenaPrintf(ac->a,
                    "line %d: the same field of the same host is written at two sites", w->line);
                return IDLE_REJ;
            }
            break;
        }
    }
    return IDLE_ACC;
}

/**
 * @brief ② 的语句访问：判 (a)(b)(c) 的语句那一半，并收集 (a) 需要的局部事实。
 *
 * 这里**不用** `switch (s->kind)`：本函数不是一趟"手写的 kind 遍历"（`tools/check_walkers.py`
 * 的棘轮针对的正是那种），下降一律交给 `astWalkStmtChildren`，所以不存在漏掉新 kind 的盲区。
 *
 * @param c            检查器（类型表）。
 * @param ac           这个函数的记录。
 * @param s            当前语句。
 * @param depth        当前的循环嵌套深度（0 = 不在任何循环里）。
 * @param scope        当前的作用域深度（函数体 1，每进一个块 +1）。
 * @param minLoopScope 当前所在的所有循环体中**作用域层最浅**的那个（0 = 不在任何循环里）；
 *                     细化 (b′) 用它当"这次写入丢的东西是否随本迭代释放"的阈值。
 * @return             总是 true。
 */
static bool idleWalkStmt(Checker *c, AcInfo *ac, Stmt *s, int depth, int scope, int minLoopScope,
                         int condDepth) {
    if (!s) return true;
    ac->idleSeq++;
    if (s->kind == ST_DOMAIN)
        idleNote(ac, "a domain block: the interleaving of reference drops is unprovable");
    if (s->kind == ST_VAR && s->u.var.overwrite) {
        idleNote(ac, arenaPrintf(ac->a,
            "line %d: `@overwrite` reuses one piece of storage, so the previous object is dropped",
            s->line));
    }
    if (s->kind == ST_VAR && !s->u.var.overwrite) {
        Expr *ini = s->u.var.init;
        /* (a) 的候选宿主：**单个** `new T`（不带长度；带长度的是视图）初始化出来的局部量。
         * 记下分配点的循环深度与 arena 层（细化 (a′) 用）。是不是"从不被重新赋值"要等整趟
         * 走完（赋值可能在声明之后、也可能在分支里）。 */
        if (ini && ini->kind == EX_NEW && !ini->u.new_.count) {
            const char *nm = idleBindName(s, NULL);
            if (nm) {
                IdleFresh *fr = arenaAllocZero(ac->a, sizeof *fr);
                fr->name = nm;
                fr->depth = depth + 1;   /* 1 起：0 要留给"不在台账里" */
                fr->arenaLevel = arenaDepthOf(planArenaLevel(ini));
                fr->seq = ac->idleSeq;
                *(IdleFresh **)vecPush(&ac->idleFresh) = fr;
            }
        }
    }
    if (s->kind == ST_ASSIGN) {
        Expr *t = s->u.assign.target;
        if (t && t->type && acSlotHoldsCountedRef(c, t->type)) {
            /* 把这次写入规范化成一个 case，交给**唯一判据**。事实（哪些名字是 fresh、哪些被重新
             * 赋值、各值的 arena 层）由走查就地收集；判据只看 case。 */
            IdleWrite w;
            memset(&w, 0, sizeof w);
            w.value     = s->u.assign.value;
            w.line      = s->line;
            w.depth     = depth;
            w.loopScope = minLoopScope;
            w.seq       = ac->idleSeq;
            w.isAssign  = true;
            w.condDepth = condDepth;
            if (s->u.assign.op) {
                w.kind = IDLE_DEST_COMPOUND;
            } else if (!acTargetSlotIsHeap(c, t)) {
                /* (b)：引用写进了栈上的槽（局部量/参数/局部聚合的字段）—— 那是丢引用。
                 * 细化 (b′)：**ref 标识符槽**可以覆盖，只要写进这个槽的**全部**值都随本迭代的
                 * arena 一起死（`idleStoreQualified`）；`null` 也算合格（它不丢对象）。
                 * 参数走同一套判据：要存进参数的值得活过这次调用 ⇒ 逃逸分析把它放到家区域
                 * （层 0）⇒ 层判据自然判否。局部聚合的字段与别的目的地不在这条里，仍旧判否。 */
                w.kind = (t->kind == EX_IDENT) ? IDLE_DEST_SLOT_NAMED : IDLE_DEST_SLOT_OTHER;
                if (t->kind == EX_IDENT) w.host = idleBindName(t, t->u.ident.name);
            } else {
                Expr *host = idleHostIdent(t);
                w.kind  = IDLE_DEST_HEAP_FIELD;
                w.fname = idleDestPath(ac->a, t);
                /* 细化 E：最外一层是**元素槽**且下标是"本循环体内被赋值"的标识符。 */
                if (t->kind == EX_INDEX && t->u.index.index
                    && t->u.index.index->kind == EX_IDENT)
                    w.seqElem = idleNameAssignedInLoop(ac, idleBindName(t->u.index.index,
                                                    t->u.index.index->u.ident.name), minLoopScope);
                if (host) w.host = idleBindName(host, host->u.ident.name);
            }
            const char *why = NULL;
            IdleJudgment v = idleWriteOk(ac, &w, 0, &why, IDLE_STAGE_WALK);
            if (v == IDLE_REJ) {
                idleNote(ac, why);
            } else if (v == IDLE_DEFER) {
                if (w.kind == IDLE_DEST_HEAP_FIELD) {
                    /* 字段写记原始值：(b′) 的"字段读沿根取"由 `idleSolveLvls` 的固定点算 ——
                     * 写进去的值可能依赖别的绑定（`c.next = h` 在 `h = c` 之前）。 */
                    IdleFieldStore *fs = arenaAllocZero(ac->a, sizeof *fs);
                    fs->fname = w.fname;
                    fs->host  = w.host;
                    fs->value = w.value;
                    fs->seq   = w.seq;
                    *(IdleFieldStore **)vecPush(&ac->idleFieldStores) = fs;
                    w.allocDepth = idleFreshAt(ac, w.host, w.seq)
                                 ? idleFreshAt(ac, w.host, w.seq)->arenaLevel : -1;
                }
                IdleWrite *rec = arenaAllocZero(ac->a, sizeof *rec);
                *rec = w;
                *(IdleWrite **)vecPush(&ac->idleWrites) = rec;
            }
        }
        if (t && t->kind == EX_IDENT) {
            /* 宿主候选的"反面"：这个名字在本函数里被赋过值（不管类型）。 */
            const char *nm = idleBindName(t, t->u.ident.name);
            if (nm) {
                *(const char **)vecPush(&ac->idleAsgn) = nm;
                *(int *)vecPush(&ac->idleAsgnLoop) = minLoopScope;
            }
        }
    }
    /* 声明初始化式是**有界的**那一支：值落进具名槽，槽的 arena 层由它的块深度给出；所以
     * 根本身不按"临时量"判（见 `idleWalkExprFlag` 的 `bound`）。它的子表达式照常判。
     * `ST_VAR` 唯一的子节点就是初始化式，所以这里替掉通用子遍历。
     *
     * 同时记下"这个 ref 槽里被写进去的值"（声明初始化算一次写入）—— (b′) 的合取要它。 */
    if (s->kind == ST_VAR) {
        const char *nm = idleBindName(s, NULL);
        if (!s->u.var.overwrite) {
            Type *dt = s->type ? s->type : (s->u.var.init ? s->u.var.init->type : NULL);
            if (dt && acSlotHoldsCountedRef(c, dt)) {
                IdleWrite w;
                memset(&w, 0, sizeof w);
                w.kind      = IDLE_DEST_SLOT_NAMED;
                w.host      = nm;
                w.line      = s->line;
                w.loopScope = minLoopScope;
                w.value     = s->u.var.init;
                w.seq       = ac->idleSeq;
                w.condDepth = condDepth;
                w.depth     = depth;
                const char *why = NULL;
                if (idleWriteOk(ac, &w, 0, &why, IDLE_STAGE_WALK) == IDLE_DEFER) {
                    IdleWrite *rec = arenaAllocZero(ac->a, sizeof *rec);
                    *rec = w;
                    *(IdleWrite **)vecPush(&ac->idleWrites) = rec;
                }
            }
        }
        return idleWalkExprFlag(c, ac, s->u.var.init, depth, true, minLoopScope);
    }
    /* 进循环体：那个块的作用域层就是"本迭代 arena"的层，取所有在册循环里最浅的一个。
     * `condDepth` 进循环体**归零**：问的是"这一轮的迭代里这次存储会不会发生"，上一层的条件
     * 包不包住这个循环与"每轮都存"无关。 */
    IdleWalk inner = { c, ac, depth + (idleLoopStmt(s) ? 1 : 0),
                       scope + (idleScopedStmt(s) ? 1 : 0), minLoopScope,
                       idleLoopStmt(s) ? 0
                                       : condDepth + ((s->kind == ST_IF || s->kind == ST_MATCH) ? 1 : 0) };
    if (idleLoopStmt(s)) {
        int bodyScope = scope + 1;                  /* 循环体在 extC 里恒是一个块 */
        if (inner.minLoopScope == 0 || bodyScope < inner.minLoopScope)
            inner.minLoopScope = bodyScope;
    }
    AstVisit v = { idleWalkExprCb, idleWalkStmtCb, &inner };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief ② 的表达式访问：判 (c) 那一半（并发、派发、看不清的被调者带引用）。
 *
 * @param c      检查器（类型表）。
 * @param ac     这个函数的记录。
 * @param e      当前表达式。
 * @param depth  当前的循环嵌套深度（表达式不改变它，原样传下去）。
 * @return       总是 true。
 */
static bool idleWalkExprFlag(Checker *c, AcInfo *ac, Expr *e, int depth, bool bound, int minLoopScope) {
    if (!e) return true;
    if (e->kind == EX_EXT)
        idleNote(ac, "a spawned task (`ext`): the callee frame's drops are not visible");
    if (e->kind == EX_DYN)
        idleNote(ac, "a `dyn` call: the callee is not resolved");
    /* (c)：**带引用的值聚合**（结构体/数组/枚举载荷/按值返回的调用结果）被产生成一颗
     * **临时量**时，它一死，里面的引用在那个时刻失去这个根 —— 而临时量的存储属于哪一层 arena
     * 由逃逸分析定，可能的形态是"本帧的家房间"（活得比块长）⇒ 界随轮数涨。
     *
     * `bound` 为真表示这个表达式是**一条声明的初始化式**：值落进一个具名槽，槽的 arena 层
     * 由它的块深度给出（循环体内声明 ⇒ 每轮整片释放）⇒ 有界，不按临时量判。它的**子**表达式
     * 照常判（`wrap { n: mk(i) }` 里的 `mk(i)` 仍是一颗会被丢掉的临时量）。
     *
     * `ref T` 自己不是聚合，不在这一条里（`t.left = bottomUp(...)` 的值是 `?ref tree`）。
     *
     * **细化 F**：站点**不在任何循环里**（`minLoopScope == 0`）时放行。理由是失效方向：②的坏
     * 形态是"**每轮**丢一份、攒在活得比迭代长的 arena 里"，而循环外的临时量每次调用只产生/丢一次，
     * 它的存储最坏落在**调用方当前块**（`arenaArg` 默认就是调用点的块）—— 调用方在循环里时那块
     * 每轮归还，调用方不在循环里时也就丢一次。真正会让它变成"家房间垃圾"的路（把聚合体存进比
     * 迭代长的槽）由 (a)/(b) 那两条判据挡着（实测：`byval_temp` 的 `take(mk(i))` 在**循环里**，
     * 仍判否；`struct_copy_ret_home` 的站点在 `mk` 里、循环外，判 idle 后 N vs 4N = 1.00×）。 */
    if (!bound && e->type && e->kind != EX_IDENT && e->kind != EX_FIELD && e->kind != EX_INDEX
        && e->kind != EX_DEREF && e->kind != EX_SLICE && e->kind != EX_NEW
        && e->kind != EX_REF) {
        Type *et = tsub(c, e->type);
        if (et && et->kind != TY_REF && idleCarriesRef(c->tt, e->type, 0) && minLoopScope != 0)
            idleNote(ac, arenaPrintf(ac->a,
                "line %d: a reference-carrying aggregate is produced as a temporary value;"
                " its references lose this root when it dies", e->line));
    }
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        FuncDef *g = planCallee(e);
        /* 看得清的被调者不在本地判：整单元合取会把它判一遍（见本节的说明）。看不清的按
         * **参数敏感**处理 —— 接收者与实参都不带 rc 引用时它拿不到我们的引用，不构成反例。 */
        if ((!acAnalyzable(g) || planCallViaFn(e)) && acCallTouchesRef(c, e)) {
            const char *nm = (g && g->name) ? g->name
                           : (e->kind == EX_METHOD ? e->u.method.name
                                                   : (e->u.call.callee && e->u.call.callee->kind == EX_IDENT
                                                          ? e->u.call.callee->u.ident.name : "?"));
            idleNote(ac, arenaPrintf(ac->a,
                "line %d: a call to '%s' (whose body is not analysed) carries a reference",
                e->line, nm ? nm : "?"));
        }
    }
    /* 表达式走查不改变三个深度：这里给的 0 只为了让聚合初始化完整（表达式不会走到
     * `idleWalkStmt` 里用它们的那些分支上；真走下去也只会更保守）。 */
    IdleWalk inner = { c, ac, depth, 0, minLoopScope, 1 };
    AstVisit v = { idleWalkExprCb, idleWalkStmtCb, &inner };
    return astWalkExprChildren(e, &v);
}

/** @brief 一般位置的表达式访问（不豁免"带引用的值聚合"那条）。 */
static bool idleWalkExpr(Checker *c, AcInfo *ac, Expr *e, int depth, int minLoopScope) {
    return idleWalkExprFlag(c, ac, e, depth, false, minLoopScope);
}

/**
 * @brief 把一个调用依赖记进一个向量（`AcDep*`，建在 arena 里）。
 *
 * @param dep   依赖向量。
 * @param f     被调函数；NULL 也记（"解析不到"是一条依赖，闭包按不成立处理）。
 * @param line  调用点行号。
 * @param a     分配依赖用的 arena。
 */
static void acPushDep(Vec *dep, FuncDef *f, int line, Arena *a) {
    AcDep *d = arenaAllocZero(a, sizeof *d);
    d->fn = f;
    d->line = line;
    *(AcDep **)vecPush(dep) = d;
}

/**
 * @brief 表达式回调：收集 (iv) 的调用依赖、黑盒调用与并发形式。
 *
 * @param ctx  扫描上下文（`AcScan *`）。
 * @param e    被访问的子表达式。
 * @return     总是 true（走完整趟；理由只记第一条）。
 */
static bool acScanExpr(void *ctx, Expr *e) {
    AcScan *sc = ctx;
    AcInfo *ac = sc->ac;
    if (!e) return true;
    switch (e->kind) {
    case EX_EXT:
        if (!ac->ivBad) { ac->ivBad = true; ac->ivWhy = "a spawned task (`ext`)"; }
        break;
    case EX_DYN:
        if (!ac->ivBad) { ac->ivBad = true; ac->ivWhy = "a `dyn` call: the callee is not resolved"; }
        break;
    case EX_CALL: case EX_METHOD: case EX_ASSOC: {
        FuncDef *g = planCallee(e);
        /* 算得出结论的被调者走调用图闭包（(iv) 也必须成立）；算不出的（黑盒 / extern /
         * 泛型体）按**参数敏感**处理：实参与接收者都不带 rc 对象引用时它拿不到我们的引用，
         * 不构成 (iv) 的反例。`ref i64` 这种指向标量的引用也不算（见 `acTypeMaybeRef`）。 */
        if (!acAnalyzable(g) || planCallViaFn(e)) {
            if (!ac->ivBad && acCallTouchesRef(sc->c, e)) {
                const char *nm = g && g->name ? g->name
                               : (e->kind == EX_METHOD ? e->u.method.name
                                                       : (e->kind == EX_CALL && e->u.call.callee
                                                              && e->u.call.callee->kind == EX_IDENT
                                                          ? e->u.call.callee->u.ident.name : "?"));
                ac->ivBad = true;
                ac->ivWhy = arenaPrintf(ac->a,
                    "a call to '%s' (whose body is not analysed) carries a reference",
                    nm ? nm : "?");
            }
        } else {
            acPushDep(&ac->ivDeps, g, e->line, ac->a);
        }
        break;
    }
    default: break;
    }
    AstVisit v = { acScanExpr, acScanStmt, sc };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief 语句回调：找 (iv) 的违反与 (i') 的依赖。
 *
 * @param ctx  扫描上下文（`AcScan *`）。
 * @param s    被访问的子语句。
 * @return     总是 true（走完整趟；理由只记第一条）。
 */
static bool acScanStmt(void *ctx, Stmt *s) {
    AcScan *sc = ctx;
    AcInfo *ac = sc->ac;
    if (!s) return true;
    if (s->kind == ST_DOMAIN && !ac->ivBad) {
        ac->ivBad = true;
        ac->ivWhy = "a domain block: the interleaving of reference drops is unprovable";
    }
    if (s->kind == ST_VAR && anStoreRefKind(s) == SR_LOCAL && !ac->ivBad) {
        /* 检查器在作用域还活着时标下的：嵌套块里的引用型局部量，块退出时丢掉引用而不销账。 */
        ac->ivBad = true;
        ac->ivWhy = arenaPrintf(ac->a,
            "line %d: a reference local is declared in a nested block; its reference is dropped without a counted slot",
            s->line);
    }
    if (s->kind == ST_ASSIGN) {
        Expr *t = s->u.assign.target;
        /* 带类型判：槽装**被计数的**引用时才算一次引用槽存储。`i = i + 1` 不是，视图句柄
         * （`s = otherSlice`）也不是。 */
        if (t && t->type && acSlotHoldsCountedRef(sc->c, t->type)) {
            if (!acTargetSlotIsHeap(sc->c, t)) {
                /* (iv)：引用写进了栈上的槽（局部量/参数/局部聚合的字段），不是被计数的 rc 槽。 */
                if (!ac->ivBad) {
                    ac->ivBad = true;
                    ac->ivWhy = arenaPrintf(ac->a,
                        "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one); references must live in counted heap slots",
                        s->line);
                }
            } else {
                int kind = anStoreRefKind(s);
                if (kind == SR_NONE || kind == SR_BAD) {
                    if (!ac->acycBad) {
                        ac->acycBad = true;
                        ac->acycWhy = arenaPrintf(ac->a,
                            "line %d: a reference field is written with a value that is neither null, nor a fresh allocation, nor a strictly younger layer",
                            s->line);
                    }
                } else if (kind == SR_CALL) {
                    acPushDep(&ac->acycDeps, planCallee(s->u.assign.value), s->line, ac->a);
                }
            }
        }
    }
    AstVisit v = { acScanExpr, acScanStmt, sc };
    return astWalkStmtChildren(s, &v);
}

/* ---- `retFresh`：这个函数的每个 `return` 都交付一个新分配的对象吗 ------------------
 *
 * 判据 (i') 允许"值经一次调用返回"。要让这条**健全**，被调者只无环还不够：它可能原样返回一个
 * 参数（`fn id(p: ref T) -> ref T { return p }`），那样存进去的就是比宿主更老的对象。所以这里
 * 另问一个问题：F 的每个 `return` 交付的是新分配吗 —— 直接 `new`、调用另一个 `retFresh` 函数、
 * 或者一个**只被初始化过一次、之后没被改过也没取过地址**的局部绑定（`var n = new T; return n`）。
 * 名字按"整具函数体里只声明一次"对齐，避免遮蔽造成张冠李戴。
 */
typedef struct { const char *name; Expr *init; } AcDecl;

typedef struct {
    Checker *c;
    AcInfo  *ac;
    Vec      rets;      /* Expr*：非 void 的 `return` 值 */
    Vec      decls;     /* AcDecl*：`ST_VAR` 声明 */
    Vec      mutated;   /* const char*：直接作为 `=` 左侧的名字 */
    Vec      addressed; /* const char*：出现在 `ref x` 里的名字 */
} FreshScan;

static bool freshStmt(void *ctx, Stmt *s);
static bool freshExpr(void *ctx, Expr *e);

/**
 * @brief 一个表达式交付的是新分配对象吗（`retFresh` 的递归判定）。
 *
 * @param fs  扫描结果（声明表、被改/取址的名字表）。
 * @param e   被交付的值。
 * @return    可证是新分配则 true。
 */
static bool freshValue(FreshScan *fs, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        return true;
    case EX_SIGN: return freshValue(fs, e->u.sign.operand);
    case EX_TRY:  return freshValue(fs, e->u.try_.operand);
    case EX_CONV: return freshValue(fs, e->u.conv.operand);
    case EX_CALL: case EX_METHOD: case EX_ASSOC: {
        FuncDef *g = planCallee(e);
        if (!g || g->isExtern || planCallViaFn(e)) return false;
        acPushDep(&fs->ac->retDeps, g, e->line, fs->ac->a);
        return true;                      /* 依赖由不动点收紧 */
    }
    case EX_IDENT: {
        const char *nm = e->u.ident.name;
        if (!nm) return false;
        AcDecl *d = NULL;
        int count = 0;
        for (size_t i = 0; i < fs->decls.len; i++) {
            AcDecl *cand = *(AcDecl **)vecAt(&fs->decls, i);
            if (cand->name && strcmp(cand->name, nm) == 0) { d = cand; count++; }
        }
        if (count != 1 || !d || !d->init) return false;
        for (size_t i = 0; i < fs->mutated.len; i++)
            if (strcmp(*(const char **)vecAt(&fs->mutated, i), nm) == 0) return false;
        for (size_t i = 0; i < fs->addressed.len; i++)
            if (strcmp(*(const char **)vecAt(&fs->addressed, i), nm) == 0) return false;
        return freshValue(fs, d->init);
    }
    default: return false;
    }
}

/**
 * @brief 语句回调：收集 `return` 值、声明，以及被改过/取过地址的名字。
 *
 * @param ctx  扫描上下文（`FreshScan *`）。
 * @param s    被访问的子语句。
 * @return     总是 true。
 */
static bool freshStmt(void *ctx, Stmt *s) {
    FreshScan *fs = ctx;
    if (!s) return true;
    if (s->kind == ST_VAR && s->u.var.name) {
        AcDecl *d = arenaAllocZero(fs->ac->a, sizeof *d);
        d->name = s->u.var.name;
        d->init = s->u.var.init;
        *(AcDecl **)vecPush(&fs->decls) = d;
    }
    if (s->kind == ST_ASSIGN && s->u.assign.target
        && s->u.assign.target->kind == EX_IDENT && s->u.assign.target->u.ident.name)
        *(const char **)vecPush(&fs->mutated) = s->u.assign.target->u.ident.name;
    if (s->kind == ST_RETURN && s->u.ret.value)
        *(Expr **)vecPush(&fs->rets) = s->u.ret.value;
    AstVisit v = { freshExpr, freshStmt, fs };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 表达式回调：记下取过地址的名字（`ref x` 的别名可能改掉那个绑定）。
 *
 * @param ctx  扫描上下文（`FreshScan *`）。
 * @param e    被访问的子表达式。
 * @return     总是 true。
 */
static bool freshExpr(void *ctx, Expr *e) {
    FreshScan *fs = ctx;
    if (!e) return true;
    if (e->kind == EX_REF && e->u.ref.operand && e->u.ref.operand->kind == EX_IDENT
        && e->u.ref.operand->u.ident.name)
        *(const char **)vecPush(&fs->addressed) = e->u.ref.operand->u.ident.name;
    AstVisit v = { freshExpr, freshStmt, fs };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief 静态判定（不看被调者）：「F 的每个 `return` 都交付新分配对象」。
 *
 * @param f   函数（有函数体）。
 * @param c   检查器（arena 与类型表）。
 * @param ac  记录（retDeps 收在这里）。
 * @return    静态部分成立则 true；`return` 为空（void 函数）时 false。
 */
static bool retFreshLocal(FuncDef *f, Checker *c, AcInfo *ac) {
    FreshScan fs;
    memset(&fs, 0, sizeof fs);
    fs.c = c;
    fs.ac = ac;
    vecInit(&fs.rets, c->arena, sizeof(void *));
    vecInit(&fs.decls, c->arena, sizeof(void *));
    vecInit(&fs.mutated, c->arena, sizeof(void *));
    vecInit(&fs.addressed, c->arena, sizeof(void *));
    AstVisit v = { freshExpr, freshStmt, &fs };
    astWalkStmtChildren(f->body, &v);
    if (fs.rets.len == 0) return false;
    for (size_t i = 0; i < fs.rets.len; i++)
        if (!freshValue(&fs, *(Expr **)vecAt(&fs.rets, i))) return false;
    return true;
}

/**
 * @brief 函数在名单里的下标。
 *
 * @param all  函数名单（`FuncDef *`）。
 * @param g    要找的函数。
 * @return     下标；不在名单里（含 NULL）时 -1。
 */
static long acIndex(Vec *all, FuncDef *g) {
    if (!g) return -1;
    for (size_t i = 0; i < all->len; i++)
        if (*(FuncDef **)vecAt(all, i) == g) return (long)i;
    return -1;
}

/**
 * @brief 被调者是不是"被算过且结论为真"。
 *
 * @param all         函数名单。
 * @param considered  每一个下标是不是被算过的函数。
 * @param flags       结论数组。
 * @param g           被调者。
 * @return            算过且为真时 true。
 */
static bool acFlagAt(Vec *all, const bool *considered, const bool *flags, FuncDef *g) {
    long i = acIndex(all, g);
    return i >= 0 && considered[i] && flags[i];
}

/**
 * @brief 求 `planFnAcyclic` / `planFnRefsCounted`：整张调用图上的两个不动点。
 *
 * 必须在所有函数体检查完、且延迟调用点解析完之后调用（`planCallee` 那时才完整）。诊断
 * `EXTC_DBG_ACYCLIC=1`：每个被用函数一行，加一行程序级总结。
 *
 * @param c  检查器。
 */
void computeFnAcyclic(Checker *c) {
    Module *m = c->m;
    Arena *a = c->arena;
    if (!m) return;
    Vec all;
    allFunctions(a, m, &all);
    size_t n = all.len;
    if (n == 0) return;
    AcInfo *info = arenaAllocZero(a, n * sizeof *info);
    bool *considered = arenaAllocZero(a, n * sizeof(bool));
    bool *rf = arenaAllocZero(a, n * sizeof(bool));
    bool *acb = arenaAllocZero(a, n * sizeof(bool));
    bool *ivb = arenaAllocZero(a, n * sizeof(bool));
    const char **acWhy = arenaAllocZero(a, n * sizeof(const char *));
    const char **ivWhy = arenaAllocZero(a, n * sizeof(const char *));

    for (size_t i = 0; i < n; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        considered[i] = acAnalyzable(f);
        if (!considered[i]) continue;
        info[i].a = a;
        vecInit(&info[i].acycDeps, a, sizeof(void *));
        vecInit(&info[i].ivDeps, a, sizeof(void *));
        vecInit(&info[i].retDeps, a, sizeof(void *));
        AcScan sc = { c, &info[i] };
        AstVisit v = { acScanExpr, acScanStmt, &sc };
        astWalkStmtChildren(f->body, &v);
        rf[i] = retFreshLocal(f, c, &info[i]);
        acb[i] = !info[i].acycBad;
        ivb[i] = !info[i].ivBad;
        acWhy[i] = info[i].acycWhy;
        ivWhy[i] = info[i].ivWhy;

        /* ---- ②「回收无事可做」：判据 (a)(b)(c) 的本地那一半（见 `idleWalkStmt`）------ */
        vecInit(&info[i].idleFresh, a, sizeof(void *));
        vecInit(&info[i].idleAsgn, a, sizeof(void *));
        vecInit(&info[i].idleAsgnLoop, a, sizeof(int));
        vecInit(&info[i].idleWrites, a, sizeof(void *));
        vecInit(&info[i].idleBindLvl, a, sizeof(void *));
        vecInit(&info[i].idleFieldLvl, a, sizeof(void *));
        vecInit(&info[i].idleFieldStores, a, sizeof(void *));
        vecInit(&info[i].idleOrig, a, sizeof(void *));
        vecInit(&info[i].idleKeep, a, sizeof(void *));
        vecInit(&info[i].idleFreshSet, a, sizeof(void *));
        IdleWalk iw = { c, &info[i], 0, 1, 0, 0 };
        AstVisit iv = { idleWalkExprCb, idleWalkStmtCb, &iw };
        astWalkStmtChildren(f->body, &iv);
        idleSolveLvls(&info[i]);
        idleSolveFreshSet(&info[i]);   /* 细化 P 的宿主事实：哪些绑定的取值都是本帧新分配 */
        idleSolveProj(&info[i]);       /* flavor (ii) 的两张表（最大不动点） */
        /* (a)/(a′) 剩下的两半与细化 A：判据的**后处理期**（走查期只筛掉了"循环里写非本迭代
         * 宿主"；这里用完整台账再判 C/B/A）。站点顺序就是记录顺序 —— 细化 A 只看更早的站点。 */
        for (size_t k = 0; k < info[i].idleWrites.len && !info[i].idleBad; k++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&info[i].idleWrites, k);
            if (w->kind != IDLE_DEST_HEAP_FIELD) continue;
            const char *why = NULL;
            if (idleWriteOk(&info[i], w, k, &why, IDLE_STAGE_POST) == IDLE_REJ)
                idleNote(&info[i], why);
        }
        /* 细化 (b′)：栈上 ref 槽的赋值站点推迟到这里判 —— 条件是"写进这个槽的**全部**值都随
         * 本迭代的 arena 一起死"（`idleStoreQualified`）。因为同一名字可能有多处写入，只有整趟
         * 走完才看得全；任一处不合格 ⇒ 这个槽的每一次覆盖都判否（保守）。 */
        for (size_t k = 0; k < info[i].idleWrites.len && !info[i].idleBad; k++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&info[i].idleWrites, k);
            if (w->kind != IDLE_DEST_SLOT_NAMED || !w->isAssign) continue;
            const char *why = NULL;
            if (idleWriteOk(&info[i], w, k, &why, IDLE_STAGE_POST) == IDLE_REJ)
                idleNote(&info[i], why);
        }
    }

    /* retFresh 闭包：被调者不成立 ⇒ 自己也不成立。 */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!rf[i]) continue;
            for (size_t k = 0; k < info[i].retDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].retDeps, k);
                if (!acFlagAt(&all, considered, rf, d->fn)) { rf[i] = false; changed = true; break; }
            }
        }
    }
    /* acyc 闭包：(i') 的每个站点要求被调者「无环**且**返回新分配」。 */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!acb[i]) continue;
            for (size_t k = 0; k < info[i].acycDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].acycDeps, k);
                long gi = acIndex(&all, d->fn);
                const char *why = NULL;
                if (!d->fn) {
                    why = "the callee of a stored call result is not resolved";
                } else if (gi < 0 || !considered[gi] || !acb[gi]) {
                    why = arenaPrintf(a, "the callee '%s' is not proved acyclic",
                                      d->fn->name ? d->fn->name : "?");
                } else if (!rf[gi]) {
                    why = arenaPrintf(a, "the callee '%s' does not provably return a fresh allocation",
                                      d->fn->name ? d->fn->name : "?");
                }
                if (why) {
                    acb[i] = false;
                    acWhy[i] = arenaPrintf(a, "line %d: %s", d->line, why);
                    changed = true;
                    break;
                }
            }
        }
    }
    /* iv 闭包：看清的被调者也必须成立（引用只住在被计数的堆槽里）。 */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!ivb[i]) continue;
            for (size_t k = 0; k < info[i].ivDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].ivDeps, k);
                if (!acFlagAt(&all, considered, ivb, d->fn)) {
                    ivb[i] = false;
                    ivWhy[i] = arenaPrintf(a,
                        "line %d: it calls '%s', which may drop references outside counted heap slots",
                        d->line, (d->fn && d->fn->name) ? d->fn->name : "?");
                    changed = true;
                    break;
                }
            }
        }
    }

    /* 写回计划侧：`fnAcyclic` 是 (i)(ii)(iii)(i') 与 (iv) 的合取，`fnRefsCounted` 单存 (iv)，
     * `fnReclaimIdle` 是另一条判据 (a)(b)(c)（见 `idleWalkStmt` 的说明：两者互不蕴含）。 */
    for (size_t i = 0; i < n; i++) {
        if (!considered[i]) continue;
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        bool ok = acb[i] && ivb[i];
        planSetFnAcyclic(f, ok);
        planSetFnRefsCounted(f, ivb[i]);
        planSetFnReclaimIdle(f, !info[i].idleBad);
        if (getenv("EXTC_DBG_RCIDLE"))
            fprintf(stderr, "[rcidle] %s idle=%d reason=%s\n", f->name ? f->name : "?",
                    (int)!info[i].idleBad,
                    info[i].idleBad ? (info[i].idleWhy ? info[i].idleWhy : "not computed") : "ok");
        if (ok) { anSetAcyclicWhy(f, NULL); continue; }
        if (!acb[i])
            anSetAcyclicWhy(f, arenaPrintf(a, "i-iii:%s",
                acWhy[i] ? acWhy[i] : "a reference store is not provably acyclic"));
        else
            anSetAcyclicWhy(f, arenaPrintf(a, "iv:%s",
                ivWhy[i] ? ivWhy[i] : "references do not live only in counted heap slots"));
    }

    /* `EXTC_DBG_ACYCLIC=1`：每个算过的被用函数一行 `[acyclic] <函数名> fn=<0|1> reason=<...>`；
     * 没算过的每个一行 `skipped`（extern/只有声明：后端不需要它）或 `unproven`（泛型体：被用到就
     * 挡着清扫）；最后一行程序级总结。 */
    if (getenv("EXTC_DBG_ACYCLIC")) {
        size_t nf = 0, na = 0, nskip = 0, nunproof = 0;
        bool hasMain = false;
        for (size_t i = 0; i < n; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f && f->name && strcmp(f->name, "main") == 0) hasMain = true;
        }
        for (size_t i = 0; i < n; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (!f) continue;
            if (!acRequired(c->m, f)) {
                /* 不会被发射（泛型模板 / 泛型结构体的模板方法）⇒ 不挡"关清扫"，只计数（一个程序
                 * 里可能有几十个模板方法，逐个打会把真正的原因埋掉）。 */
                if (planUsed(f) || (f->name && strcmp(f->name, "main") == 0)) nskip++;
                continue;
            }
            if (!considered[i]) {
                /* 会被发射、但检查器没结论（extern / 泛型实例）⇒ `planFnAcyclic` 恒 false，
                 * `unitAllAcyclic` 会因此拒绝。列出来，"为什么没生效"一眼可见。 */
                nunproof++;
                fprintf(stderr, "[acyclic] unproven %s (%s)\n",
                        f->name ? f->name : "?", acUnprovenWhy(f));
                continue;
            }
            nf++;
            if (planFnAcyclic(f)) {
                na++;
                fprintf(stderr, "[acyclic] %s fn=1 reason=ok\n", f->name ? f->name : "?");
                continue;
            }
            const char *why = anAcyclicWhy(f);
            fprintf(stderr, "[acyclic] %s fn=0 reason=%s\n",
                    f->name ? f->name : "?", why ? why : "not computed");
        }
        /* 程序级总结只打一次：定义 `main` 的那个模块就是程序本身；`use` 进来的模块各打各的函数
         * 行，但不重复"程序"总结。
         *
         * `all=1` 的含义是**可用于关清扫**：`unitAllAcyclic` 会发的每个函数都有结论且为真
         * （算过的 `acyclic/funcs`，没算过的见 `unproven` ⇒ 直接挡）。`skipped`（泛型模板）不挡：
         * 它本身不发射。**近似之处**：`unitAllAcyclic` 还会要求"被实例化过的类型"的模板方法，
         * 而实例是 codegen 惰性造的、检查器看不见 ⇒ `skipped` 的模板方法在那种情况下会漏算。 */
        if (hasMain) {
            if (nskip)
                fprintf(stderr, "[acyclic] skipped %zu generic template method(s) (not emitted)\n", nskip);
            fprintf(stderr, "[acyclic] program: funcs=%zu acyclic=%zu skipped=%zu unproven=%zu all=%d\n",
                    nf, na, nskip, nunproof, (int)(nf == na && nunproof == 0));
        }
    }
}

/**
 * @brief 调用点的“目的地有多浅”决策，arena 与 zone 两条通道共用这一份。
 *
 * 返回的是**深度**，数越小活得越久；调用方看到 `<= 0` 就取自己的 HOME。`rec->overflow`
 * （实参没记全）按活得最久（-1）算；深度 0 的实参本身就是 0；命中逃逸集合的实参按 -1 算。
 *
 * @param c    checker。
 * @param rec  记录的调用点信息。
 * @return     目的地深度。
 */

static int callSiteMinDestDepth(Checker *c, EArenaSite *rec) {
    if (rec->overflow) return -1;
    bool seen = false;
    int nd = 0;
    for (int k = 0; k < rec->n; k++) {
        int d = rec->argDepth[k];        /* 0 = parameter/global: outside this frame */
        /* Only the depths whose argument is local to this frame consult the escape set; an
         * argument at depth 0 already forces the home arena. */
        if (d != 0 && rec->argRoot[k] && isEscapeeName(c, rec->argRoot[k])) d = -1;
        if (!seen || d < nd) { nd = d; seen = true; }
    }
    EXTC_DBG_ASSERT(seen || rec->n == 0);
    return nd;
}

/**
 * @brief 检查整个模块，是本趟的入口。
 *
 * 按固定顺序分阶段跑：intern 名字 → 解析签名、字段与枚举载荷里的类型 → 查声明重名 → 查全局
 * 量 → 查每个函数体 → 按具体实例重放被推迟的检查 → 关闭各分析（`needsHome` 传递闭包、逃逸
 * 集合、效应摘要、电平求解、每个调用传的 arena）→ 把决定交给 codegen。
 *
 * 顺序不自由：实例在检查函数体时创建，闭包必须在查体之后；`needsHome` 在闭包之后才定稿；
 * 逃逸集合必须在选择每个调用的 arena 之前已知，所以关闭阶段重算而不是复用按函数的结果。
 * `refDepth` 与 `arenaLevel` 描述同一件事，求解器跑完后每个站点的 `refDepth` 都要从最终的
 * `arenaLevel` 重新同步，否则两个字段会互相矛盾。
 *
 * @param ctx   编译上下文；有诊断时 `hasError` 被置位。
 * @param arena 本趟分配用的 arena。
 * @param tt    类型表。
 * @param m     要检查的模块。
 * @return      模块没有错误时 true。
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    /* 阶段 = 显式状态（阶段契约）。*/

    double tB0=0,tB1=0,tB2=0,tB3=0,tB4=0,tB5=0,tB6=0,tB7=0,tB8=0;
    Checker c;    memset(&c, 0, sizeof c);
    vecInit(&c.funcInsts, arena, sizeof(void *));   /* free function instances */
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    /* `EXTC_DBG_FX=1`: the growth counters, read once here so the loops below never call
     * `dbgOn` per element. `EXTC_DBG_FX_VERBOSE=1` adds one line per body (`levelPass`),
     * which is what tells "a few huge passes" from "many small ones"; the summary at the
     * end of this function is what a scaling table is read from. */
    c.fxOn = dbgOn("EXTC_DBG_FX");
    vecInit(&c.scopes, arena, sizeof(void *));
    vecInit(&c.opChecks, arena, sizeof(void *));
    vecInit(&c.methodChecks, arena, sizeof(void *));   /* #57: method calls on a type parameter */
    vecInit(&c.deferredUses, arena, sizeof(void *));
    vecInit(&c.explicitTargs, arena, sizeof(void *));  /* `f<i32>(...)`: handed to inference */   /* #79: uses of a deferred call's result */
    vecInit(&c.globals, arena, sizeof(void *));
    vecInit(&c.allSyms, arena, sizeof(void *));     /* kept for the EXTC_SELFCHECK invariant scan */
    vecInit(&c.nameUses, arena, sizeof(void *));
    vecInit(&c.narrow, arena, sizeof(void *));
    vecInit(&c.refChecks, arena, sizeof(void *));
    vecInit(&c.lvlRejects, arena, sizeof(void *));
    vecInit(&c.pendingRefStores, arena, sizeof(void *));   /* 推迟的 `(*cur).f = v` 判定 */
    vecInit(&c.cycleMerges, arena, sizeof(void *));        /* 可能闭环的写的候选区域合并 */
    vecInit(&c.callChecks, arena, sizeof(void *));   /* deferred call sites */
    vecInit(&c.narrowMarks, arena, sizeof(size_t));
    vecInit(&c.eSites, arena, sizeof(EArenaSite *));   /* arena decisions that depend on escapes */
    vecInit(&c.lvlFacts, arena, sizeof(LvlFact *));

    /* ---- The **handle** protocol, before any body is checked ----
     * `coroutine<T>` is a storage type, so `next`/`value`/`send` on it must exist whatever order the
     * checker walks functions in: a library may drive handles without declaring a coroutine itself,
     * and the method would otherwise be missing when its body is checked. */
    for (size_t hi = 0; hi < m->structs.len; hi++) {
        StructDef *hsd = *(StructDef **)vecAt(&m->structs, hi);
        Checker   *cc  = &c;
        if (!hsd || !hsd->name || strcmp(hsd->name, "coroutine") != 0) continue;
        if (hsd->methods.len) continue;                 /* synthesized (pre-pass or on demand) */
        /* 与按需合成那条一致：用**标记自己的类型参数名**，因为替换是按名字（`param`）匹配的。
         * `A` 由 `send` 送进去，`B` 由 `value` 交出来；只写一个参数的 `coroutine<T>` 在替换处回落。 */
        const char *an = hsd->typeParams.len > 0
            ? *(const char **)vecAt(&hsd->typeParams, 0) : "A";
        const char *bn = hsd->typeParams.len > 1
            ? *(const char **)vecAt(&hsd->typeParams, 1) : an;
        Type *tpA = arenaAllocZero(cc->arena, sizeof *tpA);
        tpA->kind = TY_PARAM; tpA->name = an; tpA->param = an;
        vecInit(&tpA->targs, cc->arena, sizeof(Type *));
        Type *tpB = arenaAllocZero(cc->arena, sizeof *tpB);
        tpB->kind = TY_PARAM; tpB->name = bn; tpB->param = bn;
        vecInit(&tpB->targs, cc->arena, sizeof(Type *));
        Type *hinst = arenaAllocZero(cc->arena, sizeof *hinst);
        hinst->kind = TY_GENERIC;                       /* an instance with targs: `coroutine<T>` */
        hinst->name = hsd->name;
        hinst->sdef = hsd;
        vecInit(&hinst->targs, cc->arena, sizeof(Type *));
        *(Type **)vecPush(&hinst->targs) = tpA;
        *(Type **)vecPush(&hinst->targs) = tpB;
        const int hw[3] = { 3, 4, 6 };                  /* next / value / send on the handle */
        for (int k = 0; k < 3; k++) {
            const int which = hw[k];
            FuncDef *pm = arenaAllocZero(cc->arena, sizeof *pm);
            pm->name      = which == 3 ? "next" : which == 4 ? "value" : "send";
            pm->owner     = hsd;
            pm->line      = hsd->line;
            planSetCoroProto(pm, which);
            pm->ret       = which == 4 ? tpB : ttFromName(cc->tt, "bool");
            vecInit(&pm->params, cc->arena, sizeof(Param *));
            Param *self = arenaAllocZero(cc->arena, sizeof *self);
            self->name = self->cname = "self";
            self->line = hsd->line;
            Type *rt = arenaAllocZero(cc->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = hinst;
            rt->mut = which != 4;                       /* `next`/`send` advance, `value` reads */
            self->type = rt;
            *(Param **)vecPush(&pm->params) = self;
            if (which == 6) {
                Param *vp = arenaAllocZero(cc->arena, sizeof *vp);
                vp->name = vp->cname = "v";
                vp->type = tpA;   /* `send(v: A)` */
                vp->line = hsd->line;
                *(Param **)vecPush(&pm->params) = vp;
            }
            *(FuncDef **)vecPush(&hsd->methods) = pm;
        }
    }
    vecInit(&c.stores, arena, sizeof(StoreSite *));    /* long-running: "stored at level k" facts */
    /* Must be initialized here, before any pass.
     *
     * `checkGlobals` runs before the first `checkFunc`, so initializing `curArenaSites` inside
     * `checkFunc` alone is too late: a top-level initializer with a `new` (`var G = new i32`) or a
     * call to a function that needs a home arena (`let G = helper()`) pushes onto an uninitialized
     * Vec, so `vecGrow` reaches `arenaAlloc(NULL)` and the compiler dies with SIGSEGV instead of
     * reporting "a global can only be initialized with a constant". */
    vecInit(&c.curArenaSites, arena, sizeof(Expr *));

    c.tI32  = ttFromName(tt, "i32");
    c.tI64  = ttFromName(tt, "i64");
    c.tF64  = ttFromName(tt, "f64");
    c.tBool = ttFromName(tt, "bool");
    /* A string literal has type `slice<u8>`.
     * It comes from the prelude, so the string type itself is written in extC and the
     * compiler only has to turn the literal into one of its values. */
    {
        Type *base = ttFromName(tt, "slice");
        if (base && base->kind == TY_STRUCT && base->sdef) {
            c.sliceDef = base->sdef;
            Vec args;
            vecInit(&args, arena, sizeof(void *));
            *(Type **)vecPush(&args) = ttFromName(tt, "u8");
            c.tSliceU8 = ttGeneric(tt, base->sdef, &args);
        } else {
            c.tSliceU8 = ttError(tt);
            ctxError(ctx, 1, 1,
                     "the prelude (stdlib/prelude.extc) must define `slice<T>`",
                     "the prelude does not define `slice`, so string literals cannot work");
        }
    }

    /* Intern every struct / type first: method signature comparison needs them. */
    for (size_t i = 0; i < m->structs.len; i++)
        ttFromName(tt, (*(StructDef **)vecAt(&m->structs, i))->name);
    for (size_t i = 0; i < m->types.len; i++)
        ttFromName(tt, (*(TypeDef **)vecAt(&m->types, i))->name);

    /* Attach every `impl` block to its target type.
     *
     * This has to happen before signatures are resolved, because the attachment makes the
     * methods **indistinguishable** from methods written inside the type's body: from here on
     * every pass that walks "struct x method" (signature resolution, body checks, the escape and
     * borrow rules, operator collection, code generation) reaches them with no pass changed at
     * all. That is what makes an attachment point this cheap.
     *
     * A builtin scalar has no declaration body to attach to, so a synthetic holder is created
     * for it: the holder goes into this module's struct list (so those passes see the methods)
     * but is NOT interned under its name in the type table (so `i64` keeps resolving to the
     * builtin, never to a struct) and code generation emits no C struct for it. `Type.mholder`
     * points back at it, which is where method lookup ends up.
     *
     * The holder is keyed off the **type**, not off this module: attaching the same method twice
     * -- from two impl blocks, from two modules, or from the prelude and the program -- hits the
     * duplicate check below. A type has one method set, and coherence is enforced by that check
     * rather than by "the last one silently wins". */
    if (dbgOn("EXTC_DBG_IMPL"))
        fprintf(stderr, "[impl] %zu block(s), %zu struct(s), %zu type(s)\n",
                m->impls.len, m->structs.len, m->types.len);
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (dbgOn("EXTC_DBG_IMPL"))
            fprintf(stderr, "[impl]   target `%s` with %zu method(s)\n", im->typeName, im->methods.len);
        /* `impl Trait for Type` asks three questions, and all three are about the **pair**, which
         * is why they are answered here -- at the attachment point, where the type's one method
         * set is decided -- rather than in a pass of their own. */
        if (im->traitName) {
            TraitDef *tr = NULL;
            for (size_t k = 0; k < m->traits.len; k++) {
                TraitDef *cand = *(TraitDef **)vecAt(&m->traits, k);
                if (cand->name && strcmp(cand->name, im->traitName) == 0) {
                    /* `impl Codec<i64> for X`: an explicit-parameter trait must be implemented
                     * with that many arguments. The *matching* is still by name -- the arguments
                     * are not part of the conformance check yet -- but a wrong arity is caught
                     * here instead of silently binding `T` to nothing. */
                    if (im->traitArgs.len != cand->typeParams.len) {
                        ctxError(ctx, im->line, 1,
                                 "Name the trait's type arguments here: `impl Codec<i64> for X`.",
                                 "`impl %s for %s`: the trait has %zu type parameter(s), %zu given",
                                 im->traitName, im->typeName, cand->typeParams.len, im->traitArgs.len);
                        break;
                    }
                    tr = cand;
                    break;
                }
            }
            if (!tr) {
                ctxError(ctx, im->line, 1,
                         "A trait is declared with `trait Name { ... }`, and an `impl` block must"
                         " name one that exists.",
                         "`impl` on unknown trait `%s`", im->traitName);
                continue;
            }
            /* One implementation per (trait, type): the method set of a type is flat, so a second
             * implementation could only be a silent redefinition. */
            bool dup = false;
            for (size_t k = 0; k < i && !dup; k++) {
                ImplDef *prev = *(ImplDef **)vecAt(&m->impls, k);
                dup = prev->traitName && strcmp(prev->traitName, im->traitName) == 0 &&
                      strcmp(prev->typeName, im->typeName) == 0;
                if (dup)
                    ctxError(ctx, im->line, 1,
                             "A type has one implementation of a trait. Two behaviours means two"
                             " traits.",
                             "`%s` is already implemented for `%s` (first at line %d)",
                             im->traitName, im->typeName, prev->line);
            }
            planSetImplTrait(im, tr);
            /* Completeness. Whether the signatures agree is a question for the types, and is
             * answered once those are resolved; this half asks whether the names are there. */
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                bool found = false;
                for (size_t j = 0; j < im->methods.len && !found; j++)
                    found = strcmp((*(FuncDef **)vecAt(&im->methods, j))->name, want->name) == 0;
                if (!found)
                    ctxError(ctx, im->line, 1,
                             "An implementation supplies every method the trait declares.",
                             "`impl %s for %s` is missing `%s`",
                             im->traitName, im->typeName, want->name);
            }
        }
        /* Resolve the target the way a type annotation is resolved: `ttResolve` consults the
         * module's rename table and the bare-name aliases (`string` -> `stl$string`), counts
         * matches, and refuses an ambiguous bare name with the message every other type
         * position gets. `ttFromName` alone only knows the builtins and the declarations of
         * this module -- which silently made `impl string { ... }` from an importing module an
         * "unknown type" (found by writing `stl/stringio.extc`, the streaming pilot). */
        Type *base = typeNamed(arena, im->typeName);
        /* `impl slice<u8>`: the arguments belong to the target, and `ttResolve` resolves them the
         * way it resolves an annotation's (`targs` is recursed into). */
        base->targs = im->typeArgs;
        Type *t = ttResolve(tt, ctx, base, im->line,
                            im->typeParams.len ? &im->typeParams : NULL);
        StructDef *sd = NULL;
        if (ttIsError(t)) continue;
        if (!t) {
            ctxError(ctx, im->line, 1,
                     "An `impl` block extends a type that exists: a struct declaration, or a "
                     "builtin scalar such as `i64`.",
                     "`impl` on unknown type `%s`", im->typeName);
            continue;
        }
        if (t->kind == TY_BUILTIN) {
            sd = t->mholder;
            if (!sd) {
                sd = (StructDef *)arenaAllocZero(arena, sizeof(StructDef));
                sd->name = im->typeName;
                sd->line = im->line;
                planSetBuiltinHolder(sd, true);
                sd->type = t;
                vecInit(&sd->typeParams, arena, sizeof(void *));
                vecInit(&sd->fields, arena, sizeof(void *));
                vecInit(&sd->methods, arena, sizeof(void *));
                t->mholder = sd;
                *(StructDef **)vecPush(&m->structs) = sd;
            }
        } else {
            sd = structOf(t);
            if (!sd) {
                ctxError(ctx, im->line, 1,
                         "A type that can own methods is a `struct` or a builtin scalar. An enum is "
                         "read with `match`, and a view or a reference is not a declaration.",
                         "`impl` on `%s`, which cannot own methods", im->typeName);
                continue;
            }
            /* `impl<T> pair<T> { ... }`: the methods belong to the generic declaration's body --
             * `structOf(t)` already is that body, and the machinery that instantiates
             * body-declared methods (`resolveSignature` + `ttSubstitute` over `tt->instances`)
             * then gives every instance its own copy. The only thing to check is the names: a
             * method's signature resolves against `f->owner->typeParams`, so a block that renames
             * `T` to `U` would leave `U` unknown. Flat on purpose -- a nested version of this
             * check is what broke the build the first time. */
            if (im->typeParams.len > 0) {
                bool bad = im->typeParams.len != sd->typeParams.len;
                for (size_t pi = 0; !bad && pi < im->typeParams.len; pi++)
                    bad = strcmp(*(const char **)vecAt(&im->typeParams, pi),
                                 *(const char **)vecAt(&sd->typeParams, pi)) != 0;
                if (bad) {
                    ctxError(ctx, im->line, 1,
                             "Name the block's type parameters exactly as the type declares them:"
                             " the methods are resolved against the declaration's body.",
                             "`impl` on `%s`: the type parameters must match the declaration's",
                             im->typeName);
                    continue;
                }
            }
            if (sd->typeParams.len > 0 && im->typeParams.len == 0) {
                if (t->targs.len == 0) {
                    ctxError(ctx, im->line, 1,
                             "A generic type's methods are declared inside its own body; extending "
                             "every instance at once needs the block's own type parameters.",
                             "`impl` on generic type `%s` is not supported yet", im->typeName);
                    continue;
                }
                /* The block names an **instance** (`slice<u8>`), and an instance's method set is
                 * its own: attaching to the generic body would hand `slice<i32>` the same methods
                 * and substitute `Self` to the wrong type. The holder hangs off the `Type`, which
                 * is exactly how a builtin scalar's `impl i64 { ... }` block is kept. */
                StructDef *ih = t->mholder;
                if (!ih) {
                    ih = (StructDef *)arenaAllocZero(arena, sizeof(StructDef));
                    /* The holder is a **distinct declaration** from the generic one, so it needs a
                     * distinct name: reusing `im->typeName` collided with the generic's own
                     * declaration and tripped the reserved-definition check (`slice` comes from
                     * the prelude). The mangled name of the instance is unique by construction. */
                    ih->name = (t->name && t->name[0]) ? t->name : "$inst";
                    ih->line = im->line;
                    planSetBuiltinHolder(ih, true);   /* no C struct of its own: the instance has one */
                    ih->type = t;
                    vecInit(&ih->typeParams, arena, sizeof(void *));
                    vecInit(&ih->fields, arena, sizeof(void *));
                    vecInit(&ih->methods, arena, sizeof(void *));
                    t->mholder = ih;
                    /* The holder has to be in `m->structs`, exactly like a builtin scalar's: the
                     * pass that resolves method signatures walks `StructDef.methods`, so a holder
                     * nobody walks keeps `TY_UNRESOLVED` signatures -- which showed up as
                     * "initializer expects `i64`, found `i64`" (two names, one unresolved type). */
                    *(StructDef **)vecPush(&m->structs) = ih;
                }
                sd = ih;
            }
        }
        /* No duplicate check here **on purpose**. Attaching a method must make it
         * indistinguishable from one written in the body, and the rules for a method set then
         * apply to it exactly as they apply to the body -- including the one that matters here:
         * an operator name may be defined once per **right-operand type** (`isOverloadableOp`),
         * which is how `io::istream` carries `>>` for `i64`, `slice<u8>`, `u8` and now `string`.
         * A second copy of those rules in this pass rejected the legitimate overload with
         * "already has a method named `>>`" (found by attaching `>>` to `io::istream`) -- two
         * copies of a rule drift, and the stricter copy wins silently. The single authority is
         * `checkDeclarations` below, which runs after types are resolved (it needs `ttEquals` on
         * the operand types, which is only meaningful then). */
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            planSetImplTarget(im, t);                 /* resolved here, read by the conformance pass and codegen */
            /* Orphan rule: the block must live where the trait is declared or where the type is
             * declared. Anywhere else it has no home -- and because a type has exactly one method
             * set, a competing implementation from a third module would surface as an unrelated
             * duplicate-method error far from its cause. */
            if (planImplTrait(im) && !sameModule(im->modName, planImplTrait(im)->modName) &&
                              !sameModule(im->modName, sd ? sd->modName : NULL)) {
                ctxError(ctx, im->line, 1,
                         "Implement a trait in the module that declares the trait, or in the one"
                         " that declares the type.",
                         "`impl %s for %s` is an orphan: module `%s` declares neither",
                         im->traitName, im->typeName,
                         im->modName ? im->modName : "this file");
                continue;
            }
            mth->owner = sd;
            *(FuncDef **)vecPush(&sd->methods) = mth;
        }
    }

    /* First pass: resolve the type names in every signature and field. */
    /* Enum payload types are resolved here as well (`| circle(f64) | rect(f64, f64)`). */
    ctPhase("ckB0", tB0); tB1 = ctNow();
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            for (size_t k = 0; k < v->types.len; k++)
                *(Type **)vecAt(&v->types, k) =
                    ttResolve(tt, ctx, *(Type **)vecAt(&v->types, k), v->line, &td->typeParams);
        }
    }
    ctPhase("ckB1", tB1); tB2 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
            fd->type = ttResolve(tt, ctx, fd->type, fd->line, &sd->typeParams);
        }
        for (size_t j = 0; j < sd->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB2", tB2); tB3 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++)
        resolveSignature(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* `@export`: the function is called **from C**, so C has to be able to write its signature down.
     * The predicate is the one `extern!` obeys (`ttCrossesC`), asked in the other direction -- one
     * spelling and two callers, which is what keeps the two halves of the boundary from drifting.
     * One more thing can break the promise (a hidden arena/zone parameter) and is refused in codegen,
     * where that transitive closure is final. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExport) continue;
        for (size_t j = 0; j < f->params.len; j++) {
            Param *p = *(Param **)vecAt(&f->params, j);
            if (ttCrossesC(p->type, false)) continue;
            ctxError(ctx, p->line ? p->line : f->line, 1,
                     "A function C calls must take what C can pass: one machine word per parameter"
                     " -- a scalar, a pointer, a function pointer, or a `@frozen` struct. A"
                     " `slice<T>` is two C parameters, so it cannot be one.",
                     "`@export` parameter %zu has type `%s`, which C cannot pass",
                     j + 1, typeStr(&c, p->type));
        }
        Type *rt = f->ret ? f->ret : ttVoid(tt);
        if (!ttCrossesC(rt, true))
            ctxError(ctx, f->line, 1,
                     "A function C calls must return what C can return: `void`, one machine word, or"
                     " a `@frozen` struct.",
                     "`@export` returns `%s`, which C cannot return", typeStr(&c, rt));
    }

    /* Trait method signatures are resolved here for the same reason struct methods are: the
     * conformance check compares resolved types on both sides. `Self` resolves because the
     * parser gave every signature that type parameter (see parseTrait) -- which is the whole
     * implementation of "`Self` is an implicit type parameter". */
    ctPhase("ckB3", tB3); tB4 = ctNow();
    for (size_t i = 0; i < m->traits.len; i++) {
        TraitDef *td = *(TraitDef **)vecAt(&m->traits, i);
        for (size_t j = 0; j < td->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&td->methods, j));
    }


    checkGlobals(&c);
    checkDeclarations(&c);

    ctPhase("ckB4", tB4); tB5 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkMethodShape(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB5", tB5); tB6 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (planTemplate(fx)) continue;      /* an instance is not checked separately */
        checkMethodShape(&c, fx);
    }

    /* Second pass: check function bodies. */
    c.phase = PHASE_BODIES;   /* 从这一刻起有作用域（阶段契约）*/
    ctPhase("ckB6", tB6); tB7 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB7", tB7); tB8 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++) {
        /* `m->funcs` grows during this loop (instance materialisation pushes to it), so the base
         * pointer is still taken through vecAt every time -- only the next function is prefetched. */
        if (i + 4 < m->funcs.len) __builtin_prefetch(*(FuncDef **)vecAt(&m->funcs, i + 4), 0, 0);
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (planTemplate(fx)) continue;      /* covered by the per-instance recheck */
        checkFunc(&c, fx);
    }

    /* 调用图不动点：必须在**所有函数体检查完之后**（`callees` 那时才填好），
     * 且要在下面这些批次改调用点之前 —— 它们不会新增“写引用槽”的来源。 */
    computeRcNoDec(&c);

    /* This batch has to run **before** the requirement batches below.
     *
     * It repoints the call sites of a provisional instance at a concrete one (`less_T` ->
     * `less_i64`), while the batches below (operators / protocol methods / result types /
     * reference rules) only ever look at concrete instances. The other order let the concrete
     * instance this batch had just created miss its own re-check: it was built, but nobody
     * asked whether `T` really has `hash()`, so the missing check travelled all the way to code
     * generation, which can only emit a `0` for a call it cannot resolve.
     *
     * Three steps per site: substitute the type arguments, intern (or create) that instance,
     * and point the call expression's `func` at it. Instances are interned (`funcInstance`), so
     * one argument combination has exactly one instance. */
    /* Re-resolve each deferred call site into a concrete instance under the
     * substitution of the enclosing instance.
     *
     * `params` / `targs` are the enclosing instance's type parameters and type arguments
     * (and `c.substParams` / `c.substArgs` are set to that same pair). Three steps per
     * site: substitute the type arguments, intern (or create) that concrete instance,
     * then point the call expression's `func` at it. Static instances are interned
     * (`funcInstance` de-duplicates), so one argument combination has exactly one
     * instance. */
    c.phase = PHASE_POST;    /* 之后**没有作用域**：只能读表与记在节点/符号上的身份 */
    ctPhase("ckB8", tB8);   /* after the last of the eight: the rest of checkModule */
    /* **To a fixpoint**, not in a single pass.
     *
     * Resolving one call site can *create* the callee's instance (`funcInstance` interns on
     * demand), and that new instance has deferred call sites of its own that also need resolving.
     * One pass therefore left the inner ones dangling: `fn f<T>` / `fn g<T> { f(x) }` /
     * `fn m<T> { g(x) }` **declared in that order** generated a call to `f_T` that was never
     * emitted, so the C did not compile -- while the same program with the declarations reversed
     * compiled, which is what made this look like a declaration-order oddity instead of a missing
     * round (). `resolveDeferredCall` is idempotent (interning the same argument
     * combination gives the same instance), so repeating the pass is free; the loop stops as soon
     * as a round creates no new instance. The bound mirrors the other fixpoints here and is high
     * enough that a real program never reaches it (each round strictly grows one of the two
     * tables). */
    size_t instsSeen = 0, tinstsSeen = 0;
    for (int round = 0; round < 64; round++) {
        if (round > 0 && c.funcInsts.len == instsSeen && tt->instances.len == tinstsSeen) break;
        instsSeen  = c.funcInsts.len;
        tinstsSeen = tt->instances.len;
        for (size_t i = 0; i < c.callChecks.len; i++) {
            CallCheck *cc = *(CallCheck **)vecAt(&c.callChecks, i);
            if (!cc->node || !cc->tmpl) continue;
            /* (1) The enclosing body is a free-function instance. */
            for (size_t j = 0; j < c.funcInsts.len; j++) {
                FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
                if (planTemplate(fi) != cc->func) continue;   /* not a call in this template body */
                resolveDeferredCall(&c, cc, fi, NULL, &planTemplate(fi)->typeParams, &fi->targs);
            }
            /* (2) The enclosing body is a type instance (a generic call inside a method);
             * `owner` is the struct or enum definition. */
            StructDef *owner = cc->func ? cc->func->owner : NULL;
            if (!owner) continue;
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != owner) continue;
                /* The enclosing body is a **type** instance: the same method body is shared
                 * by every instance of that struct, so the answer is keyed on the type
                 * instance (`planEnterInst` on the code generation side). */
                resolveDeferredCall(&c, cc, NULL, inst, &owner->typeParams, &inst->targs);
            }
        }
    }


    /* Deferred operator checks: re-check each recorded use for every concrete instance.
     * This is the price of having no traits. The error surfaces late, here, so the
     * message has to name the instance it came from. */
    for (size_t i = 0; i < c.opChecks.len; i++) {
        OpCheck *ec = *(OpCheck **)vecAt(&c.opChecks, i);
        /* The function holding the operator is never called, so none of its instances can
         * run and the per-instance recheck is unnecessary. This also spares the user from
         * having to define the operator for a type that is never used with it. */
        if (ec->func && !planUsed(ec->func)) continue;
        /* (1) Type instances (an operator used on a method's `T`). A free function has no
         * owner, so it has no type instances; it is covered by (2).
         *
         * The two branches are separate on purpose: a `goto done` when `ec->owner` is NULL
         * would skip (2) as well, leaving every `T: ==` inside a free generic function to
         * code generation, whose message happens to be word for word the same -- and which
         * cannot see the `%`-on-float case at all. So the loop boundary is written out here. */
        if (ec->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != ec->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runOpCheck(&c, ec, &ec->owner->typeParams, &inst->targs, inst->name);
            }
        }
        /* (2) Free-function instances: an operator used on `T` inside `fn f<T>`. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (ec->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runOpCheck(&c, ec, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    /* Deferred method calls on a type parameter: the same shape as the operator batch
     * above -- replayed once per concrete instance, with the instance named in the message.
     *
     * This is what makes a generic container able to do anything with its elements at all:
     * `hashMap<K, V>` hashes a `K`, which means calling `hash()` on a value whose type is a type
     * parameter. On the template there is no such method to find, so the call is recorded and
     * the instance decides. */
    for (size_t i = 0; i < c.methodChecks.len; i++) {
        MethodCheck *mc = *(MethodCheck **)vecAt(&c.methodChecks, i);
        /* A template that is never called has no instances, so nothing can be wrong yet. */
        if (mc->func && !planUsed(mc->func)) continue;
        /* (1) Type instances: a method call inside a generic type's method, where `T` comes
         * from the type arguments of the struct that owns it. */
        if (mc->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != mc->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runMethodCheck(&c, mc, &mc->owner->typeParams, &inst->targs, inst->name);
            }
        }
        /* (2) Free-function instances: a method call on `T` inside `fn f<T>`. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (mc->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runMethodCheck(&c, mc, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    for (size_t j = 0; j < c.funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
        FuncDef *tmpl = planTemplate(fi);
        if (!fi || !tmpl || !planIsCoro(tmpl) || planCoroFrameType(fi)) continue;
        if (provisionalInstance(&fi->targs)) continue;
        planSetIsCoro(fi, true);
        if (planYieldType(tmpl))
            planSetYieldType(fi, ttSubstitute(c.tt, planYieldType(tmpl),
                                              &tmpl->typeParams, &fi->targs));
        coroSetup(&c, fi);
        /* A local declaration's type inside a generic body is substituted only while the body is
         * **emitted** -- which happens after code generation has built its unit list and emitted the
         * struct definitions. A type instance that appears **only** as a local's type was therefore
         * never interned in time and its C struct was never emitted: `unknown type name 'pair_i64'`
         * on a local inside `mk<i64>`. Substituting the annotation here interns
         * it (`ttGeneric`), early enough for the unit list to see it. It is the same kind of fix as
         * the `coroSetup` re-run above: the instance existed, it just appeared too late. */
        coroFrameLay(&c, fi);
    }

    /* Every generic function **instance**: intern the type instances its body declares as local
     * types. The loop above is coroutine-only (it skips instances that already have a frame), and the
     * substitution that would normally intern such a type happens during **body emission** -- after
     * code generation built its unit list and emitted the struct definitions, so the struct never
     * appeared (`unknown type name 'pair_i64'`). */
    for (size_t j = 0; j < c.funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
        if (fi && planTemplate(fi)) internLocalTypes(&c, fi);
    }

    /* Deferred uses of such a call's result: same two branches, but the question is now
     * whether the instance's real return type fits the type the use site expected. */
    for (size_t i = 0; i < c.deferredUses.len; i++) {
        DeferredUse *du = *(DeferredUse **)vecAt(&c.deferredUses, i);
        if (du->func && !planUsed(du->func)) continue;
        if (du->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != du->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runDeferredUse(&c, du, &du->owner->typeParams, &inst->targs, inst->name);
            }
        }
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (du->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runDeferredUse(&c, du, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    /* Materialise every concrete instance's field types.
     *
     * A generic struct that holds another generic instance (`vals: pool<V>`) declares that field
     * with a parameter, so checking the template interns nothing: `pool<V>` is not concrete, and
     * correctly so. The concrete `pool<i32>` only comes into being when the field type is
     * substituted with the instance's arguments. Leaving that to code generation is too late: it
     * snapshots the instance table first, so the mangled call is emitted while the callee's
     * functions are not, and the C compiler reports
     * `implicit declaration of function 'pool$pool_i32_withCap'`.
     *
     * Substituting here is what interns it in time. It is also required for plain correctness:
     * the C struct emitted for `hashMap_i64Key_i32` has a `pool_i32` field, so that typedef has to
     * exist whether or not some other path noticed. The loop walks the table as it grows, so a
     * nested instance's own fields are materialised in turn. */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (!inst->sdef) continue;
        for (size_t j = 0; j < inst->sdef->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&inst->sdef->fields, j);
            (void)ttSubstitute(tt, fd->type, &inst->sdef->typeParams, &inst->targs);
        }
    }

    /* ------------------------------------------------- deferred generic rechecks
     *
     * The `==` batch was handled above; this is the reference-rule batch. On the
     * template, a value that mentions `T` was only recorded (`T` is opaque there), and
     * every concrete instance is now re-run with its substitution in place.
     *
     * Why it has to be done this way: `typeContainsRef(T)` can only answer false for an
     * opaque `T`, so `targetDepth` / `exprBorrowed` return early for it --
     *     struct boxT<T> {
     *         v: T
     *         fn stash(self: mut ref boxT<T>, value: T) { self.v = value }
     *     }
     * instantiated as `boxT<slice<u8>>`, that launders a view into a longer-lived object
     * => a dangling reference. The two-line conservative fix ("any `T` may carry a
     * reference") would also reject the textbook `box<T>::set`, which is entirely safe
     * for `T = i64`, so the check has to be done per instance. */
    for (size_t i = 0; i < c.refChecks.len; i++) {
        RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
        StructDef *owner = rc->func->owner;
        /* Free-function instances need the same recheck: same rule, with `T` coming
         * from the function's own parameter list. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (!refCheckApplies(rc, fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            c.substParams = &planTemplate(fi)->typeParams;
            c.substArgs   = &fi->targs;
            runRefCheck(&c, rc, planInstName(fi));
        }
        /* (2) Type instances, the original path: a method's `T` is fixed by the type
         * arguments of the struct that owns it. */
        if (!owner) continue;                    /* free function: handled by the loop above */
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;
            if (provisionalInstance(&inst->targs)) continue;

            c.substParams = &owner->typeParams;
            c.substArgs   = &inst->targs;
            runRefCheck(&c, rc, inst->name);
        }
    }

    /* ----------------------------- effect summaries, recomputed per instance
     *
     * On the template `T` is opaque, so the `carrier` test needs `mentionsParam`:
     * "anything mentioning `T` counts as carrying a reference". That charges the `Cont`
     * bit to instances such as `varArray<i32>` that carry no reference at all -- a loss
     * of precision, not a wrong answer.
     *
     * Here the summary is recomputed inside each instance's substitution context
     * (`c.substParams` / `c.substArgs` set). The `carrier` line goes through
     * `tsub(c, e->type)`, so `carrier` is false when `T = i32` and that bit stays clear,
     * while `T = slice<u8>` still sets it. Safety rests on the latter case.
     *
     * The `mentionsParam` clause cannot be deleted in place of this recompute: without it,
     * `tests/errors/generic_borrowed_store.extc` goes from rejected to
     * accepted, with a real dangling reference. That clause plugs a hole; it is not
     * redundant conservatism.
     *
     * The summary must be cleared before recomputing. `*in = *tmpl` is a shallow copy, so
     * an instance starts out holding the template's summary, and `callees` is copied from
     * the template as well (`collectEffects` does not reset a non-NULL `callees`).
     * Without the clear, the leaf count doubles and `effState` still claims to be
     * finished, so the transitive closure reads the template's stale bookkeeping. */
    for (size_t i = 0; i < c.funcInsts.len; i++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, i);
        if (!fi || !fi->body || fi->isExtern) continue;
        c.substParams = &planTemplate(fi)->typeParams;
        c.substArgs   = &fi->targs;
        anSetAddrMask(fi, 0);
        anSetContMask(fi, 0);
        anSetOtherMask(fi, 0);
        anSetHomeAddrMask(fi, 0);
        anSetHomeContMask(fi, 0);
        anSetAddrFromLocal(fi, false);
        anSetFreshCount(fi, 0);
        anSetEffState(fi, EFF_NONE);  anSetEffComplete(fi, false);  anSetEffUnknown(fi, false);
        vecInit(&fi->callees, c.arena, sizeof(FuncDef *));
        collectEffects(&c, fi);
    }
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        StructDef *sd = inst->sdef;
        if (!sd || sd->typeParams.len == 0) continue;
        for (size_t k = 0; k < sd->methods.len; k++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, k);
            if (!f || !f->body || f->isExtern) continue;
            c.substParams = &sd->typeParams;
            c.substArgs   = &inst->targs;
            anSetAddrMask(f, 0);
            anSetContMask(f, 0);
            anSetOtherMask(f, 0);
            anSetHomeAddrMask(f, 0);
            anSetHomeContMask(f, 0);
            anSetAddrFromLocal(f, false);
            anSetFreshCount(f, 0);
            anSetEffState(f, EFF_NONE);  anSetEffComplete(f, false);  anSetEffUnknown(f, false);
            vecInit(&f->callees, c.arena, sizeof(FuncDef *));
            collectEffects(&c, f);
        }
    }
    c.substParams = NULL;
    c.substArgs   = NULL;

    /* `@inline` is a requirement, not a hint, so a request that cannot be honoured has to
     * be refused here -- before code generation, and at the line the request was written on.
     *
     * The generated C asks for inlining with `always_inline`; a function that reaches
     * itself cannot be inlined, and the C compiler then fails with "inlining failed in call
     * to 'always_inline'" pointing into the generated file, which is no place to explain
     * anything. The call graph is complete by now: `collectEffects` has just filled in
     * `callees` for every function and instance.
     *
     * `funcReachesItself` already exists for the `@overwrite` cells and answers
     * conservatively -- a NULL callee, or a walk deeper than 64, answers yes -- which is the
     * direction wanted: refusing a request that might have worked costs a rewrite, while
     * accepting one that does not costs a compile error the user cannot act on. */
    {
        Vec inl; vecInit(&inl, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&inl) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&inl) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        for (size_t i = 0; i < inl.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&inl, i);
            if (!f || !f->isInline || !f->body) continue;
            if (!funcReachesItself(&c, f, 0)) continue;
            ckError(&c, f->line,
                    "Inlining is a requirement here, not a hint: the generated C asks the C"
                    " compiler for it, and a function that calls itself cannot be inlined."
                    " Make it a loop, or drop `@inline`.",
                    "`@inline` cannot be honoured: `%s` reaches itself, and a recursive"
                    " function cannot be inlined", f->name ? f->name : "?");
        }
    }

    /* ------------------------------- transitive closure of `needsHome`
     * Calling a function that needs a home arena (the arena the caller passes) means the caller
     * must have something to pass (`__extc_home`, or `&__extc_a[current block]`), so the caller
     * needs a home arena of its own. The rule and the iteration are `closeReach`; what is spelled
     * here is only which functions a round visits. */
    CloseCtx closeCtx = { m, tt, &c };
    closeReach(&closeCtx, REACH_HOME, roundNeedsHome);

    /* Single authority for arena levels: the checker hands codegen every decision it
     * needs, and codegen validates nothing on its own.
     *
     * While a body is being checked, `needsHome` has only the direct criterion (the body
     * contains a `new` and the return type carries a reference); the other half -- "the
     * function needs a home arena because it calls one that does" -- waits for the
     * transitive closure above. The `arenaLevel` computed at that point is therefore the
     * block level, while the generated C allocates as if the arena were a home arena. A
     * longer-lived arena is only safer, so this was not a hole, but it left two
     * authorities: an out-parameter allocation fell from the home arena back to a block
     * arena, which is what a golden test caught.
     *
     * Once the closure has run, every `new` in a function that needs a home arena is
     * rewritten to `ARENA_HOME`, so codegen's `arenaRefAt` consults only the number the
     * checker produced, never a per-function home flag. */
    {
        Vec all; allFunctions(arena, m, &all);
        /* An instance's arena sites are the template's arena sites.
         *
         * `funcInstance` copies the template shallowly (`*in = *tmpl`), which copies the
         * `arenaSites` *struct*: same backing array, but the length as it stood at that
         * moment. An instance is created at a call site, and the calling function may be
         * declared before the template, so the template's own body was often checked
         * **after** the copy - and `vecPush` on the template then moved a length the
         * instance never saw. The instance was left with an empty list, so the pass below
         * never gave it a home arena, while its body (the template's, shared) had already
         * been rewritten to read `(*__extc_home)`: `f_i32` in
         * `tests/arena-promoted/R2a_generic_instance_first.extc` took no `__extc_home`
         * parameter and gcc rejected the generated C. Re-point both at one list here, once
         * every body has been checked, and the whole final phase agrees by construction.
         *
         * Placing the same `Expr*` twice is harmless: every pass below is idempotent for a
         * site (the level it computes does not depend on how often it runs). */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (planTemplate(f)) anSetArenaSites(f, anArenaSites(planTemplate(f)));
        }

        /* Recompute the direct `needsHome` criterion for every function, uniformly.
         *
         * The criterion in `checkFunc` (the body allocates and the return type carries a
         * reference) is evaluated only while that body is being checked, and a generic
         * instance's body is never checked on its own (`checkFunc` returns early for an
         * instance, where `planTemplate(fx)` is non-NULL). An instance's `needsHome` is
         * therefore only the shallow copy made
         * when it was created (`*in = *tmpl`, see `funcInstance`), so it inherits from
         * whichever function was created first and the same program behaves differently
         * under a different declaration order:
         *   - caller first: the instance is created before its template is checked, so
         *     `needsHome` is false. Codegen then emits no `__extc_home` parameter while
         *     the template's `new` has already been rewritten to `(*__extc_home)`, and
         *     the generated C does not compile (gcc: `__extc_home` undeclared).
         *   - template first: everything works.
         *
         * So, after the closure above (all bodies checked, `arenaSites` final), recompute
         * it once for every function -- instances and methods included -- using the
         * substituted return type. Declaration order does not matter.
         *
         * Only the direct criterion is recomputed here; the "needs a home arena because it
         * calls one that does" half stays with the closure above. The recompute is
         * idempotent: it only sets the flag (`|=`), never clears it, so a result the
         * closure produced survives. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f->isExtern || !f->body) continue;
            Type *rt = f->ret;
            if (rt && f->typeParams.len > 0 && f->targs.len == f->typeParams.len)
                rt = ttSubstitute(c.tt, rt, &f->typeParams, &f->targs);
            if (stmtHasNew(f->body) &&
                ((rt && typeContainsRef(c.tt, rt)) || stmtStoresThroughDeref(&c, f->body, f)))
                anSetNeedsHome(f, true);
        }

        /* Recompute the escape-sensitive home-arena decision: the analysis writes last and
         * the checks only read.
         *
         * While a body was being checked, `computeEscapes` could not see the callee effect
         * summaries (the call-graph closure had not run yet), so `l` in `pushOne(l, 7)` was
         * not marked as escaping even though the later `sink(out, l)` publishes it. The home
         * arena then came out as the caller's own frame, so whatever the callee put into the
         * container was released as soon as this call returned.
         *
         * The closure has run now, so recompute the escape set (`computeEscapes` reads
         * `callsNeedsHome` and the summaries) and update `arenaArg` at those sites. */
        /* Run a few rounds (at most four). `computeEscapes` decides its own fixed point against the
         * `isEscapeeName` set as it stands at that moment, and the names it marks need other
         * call sites to propagate them, so one round is not enough. A few rounds push it to the
         * fixed point; there are few functions, so the cost is negligible, and `computeEscapes`
         * itself is capped at 32 rounds. */
        for (int round = 0; round < 4; round++) {
            bool changed = false;
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (f->isExtern || !f->body) continue;
                size_t before = c.escapees.len;
                computeEscapes(&c, f);
                if (c.escapees.len != before) changed = true;
            }
            if (!changed) break;
        }
        /* Decide on the union of the escape sets of all functions. `c.escapees` is the escape
         * set of the function currently being checked (it is rebuilt per function), while the
         * question asked here is whether a name can be moved out of the function it belongs
         * to, which is a different question.
         *
         * So take the union, conservatively: a name marked as escaping in any function counts
         * as possibly escaping. The direction is safe, since over-counting only makes the
         * home arena longer-lived: it costs memory but never misses an escape. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f->isExtern || !f->body) continue;
            computeEscapesMode(&c, f, true);   /* union mode: keeps what earlier functions added */
        }
        int eFixed = 0;
        for (size_t i = 0; i < c.eSites.len; i++) {
            EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
            if (!rec || !rec->call) continue;
            if (rec->overflow) {        /* args not all recorded => home arena (always sound) */
                planSetArenaArg(rec->call, ARENA_HOME);
                rec->call->arenaArgPending = false;
                continue;
            }
            /* The minimum depth over the recorded destinations, with `seen` to say whether a
             * value has been seen at all (INV-S).
             *
             * `nd == 0` must not double as "nothing seen yet" and as "this destination lives
             * outside the frame": depth 0 is the *shallowest* there is, so a loop keyed on
             * `nd` alone computes the **maximum** instead of the minimum -- `argDepth = {0, 1}`
             * comes out as 1, and the call hands over the caller's own block arena where it has to
             * hand over the home arena. The generated program then reads a block that has already
             * been released (heap-use-after-free). Two separate representations remove
             * the ambiguity -- no argument recorded leaves `nd` at 0, which still means "home
             * arena", the conservative answer. */
            /* 目的地有多浅：与 zone 通道**共用一份**实现（U4； 的哨兵 bug 曾在这里
             * 与 zone 通道各有一份）。`overflow` 已在上面的分支里 `continue` 处理掉了。*/
            int nd = callSiteMinDestDepth(&c, rec);
            /* Write the new homeDepth onto `arenaArg`, by the same rules as `setCallArenaArg`. */
            int want = (nd <= 0) ? ARENA_HOME : nd;
            /* 提权（`promoteInto2` 的调用结果那一格）可能已经把 arena 挪到**更长寿**的一侧：
             * 两边的要求取 min，别把它抬回来。`ARENA_HOME`(-1) 就是最长寿的一侧。 */
            {
                int cur = planArenaArg(rec->call);
                if (cur == ARENA_HOME || (cur >= 1 && cur < want)) want = cur;
            }
            if (planArenaArg(rec->call) != want) { planSetArenaArg(rec->call, want); eFixed++; }
            rec->call->arenaArgPending = false;
        }

        /* Long-running memory: replay the "stored at level k" fact table to a fixed point.
         *
         * Why repeat: one fact can only promote the sites it touches, and once those sites
         * are longer-lived, other facts can promote as well, so keep going until a round
         * changes nothing. It is the same iteration as the four rounds over `EArenaSite` and
         * the transitive closure of `needsHome`.
         *
         * Why that is correct: a site's final level is the smallest level among all the
         * constraints that reach it, and `promoteInto` only moves a level down, that is, it
         * only extends a lifetime, so replay is monotone and must converge on that minimum.
         * The 32-round cap is the same number as the cycle guard inside `promoteInto`. */
        int solveRounds = 0;
        c.lvlSolving = true;            /* no recording during replay: this really happened */
        size_t nFacts = c.lvlFacts.len;          /* snapshot: frozen while solving */
        for (int round = 0; round < 32; round++) {
            bool changed = false;
            for (size_t i = 0; i < nFacts; i++) {
                LvlFact *f = *(LvlFact **)vecAt(&c.lvlFacts, i);
                if (!f || !f->val) continue;
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[replay] kind=%d at=%d\n", (int)f->val->kind, f->at);
                if (promoteInto(&c, f->val, f->at)) changed = true;
            }
            solveRounds++;
            if (!changed) break;
        }
        c.lvlSolving = false;
        if (c.fxOn) { c.fxReplayRounds += solveRounds; c.fxReplayFacts += (long)nFacts; }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] %zu level facts, converged after %d round(s)\n",
                    c.lvlFacts.len, solveRounds);

        /* 路线 C：把「可能的环」的候选区域并到最浅的那一层。
         *
         * 必须在**求解之后、placement 之前**：求解给出每个站点最终的 `anMinAt`（合并的输入），
         * placement 再据此写 `arenaLevel`/`refDepth`（合并的输出）。作用域这时已全部关闭，所以
         * 这里只读站点指针、不再查名字。见 `CycleMerge` / `applyCycleMerges`。 */
        applyCycleMerges(&c);

        int fixed = 0, keptBlock = 0;
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind == EX_NEW || site->kind == EX_GENCALL) {
                    /* The solver has finished, so place the site as the single authority:
                     *   - `escaped` (some path that touched it demanded a lifetime beyond
                     *     this frame) => the home arena (the arena the caller chose);
                     *   - otherwise => the block level the solver produced, which inside a
                     *     loop means reclaimed every iteration.
                     *
                     * An unconditional "needs a home arena => home arena" fallback would pin
                     * per-iteration temporaries that never escape there until the frame
                     * ended. */
                    if (dbgOn("EXTC_DBG_MINAT"))
                        fprintf(stderr, "[minAt] %-8s line=%-4d minAt=%-3d arena=%d\n",
                                f->name ? f->name : "?", site->line, anMinAt(site), planArenaLevel(site));
                    if (dbgOn("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", anMinAt(site), anLexicalLevel(site),
                                planArenaLevel(site), anNeedsHome(f)?1:0, (int)site->kind);
                    if (dbgOn("EXTC_DBG_S3"))
                        fprintf(stderr, "[s3] %-8s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", anMinAt(site), anLexicalLevel(site),
                                planArenaLevel(site), anNeedsHome(f)?1:0, (int)site->kind);
                    int want;                            /* the arena it finally belongs to */
                    if (anMinAt(site) == 0) {
                        want = ARENA_HOME;              /* must outlive this frame => home arena */
                    } else if (anMinAt(site) >= 1) {
                        want = anMinAt(site);             /* must live to level k => block arena k */
                    } else {
                        /* No constraint touched it, so use its own level.
                         * `arenaLevel` cannot be consulted here: the provisional pass already
                         * rewrote every site in a function that needs a home arena to the
                         * not-yet-decided value. */
                        want = anLexicalLevel(site) >= 1 ? anLexicalLevel(site) : 1;
                    }
                    if (planArenaLevel(site) != want) {
                        planSetArenaLevel(site, want);
                        anSetRefDepth(site, arenaDepthOf(want));   /* the single conversion point */
                        if (want == ARENA_HOME) fixed++; else keptBlock++;
                    }
                } else if (site->arenaArgPending) {
                    /* A call site that hands an arena down is settled **after** the fixed
                     * point below, from the callee's `usesHome`, and deliberately not here:
                     * codegen appends the extra argument exactly when the callee's
                     * `usesHome` is set, so the value has to come from that same flag.
                     * Answering from the *owning* function's `needsHome` answers a
                     * different question (does this body have a home, not does the
                     * callee take one) and leaves the site with a block
                     * level: the caller then passes `&__extc_a[1]` while no such array is
                     * declared, and `tests/arena-promoted/R2a` stops compiling. */
                    (void)0;
                }
            }
        }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] decided: %d sites in the home arena, %d kept at block level\n", fixed, keptBlock);

    /* ------------------------- the precise question a signature needs
     * `usesHome` is the narrow counterpart of `needsHome`: the per-node rule (a site that really
     * sits in the home arena, or handing this function's home on to a callee) is in `bodyReaches`
     * under `REACH_USES_HOME`, and the closure is the same one every other property uses. */
    closeReach(&closeCtx, REACH_USES_HOME, roundUsesHome);


    /* Settle every call site that hands an arena down, now that `usesHome` is final.
     *
     * This is the one place the value is decided, and it is decided by the flag codegen
     * reads to decide *whether* to pass anything at all: a callee that takes a home
     * parameter gets the caller's own, `__extc_home`, which its caller handed to it. The
     * fixed point above guarantees the caller has one - a body that calls a function with
     * `usesHome` is marked itself - so no arena array has to be materialized here, and the
     * `__extc_a` a function declares stays tied to the sites that really allocate.
     *
     * Anything left pending is a callee that takes no home parameter, so its site passes
     * nothing and the field is not read. */
    for (size_t i = 0; i < all.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        Vec as_ = anArenaSites(f);
        for (size_t j = 0; j < as_.len; j++) {
            Expr *site = *(Expr **)vecAt(&as_, j);
            if (!site->arenaArgPending || !planCallee(site) || !planUsesHome(planCallee(site))) continue;
            planSetArenaArg(site, ARENA_HOME);
            site->arenaArgPending = false;
        }
    }

        /* Recompute every site's `refDepth` from its final level, whether or not the level
         * changed.
         *
         * Why: the
         * `anSetRefDepth(site, ...` in the placement loop above sits inside
         * `if (planArenaLevel(site) != want)`. For a site that the provisional pass had already
         * set to `ARENA_HOME` and the solver also placed in `ARENA_HOME`, the level did not
         * change, so that assignment never ran and `refDepth` kept a value that had been
         * damaged midway. Measured: the site of `alloc<i32>(1)` had `refDepth` 0, meaning
         * "must outlive this frame", while its level says 1, so the two contradicted each
         * other and the user saw "borrowed from depth 0", which makes no sense for an
         * allocation that lives in a block arena.
         *
         * The rule, and it was always the design rule: `refDepth` and `arenaLevel` must be
         * two ways of stating the same number, so sync them unconditionally after placement. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                anSetRefDepth(site, arenaDepthOf(planArenaLevel(site)));   /* keep the two in sync */
            }
        }
        /* `EXTC_DBG_HOME=1` prints, for every function, the two home flags and the arena
         * levels of its sites. Kept because the pair (`usesHome`, the levels of the very
         * sites it was computed from) is what has to agree for the generated C to compile,
         * and a mismatch is invisible in the source: the instance above took its parameter
         * from one answer and its body from the other. */
        if (dbgOn("EXTC_DBG_HOME"))
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (!f->body && !f->isExtern) continue;
                FuncDef *ft = planTemplate(f);
                const char *ftName = !ft ? "-" : (planInstName(ft) ? planInstName(ft) : ft->name);
                fprintf(stderr, "[home] %-24s uses=%d needs=%d tmpl=%-12s sites=%zu",
                        planInstName(f) ? planInstName(f) : f->name, (int)planUsesHome(f), (int)anNeedsHome(f),
                        ftName,
                        anArenaSites(f).len);
                Vec as_ = anArenaSites(f);
                for (size_t j = 0; j < as_.len; j++)
                    fprintf(stderr, " L%d", planArenaLevel(*(Expr **)vecAt(&as_, j)));
                fprintf(stderr, "\n");
            }

        /* Recompute the depths that were deferred until instantiation: they froze the value
         * from before the solver ran, and placement may have moved a site from the home arena
         * back down to a block level.
         *
         * Measured: the `return v` of `varArray<T>::withCap` froze `depth=1` in the template
         * round, while the `new T[cap]` inside it had correctly been placed in the home arena
         * (depth 0), so the instantiation re-check reported "depth 1, but this can only hold
         * up to 0" although the truth is 0.
         *
         * Recompute only shapes where `depthComesFromAlloc` holds, and only lower the value:
         * recomputing bindings would accept something like `generic_return_local`, which has
         * to be rejected. This was hit for real. */
        for (size_t i = 0; i < c.refChecks.len; i++) {
            RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
            if (!rc || !rc->val) continue;
            if (!depthComesFromAlloc2(&c, rc->val, 0)) continue;
            /* Recompute unconditionally, not only downwards: the value frozen before the
             * solver ran can be too low as well. In the template round the `refDepth` of
             * `new T[cap]` is still the not-yet-decided 0, and after solving it may be 1.
             * Only shapes where `depthComesFromAlloc` holds are touched, so bindings (derived
             * from a parameter or from a call result) are never relaxed. */
            rc->depth = solvedDepth(rc->val);
        }
        if (getenv("EXTC_DUMP_OW"))
            fprintf(stderr, "[arena] single source of truth: %d site(s) (`new` and call sites) placed in a home arena\n", fixed);
    }

    /* ---- Which storage an `@overwrite` cell gets, and how many sites it has ----
     * Must run after every function body has been checked: whether a function can reach
     * itself is decided from the call graph. */
    {
        Vec all; allFunctions(arena, m, &all);
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            planSetOwSites(f, countOwSites(f->body));
            /* Set `owLocal` even when there are no sites: otherwise the signature of a
             * function with no parameters and no home arena loses its `void` (a golden test
             * caught this immediately). */
            /* An `@overwrite` cell always lives in the current frame -- **for the functions this
             * pass sees**. It is not always true at code generation time: a generic instance is
             * created later, during codegen, and keeps the `false` it was born with. That is the
             * case the comment above is about (a no-parameter instance whose signature then carries
             * parameters instead of `void`), which is why the flag is still read in four places in
             * `codegen.c` -- and why `funcReachesItself` is not called here any more: the decision
             * it was meant to feed is a constant for this pass's functions. */
            planSetOwLocal(f, true);
        }
        if (getenv("EXTC_DUMP_OW"))
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (planOwSites(f)) fprintf(stderr, "[ow] %-18s sites=%d local=%d\n",
                                            f->name, planOwSites(f), (int)planOwLocal(f));
            }
    }

    /* ---- Compile-time optimization: does this function put anything into its own block
     * arena (`mayUseArena`)? Must be computed after the closure above has run. The test is
     * "the body has a `new`, or it calls a function that needs a home arena".
     *
     * When the answer is no, codegen drops the arena array and the release calls, which is
     * about 20% fewer lines of generated C (see the benchmark). `main` is always true: it is
     * the root home arena, and `__extc_home = &__extc_a[1]` needs that array. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f->isExtern || !f->body) { planSetMayUseArena(f, false); continue; } /* no body, no arena */
        planSetMayUseArena(f, stmtHasNew(f->body) || callsNeedsHome(f->body));
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            planSetMayUseArena(f, stmtHasNew(f->body) || callsNeedsHome(f->body));
        }
    }

    /* ---- Which functions can create a pool (`makesPool`)? ----
     *
     * Same shape as the `needsHome` closure above, and it has to be a closure for the same
     * reason: `vector<i32>::withCap` contains `extc_pool_new` and `withCap` calls `new`,
     * which calls `withCap`, so no single pass settles the answer. The rule
     *
     *     makesPool(f)  <=  f's body calls `extc_pool_new`
     *                    or f's body calls some g with makesPool(g)
     *
     * is monotone (a flag only ever goes from false to true) and the lattice is finite, so
     * iterating until a round changes nothing yields the *least* fixed point - which is the
     * answer wanted, not merely a consistent one: a larger fixed point would mark functions
     * that cannot create a pool and give back the hooks this pass exists to remove.
     *
     * Recursion and mutual recursion need no special case here, unlike the lazy
     * `funcAllocates` walk: a cycle simply stays false until one of its members is reached
     * from a body that really does call `extc_pool_new`, and if none of them ever is, then
     * none of them can create a pool at all.
     *
     * Three rounds: the plain functions, the methods of the non-generic structs, and -- because a
     * generic method is checked once per instance, with the protocol resolved for that instance --
     * the instances themselves. The rule and the iteration are `closeReach`; only the list of
     * functions a round visits is spelled here. */
    closeReach(&closeCtx, REACH_POOL, roundMakesPool);
    coroCheckDeferred(&c, m);
    /* 顺手把"这个结构体的某个方法建池"记到结构体上（见 StructDef.makesPoolAny）。 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (f && planMakesPool(f)) { anSetMakesPoolAny(sd, true); break; }
        }
    }
    /* ---- 调用点的 zone 层级：也要在 `makesPool` 闭包之后才能定 ----
     *
     * 与上面"电平求解再跑一遍"同一个病根、同一剂药：`zoneLevel` 决定"被调者的池分配活到
     * 第几层"，而**只有建池的被调者才会收到这个实参**；`makesPool` 是闭包 ⇒ 闭包之前它一律
     * 读作假，于是调用点停在临时层级上，跨两跳的链就会把自己的 place 传下去：
     *
     *     main → wrap → put → newString
     *
     * `wrap` 把**自己的** zone 传给了 `put`，`newString` 的缓冲因此生在 `wrap` 的 place 里，
     * `wrap` 一返回就被释放、下一次分配复用它 ⇒ ASan `heap-use-after-free`
     * （`tests/arena-soundness/H2_home_zone_two_hops.extc`）。
     *
     * 判据与 arena 完全同一个问题、同一个量：`nd` = 那个地方必须比被调者能存进去的每个
     * `mut ref` 实参（含接收者）活得更久；参数/全局算"本帧之外"（0），逃逸的局部量也算
     * （-1）。`ZONE_HOME` = "把我收到的 zone 一路传下去"。
     * 只往长里改（`<`），不往短里改：`zoneLevel` 从 `ZONE_HOME`（最长）向上是各层块号。 */
    for (size_t i = 0; i < c.eSites.len; i++) {
        EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
        if (!rec || !rec->call || !planCallee(rec->call)) continue;
        if (!planMakesPool(planCallee(rec->call)) || planZoneLevel(rec->call) == 0) continue;
        /* 目的地有多浅：与 arena 通道**共用一份**实现（U4）；`overflow` 由它在内部按
         * "活得最久"处理（两条通道的差别只剩这一点，现在写在共享函数里而不是两处代码里）。*/
        int nd = callSiteMinDestDepth(&c, rec);
        int zwant = (nd <= 0) ? ZONE_HOME : nd;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zfix] call=%s nd=%d lvl=%d -> %d\n",
                    planCallee(rec->call)->name ? planCallee(rec->call)->name : "-",
                    nd, planZoneLevel(rec->call), zwant);
        if (zwant < planZoneLevel(rec->call)) planSetZoneLevel(rec->call, zwant);
    }

    /* ---- 电平求解要再跑一遍 ----
     *
     * 第一遍跑在 `makesPool` 闭包**之前**（见上面那段求解），那时 `calleeMakesPool` 对所有
     * 调用点都还是假 ⇒ **池站点的提权一次都没落地**。
     * 闭包之后重放一次事实表，这次闸门读到的标志是真的；`promoteInto` 只把层级往小改，
     * 重放是单调的，所以多跑一遍不会引入新东西，只是把该落的提权落下去。 */
    c.lvlSolving = true;
    for (int round = 0; round < 32; round++) {
        bool changed2 = false;
        for (size_t i = 0; i < c.lvlFacts.len; i++) {
            LvlFact *f = *(LvlFact **)vecAt(&c.lvlFacts, i);
            if (!f || !f->val) continue;
            if (promoteInto(&c, f->val, f->at)) changed2 = true;
        }
        if (!changed2) break;
    }
    c.lvlSolving = false;

    if (getenv("EXTC_DUMP_POOL")) {
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            fprintf(stderr, "[pool] %-28s makesPool=%d mod=%s\n",
                    f->name ? f->name : "-", (int)planMakesPool(f),
                    f->modName && *f->modName ? f->modName : "-");
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                fprintf(stderr, "[pool] %s::%-22s makesPool=%d mod=%s\n", sd->name,
                        f->name ? f->name : "-", (int)planMakesPool(f),
                        f->modName && *f->modName ? f->modName : "-");
            }
        }
    }


    if (getenv("EXTC_SELFCHECK")) {
        int bad = 0;
        for (size_t i = 0; i < c.allSyms.len; i++) {
            Sym *sy = *(Sym **)vecAt(&c.allSyms, i);
            if (!sy) continue;
            /* Inequality, not equality: a reference may point at something that
             * outlives the site (`var cur: ?ref node = head`, where `head` lives
             * further out). Only claiming to be *shallower* than the site is wrong,
             * because that would mean pointing at storage that dies first.
             * The equality version was wrong and the escape-promotion example caught
             * it immediately, so the predicate itself needs checking too. */
            if (sy->origin && (sy->origin->kind == EX_NEW || sy->origin->kind == EX_GENCALL)) {
                int siteD = arenaDepthOf(planArenaLevel(sy->origin));
                if (anRefDepth(sy) < siteD) {
                    fprintf(stderr, "[selfcheck] binding `%s` has refDepth=%d, shallower than"
                            " its site (level %d => %d)\n", sy->name, anRefDepth(sy),
                            planArenaLevel(sy->origin), siteD);
                    bad++;
                }
            }
        }
        Vec allF; allFunctions(arena, m, &allF);
        for (size_t i = 0; i < allF.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&allF, i);
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                int want = arenaDepthOf(planArenaLevel(site));
                if (anRefDepth(site) != want) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: refDepth=%d but"
                            " arenaLevel=%d (should be %d)\n", site->line,
                            f->name ? f->name : "?", anRefDepth(site), planArenaLevel(site), want);
                    bad++;
                }
                if (anMinAt(site) == 0 && planArenaLevel(site) != ARENA_HOME) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: minAt=0 (must outlive"
                            " this frame) but it was not placed in the home arena\n",
                            site->line, f->name ? f->name : "?");
                    bad++;
                }
            }
        }
        if (bad)
            fprintf(stderr, "[selfcheck] %d consistency breaches (refDepth and arenaLevel"
                    " must describe the same fact)\n", bad);
        else if (getenv("EXTC_SELFCHECK_VERBOSE"))
            fprintf(stderr, "[selfcheck] all invariants hold\n");
        if (bad) ctx->hasError = true;      /* so scripts see the failure */
    }

    /* The full report, on request.
     *
     * Every level is final by now, so this is the one place that can print what the
     * checker knows about memory. The flag is read from the environment so that the driver
     * stays unaware of the report, the same arrangement `--dump-effects` uses. */
    if (getenv("EXTC_EXPLAIN_MEMORY") && getenv("EXTC_EXPLAIN_ROOT")
        && strcmp(getenv("EXTC_EXPLAIN_ROOT"), "1") == 0) {
        Vec allF; allFunctions(arena, m, &allF);
        reportMemory(&c, &allF);
    }

    /* `EXTC_DBG_FX=1`: what the analysis cost, in the quantities that can grow.
     *
     * The point of the split is that a scaling table can be read off it: `stores`/`lvlFacts`
     * are the **sizes** of the two fact tables, `storesSeen`/`symCmp`/`reachVisits` are what
     * the passes **walked** to fold them. A pass that is linear shows walked ~= size; the
     * front end being superlinear shows up as walked growing faster than size -- and which
     * of the three walked quantities it is tells which pass to look at. `own` is the share
     * of the walked records that belongs to the body being solved, so `seen/own` is the
     * ratio a per-body slice would remove. */
    if (c.fxOn) {
        size_t nBodies = 0, nEdges = 0;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (!f || !f->body) continue;
            nBodies++;
            nEdges += f->callees.len;
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (!f || !f->body) continue;
                nBodies++;
                nEdges += f->callees.len;
            }
        }
        fprintf(stderr, "[fx] sizes: bodies=%zu stores=%zu ownRecords=%ld lvlFacts=%zu"
                        " calleeEdges=%zu\n",
                nBodies, c.stores.len, c.fxTailRecords, c.lvlFacts.len, nEdges);
        fprintf(stderr, "[fx] levelpass: passes=%ld seen=%ld own=%ld altScan=%ld tblSum=%ld"
                        " tblMax=%ld\n",
                c.fxLvlPasses, c.fxStoresSeen, c.fxStoresOwn, c.fxAliasWalks,
                c.fxTblSum, c.fxTblMax);
        fprintf(stderr, "[fx] levelpass: rounds=%ld/%ld moved=%ld symCalls=%ld symCmp=%ld\n",
                c.fxLvlRounds1, c.fxLvlRounds2, c.fxLvlMoved, c.fxSymCalls, c.fxSymCmp);
        fprintf(stderr, "[fx] effects: calls=%ld cached=%ld edges=%ld\n",
                c.fxEffCalls, c.fxEffCached, c.fxEffEdges);
        fprintf(stderr, "[fx] reach: rounds=%ld visits=%ld replay=%ld rounds/%ld facts\n",
                c.fxReachRounds, c.fxReachVisits, c.fxReplayRounds, c.fxReplayFacts);
        fprintf(stderr, "[fx] stores: dedupSteps=%ld preBody=%ld\n",
                c.fxDedupSteps, c.fxPreBodyStores);
    }

    /* 区域无环判定：所有函数体与延迟调用点都落定之后才算，`planCallee` 那时才完整。
     * `EXTC_DBG_ACYCLIC=1` 时它自己打诊断。 */
    computeFnAcyclic(&c);

    return !ctx->hasError;
}

static bool stmtCallsAllocator(Checker *c, Stmt *s);   /* defined below; mutually recursive */
static bool allocInStmt(void *ctx, Stmt *s);
/**
 * @brief 判断函数是否直接或经由被调者分配。
 *
 * 与 `needsHome` 传递闭包问的是同一个问题，但必须惰性回答：闭包只在每个函数都检查完之后才跑，
 * 而检查自身函数体的调用方已经需要知道眼前这个调用会不会往容器里放新存储（`varArray::push`
 * 自己不分配，它调的 `grow` 分配）。结果缓存在 `FuncDef.allocState`；正在计算中的函数算作会
 * 分配，这是保守方向也是打破环的办法。
 *
 * @param c  checker。
 * @param f  要分类的函数；NULL 表示未知。
 * @return   函数体有 `new`、或被调者会分配时 true。
 */
static bool funcAllocates(Checker *c, FuncDef *f) {
    if (!f) return true;                     /* unknown, so assume it allocates */
    if (f->isExtern) return false;           /* an extern function does not touch our arenas */
    if (anAllocState(f) == 1) return true;
    if (anAllocState(f) == 2) return false;
    if (anAllocState(f) == 3) return true;     /* on the cycle being computed => conservative */
    anSetAllocState(f, 3);
    /* The same per-node criterion the closures use (`bodyReaches`), plus the callee half that only
     * this lazy path can afford to ask: it runs while a body is being checked, before the closures
     * exist, and it counts a function already on the stack as allocating, which is the
     * conservative direction for a question asked mid-check. */
    bool r = bodyReaches(f, REACH_ALLOC) || stmtCallsAllocator(c, f->body);
    anSetAllocState(f, r ? 1 : 2);
    return r;
}

typedef struct { Checker *c; } AllocCtx;

/**
 * @brief 判断求值这个表达式是否能到达分配器。
 *
 * `alloc<T>(n)` 与 `new` 一样是分配；调用按被调者是否可能分配算（`funcAllocates`，带记忆化，
 * 因为要递归调用图）；其余交给子节点。遍历走 `astWalkExprChildren`。
 *
 * @param ctx  `AllocCtx *`，携带 checker。
 * @param e    表达式。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool allocInExpr(void *ctx, Expr *e) {
    AllocCtx *a = (AllocCtx *)ctx;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) &&
        (!planCallee(e) || funcAllocates(a->c, planCallee(e)))) return false;   /* found */
    if (e->kind == EX_NEW || e->kind == EX_GENCALL) return false;   /* found */
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief `allocInExpr` 的语句版遍历回调。
 *
 * @param ctx  `AllocCtx *`，携带 checker。
 * @param s    语句。
 * @return     回调语义：找到返回 false 以停止遍历，否则返回 true。
 */
static bool allocInStmt(void *ctx, Stmt *s) {
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 判断语句里是否有调用可能到达分配器。
 *
 * 两个遍历函数互相递归并共用按函数的缓存，所以只有被调者检查过之后才有意义。
 *
 * @param c  checker。
 * @param s  语句；NULL 返回 false。
 * @return   会分配则 true。
 */
static bool stmtCallsAllocator(Checker *c, Stmt *s) {
    if (!s) return false;
    AllocCtx a = { c };
    return !allocInStmt(&a, s);
}

/**
 * @brief 为协程收尾：算出哪些协程需要自己的 place，并拒绝活过 `yield` 却携带外部存储的局部。
 *
 * 模板与实例都要算 `coroNeedsZone`：实例与模板共用函数体，漏算会让泛型协程的分配落进**调用
 * 者**的 zone，任务永不释放。
 *
 * @param c  checker。
 * @param m  模块。
 */
static void coroCheckDeferred(Checker *c, Module *m) {
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || planTemplate(cf)) continue;
        /* A boxed coroutine always has a task: its frame lives in that task's place. */
        planSetCoroNeedsZone(cf, planMakesPool(cf) || planCoroBoxed(cf));
    }
    /* **Instances** need the same answer, so the loop below repeats the computation for them:
     * skipping them leaves a generic coroutine without a place, so allocations made inside it land
     * in the *caller's* zone, and nothing ever releases the task (`live=20000` after 20000 finished
     * tasks; the same omission as `coroNeedsZone` not being computed for an instance).
     * An instance shares its template's body, so the template's freshly computed answer is the
     * right one -- and a coercion records `coroBoxed` on whichever function owns the frame. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || !planTemplate(cf)) continue;
        planSetCoroNeedsZone(cf, planCoroNeedsZone(planTemplate(cf)) || planCoroBoxed(cf));
    }
    for (size_t i = 0; i < c->coroDeferred.len; i++) {
        CoroDeferred *d = *(CoroDeferred **)vecAt(&c->coroDeferred, i);
        /* Two shapes a reference may legally outlive a suspension in:
         *   - a pooled container: the pool has a place of its own (the older exception);
         *   - a `new` written **in this coroutine's own body**: its storage is the frame's block arena
         *     (`arenaRefAt` puts every block level in the frame), which lives as long as the task does.
         * Everything else -- a parameter, a `ref` into the caller, a view handed in -- still gets the
         * error below, because that storage really is gone when the step returns. */
        /* A third shape joins the two above: storage that **provably outlives every frame**.
         * A global, a `effects Ret=0` result, and a **string literal** (static storage) are all
         * outside the frame by construction -- `exprOutOfFrame` is the predicate that already
         * answers that question, so the rule generalizes instead of growing a case list.
         * (Measured cost of not having it: a literal slice bound with `let` could not cross a
         * `yield`, so the workaround was to inline the literal at every call site.) */
        if (d->init && (exprMakesPool(d->init, true) || d->init->kind == EX_NEW ||
                        d->outOfFrame || exprOutOfFrame(c, d->init))) continue;
        ckError(c, d->line,
                "A local that lives across a `yield` may not point at someone else's storage: the C"
                " stack of this call is gone when the coroutine yields. Two fixes: allocate it"
                " yourself in this body (`new` lands in the frame's own arena, a pooled container in"
                " the pool's place), or keep the offset and take the view again after the resume"
                "",
                "`%s` lives across a `yield` and carries a reference", d->cname);
    }
    c->coroDeferred.len = 0;
}

/**
 * @brief 判断 codegen 是否会用 place 把这条语句包起来
 *        （`zoneEnter`/`zoneLeaveTo` + `arena_release`）。
 *
 * checker 需要的是**保守超集**：对会被 codegen 包起来的块答“否”，会让 `yield` 落在已回收的块
 * 里，所以它只问手上已有的两个谓词；codegen 自己的判断更细，更细在这里是安全的。
 *
 * @param s       语句；NULL 返回 false。
 * @param inCoro  语句在协程体内时为 true，此时恒返回 false。
 * @return        需要 place 边界时 true。
 */
bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro) {
    if (!s) return false;
    /* A coroutine body has no place boundary, for two reasons:
     *   - block arenas live **in the frame** (`arenaRefAt`), so an allocating block survives a
     *     suspension and its own exit still reclaims it;
     *   - a pool's storage is the **task's place**, not the block's -- `tests/coro/coro_pool.extc`
     *     has been creating a vector at body level, growing it across three suspensions and staying
     *     ASan-clean since before this rule existed; the zone leave in a step goes through the
     *     "current top" fallback, so the stack-local mark is never read after a resume.
     * So `placeBoundaryDepth` stays 0 inside a coroutine and the check in the `ST_YIELD` case cannot
     * fire there. It stays in the code because it is the invariant that would have to come back the
     * day a suspension can sit somewhere its storage does not survive. */
    if (inCoro) return false;
    return stmtHasNew(s) || stmtMakesPool(s, true);
}

/* ---- the frame of a coroutine -------------------------------
 *
 * Which locals live in the frame? The ones **live across a `yield`**: declared before it and used
 * after it. One walk answers both questions the design needs:
 *
 *   1. rule 2 -- such a local may not carry a reference (a view, a `ref`, a container): its storage
 *      is either the C stack of this very call, which is gone the moment the coroutine yields, or an
 *      arena that dies before the task does. The rejection message carries the two fixes.
 *   2. the frame layout codegen needs (slice B): those locals by value, plus `pc`, the return slot,
 *      and the zone id when the body can create a pool.
 *
 * The walk is the authoritative one (`astWalkStmtChildren` / `astWalkExprChildren`), so a new kind
 * cannot be forgotten here, and its pre-order visit order is what "before" and "after" mean.
 *
 * **Loop-back edges** make "after" more than source order: a use that sits inside a loop which also
 * encloses the `yield` can run again *after* the resume, even when the textual use comes first --
 * `while c { use(a)  yield x }` uses `a` again on the next iteration. So a local counts as live
 * across when the use and the yield **share an enclosing loop** (the loop chain is recorded per node;
 * deeper than the chain array means "assume shared", which is the conservative direction). Getting
 * this wrong the other way would let a dangling view through silently, which is exactly what these
 * rounds exist to prevent.
 */
#define CORO_CHAIN_MAX 8
typedef struct {
    const char *cname;
    int         index;
    int         chain[CORO_CHAIN_MAX];      /* the enclosing loop ids, outermost first */
    int         nchain;
} CoroUse;
typedef struct {
    const char *cname;
    Type       *type;
    Expr       *init;
    int         index;
    /* A `global` declaration's storage is not in any frame, so it must **not** go into the
     * frame: its C form is a block-scope `static` that already lives to the end of the process.
     * Putting it in the frame would make every resume read and write a frame field instead. */
    /* The statement itself, so the layout pass can **read** the syntax facts it needs
     * (`st->u.var.isGlobal`) and the plan results (`planVarInitOutOfFrame(st)`) instead of
     * copying them into this descriptor -- one source of truth, and no field name that the
     * AST-freeze scan has to tell apart from an AST node member. */
    const Stmt *st;
} CoroDecl;
typedef struct {
    int index;
    int line;
    int chain[CORO_CHAIN_MAX];
    int nchain;
} CoroYield;
typedef struct {
    AstVisit v;                 /* so the callbacks can share one context */
    int      index;
    int      chain[CORO_CHAIN_MAX];
    int      nchain;
    int      nextLoop;
    Vec      decls;             /* CoroDecl*  */
    Vec      uses;              /* CoroUse*   */
    Vec      yields;            /* CoroYield* */
} CoroScan;

/**
 * @brief 把循环 id 压进当前循环链。
 *
 * 链条过深时置哨兵值，`coroSharesLoop` 便答“共享”。
 *
 * @param s   扫描状态。
 * @param id  循环 id。
 */
static void coroPushLoop(CoroScan *s, int id) {
    if (s->nchain < CORO_CHAIN_MAX) s->chain[s->nchain++] = id;
    else s->nchain = CORO_CHAIN_MAX + 1;      /* too deep: `coroSharesLoop` then says yes */
}
/**
 * @brief 弹出当前循环链的栈顶。
 *
 * @param s  扫描状态。
 */
static void coroPopLoop(CoroScan *s) {
    if (s->nchain == CORO_CHAIN_MAX + 1) s->nchain = CORO_CHAIN_MAX;
    else if (s->nchain > 0) s->nchain--;
}
/**
 * @brief 复制当前的循环链。
 *
 * @param s      扫描状态。
 * @param chain  输出缓冲，至少 `CORO_CHAIN_MAX` 个 int。
 * @param n      输出：实际复制了多少个。
 */
static void coroCopyChain(CoroScan *s, int *chain, int *n) {
    *n = s->nchain > CORO_CHAIN_MAX ? CORO_CHAIN_MAX : s->nchain;
    for (int i = 0; i < *n; i++) chain[i] = s->chain[i];
}
/**
 * @brief 判断两个节点是否共享外层循环。
 *
 * 链长超过 `CORO_CHAIN_MAX` 表示“假定共享”。
 *
 * @param ac  第一个节点的循环链。
 * @param an  第一个节点链长。
 * @param bc  第二个节点的循环链。
 * @param bn  第二个节点链长。
 * @return    有共同循环 id 时 true。
 */
static bool coroSharesLoop(const int *ac, int an, const int *bc, int bn) {
    for (int i = 0; i < an; i++)
        for (int j = 0; j < bn; j++)
            if (ac[i] == bc[j]) return true;
    return false;
}

static bool coroScanStmt(void *ctx, Stmt *s);
static bool coroScanExpr(void *ctx, Expr *e);

/**
 * @brief 扫描回调：记录表达式里对绑定的读取及其循环链。
 *
 * @param ctx  `CoroScan *`。
 * @param e    表达式。
 * @return     继续遍历返回 true。
 */
static bool coroScanExpr(void *ctx, Expr *e) {
    CoroScan *s = (CoroScan *)ctx;
    if (e->kind == EX_IDENT && planCName(e)) {
        CoroUse *u = (CoroUse *)vecPush(&s->uses);   /* by value: never a pointer into the vec */
        u->cname = planCName(e);
        u->index = s->index++;
        coroCopyChain(s, u->chain, &u->nchain);
    } else {
        s->index++;
    }
    AstVisit v = { coroScanExpr, coroScanStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief 扫描回调：记录局部声明与 `yield`，并维护循环链。
 *
 * @param ctx  `CoroScan *`。
 * @param s    语句。
 * @return     继续遍历返回 true。
 */
static bool coroScanStmt(void *ctx, Stmt *s) {
    CoroScan *sc = (CoroScan *)ctx;
    if (s->kind == ST_VAR && planCName(s)) {
        CoroDecl *d = (CoroDecl *)vecPush(&sc->decls);   /* by value */
        d->cname = planCName(s);
        d->type = s->type;
        d->init = s->u.var.init;
        d->st = s;
        d->index = sc->index++;
    } else if (s->kind == ST_YIELD) {
        CoroYield *y = (CoroYield *)vecPush(&sc->yields);
        y->index = sc->index++;
        y->line = s->line;
        coroCopyChain(sc, y->chain, &y->nchain);
    } else {
        sc->index++;
    }
    const bool loop = s->kind == ST_WHILE;
    if (loop) coroPushLoop(sc, sc->nextLoop++);
    AstVisit v = { coroScanExpr, coroScanStmt, ctx };
    bool ok = astWalkStmtChildren(s, &v);
    if (loop) coroPopLoop(sc);
    return ok;
}

/**
 * @brief 判断声明 `d` 是否活过 yield `y`。
 *
 * @param s  扫描状态。
 * @param d  局部声明。
 * @param y  yield 站点。
 * @return   在 `y` 之后被使用、或与它共享循环时 true。
 */
static bool coroLiveAcross(const CoroScan *s, const CoroDecl *d, const CoroYield *y) {
    for (size_t i = 0; i < s->uses.len; i++) {
        const CoroUse *u = (const CoroUse *)vecAt((Vec *)&s->uses, i);
        if (!u->cname || strcmp(u->cname, d->cname) != 0) continue;
        if (u->index > y->index) return true;
        if (u->index > d->index && coroSharesLoop(u->chain, u->nchain, y->chain, y->nchain))
            return true;
    }
    return false;
}

/**
 * @brief 排出协程的帧，并拒绝不能放进帧里的东西。
 *
 * 在函数体检查完之后跑，此时每个声明的类型都已定稿。扫描结果按值存进向量：向量在填充过程中
 * 会增长，持有元素指针会立刻悬空。
 *
 * @param c  checker。
 * @param f  协程函数；不是协程或没有函数体时直接返回。
 */
static void coroFrameLay(Checker *c, FuncDef *f) {
    if (!f || !planIsCoro(f) || !f->body) return;
    CoroScan s;
    memset(&s, 0, sizeof s);
    s.index = 0;
    /* Stored **by value**: these vectors grow while they are being filled, so holding pointers to
     * their elements would leave dangling pointers the moment one of them reallocates. */
    vecInit(&s.decls, c->arena, sizeof(CoroDecl));
    vecInit(&s.uses, c->arena, sizeof(CoroUse));
    vecInit(&s.yields, c->arena, sizeof(CoroYield));
    CoroScan *sp = &s;
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        coroScanStmt(sp, *(Stmt **)vecAt(&f->body->u.block.stmts, i));
    if (s.yields.len == 0) return;                  /* nothing to lay out */

    /* The frame holds the **parameters** too: a resume has no arguments to pass them again, so they
     * are captured at spawn and read back on every step. */
    vecInit(&f->coroFrame, c->arena, sizeof(Param));   /* by value, like the scan above */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *src = *(Param **)vecAt(&f->params, i);
        Param *p = (Param *)vecPush(&f->coroFrame);
        *p = *src;
    }
    for (size_t i = 0; i < s.decls.len; i++) {
        const CoroDecl *d = (const CoroDecl *)vecAt(&s.decls, i);
        bool live = false;
        int line = f->body->line;
        for (size_t k = 0; k < s.yields.len && !live; k++) {
            const CoroYield *y = (const CoroYield *)vecAt(&s.yields, k);
            if (d->index < y->index && coroLiveAcross(&s, d, y)) { live = true; line = y->line; }
        }
        if (!live) continue;                        /* stays an ordinary C local ✓ */
        if (d->st && d->st->u.var.isGlobal) continue;   /* `global`: storage is already out of frame ✓ */
        if (typeContainsRef(c->tt, d->type)) {
            /* Safe or not depends on who owns the storage, which the pool fixpoint only settles
             * later; the decision is made in `coroCheckDeferred`. **No `continue`**: the local still
             * belongs in the frame (a resume jumps past its declaration). */
            if (!c->coroDeferred.arena)
                vecInit(&c->coroDeferred, c->arena, sizeof(CoroDeferred *));
            CoroDeferred *dl = arenaAllocZero(c->arena, sizeof *dl);
            dl->cname = d->cname; dl->init = d->init; dl->line = line;
            dl->outOfFrame = d->st ? planVarInitOutOfFrame(d->st) : false;
            *(CoroDeferred **)vecPush(&c->coroDeferred) = dl;
        }
        if (0 && typeContainsRef(c->tt, d->type)) {
            ckError(c, line,
                    "A local that lives across a `yield` may not carry a reference: its storage is"
                    " gone when the coroutine resumes (the C stack of this call is, and a block's"
                    " arena is). Two fixes: allocate it yourself with `new` so it lands in the"
                    " task's own place, or keep the offset and take the view again after the"
                    " resume",
                    "`%s` lives across a `yield` and carries a reference", d->cname);
            continue;
        }
        Param *p = (Param *)vecPush(&f->coroFrame);
        p->name = d->cname;
        p->cname = d->cname;
        /* The frame is laid out from the **body's** recorded declarations, and for an instance that
         * body is still the template's -- so a local declared `var b: box<T>` kept the parameter
         * spelling in the frame: the instance's frame had `box_i64 ret` next to `box_T b` and the C
         * said "incompatible types when assigning to type 'box_i64' from type 'box_T'"). Substitute with the instance's arguments here; the template keeps its
         * own spelling, which is what `ttSubstitute` returns unchanged when there is no template. */
        p->type = (planTemplate(f) && f->targs.len)
                    ? ttSubstitute(c->tt, d->type, &planTemplate(f)->typeParams, &f->targs)
                    : d->type;
        p->line = f->body->line;
    }
    /* The frame is a **real type**, with real fields: the units pass scans a struct's fields to
     * find the instances its storage needs, and a `vector<i32>` living in the frame is exactly how a
     * coroutine pulls in that container's helpers. `zone` is always present -- eight bytes, and it
     * keeps the layout independent of the pool fixpoint, which only runs later. */
    {
        StructDef *fsd = planCoroFrameType(f) ? planCoroFrameType(f)->sdef : NULL;
        if (fsd) {
            if (!fsd->fields.arena) vecInit(&fsd->fields, c->arena, sizeof(FieldDef *));
            Type *i64t = ttFromName(c->tt, "i64");
            FieldDef *fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "pc"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "ret"; fd->type = planYieldType(f); fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "zone"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The task id: the table owns the place, and the step reads the zone beside it rather
             * than re-deriving it (rule 1). See src/coroutine.c. */
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "task"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The **incoming slot**: what a resume hands the coroutine, read by a `yield` binding. */
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "in"; fd->type = planYieldType(f); fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The block arenas, one per block level. A step has no C-stack arena array, and a block in
             * a coroutine can span a suspension (`new` ... `yield` ... use), so the arena lives here
             * and the block's release point still reclaims it. `cType` renders the synthesized field
             * type verbatim, so the generated struct says `extc_arena arena1;` and friends. */
            {
                int maxLv = 1 + blkMaxOfBlock(f->body);
                for (int lv = 1; lv <= maxLv; lv++) {
                    Type *at = arenaAllocZero(c->arena, sizeof *at);
                    at->kind = TY_STRUCT;
                    at->name = "extc_arena";
                    FieldDef *af = arenaAllocZero(c->arena, sizeof *af);
                    af->name = arenaPrintf(c->arena, "arena%d", lv);
                    af->type = at;
                    af->line = f->line;
                    *(FieldDef **)vecPush(&fsd->fields) = af;
                }
            }
            /* `yield` bindings recorded while the body was checked: frame locals like any other. */
            for (size_t i = 0; i < c->coroBinds.len; i++) {
                CoroBind *cb = *(CoroBind **)vecAt(&c->coroBinds, i);
                if (cb->fn != f) continue;
                bool dup = false;
                for (size_t k = 0; k < f->coroFrame.len && !dup; k++) {
                    const Param *pp = (const Param *)vecAt(&f->coroFrame, k);
                    dup = pp->name && strcmp(pp->name, cb->name) == 0;
                }
                if (dup) continue;
                Param bp;
                bp.name = bp.cname = cb->name;
                bp.type = cb->type;
                bp.line = cb->line;
                *(Param *)vecPush(&f->coroFrame) = bp;
            }
            for (size_t k = 0; k < f->coroFrame.len; k++) {
                const Param *p = (const Param *)vecAt(&f->coroFrame, k);
                fd = arenaAllocZero(c->arena, sizeof *fd);
                fd->name = p->name; fd->type = p->type; fd->line = f->line;
                *(FieldDef **)vecPush(&fsd->fields) = fd;
            }
            if (!fsd->type) fsd->type = planCoroFrameType(f);
        }
    }

    if (getenv("EXTC_DBG_CORO")) {
        fprintf(stderr, "[coro] %s: frame = pc, ret%s", f->name,
                planMakesPool(f) ? ", zone" : "");
        for (size_t i = 0; i < f->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt(&f->coroFrame, i);
            fprintf(stderr, ", %s: %s", p->name, typeStr(c, p->type));
        }
        fprintf(stderr, "   (%zu fields: %zu param%s + %zu live-across-yield local%s)\n",
                f->coroFrame.len, f->params.len, f->params.len == 1 ? "" : "s",
                f->coroFrame.len - f->params.len,
                f->coroFrame.len - f->params.len == 1 ? "" : "s");
    }
}

/* The pool questions, shared with code generation.
 *
 * `stmtMakesPool` / `exprMakesPool` walk a tree asking "can this reach `extc_pool_new`?" --
 * codegen asks the block-local form to decide whether a block needs a zone bracket, the
 * checker asks the transitive form for the `makesPool` closure. One spelling, two askers
 * (review F11).
 *
 * **The resolver hook is the checker's extension point, and it is the reason this is not in
 * `typelayer.c`**: a generic body's protocol method on a *type parameter* deliberately has
 * no callee recorded (one template body, many instances), so the pure AST answer is "no",
 * while the conservative answer is "yes". The checker installs a resolver that substitutes
 * the instance's type arguments; codegen gets the resolved callee from `planCallee`. With no
 * resolver installed the answer stays conservative. */
/* 解析钩子；默认没有，判定保持保守。 */
static FuncDef *(*poolCalleeResolve)(Expr *e) = NULL;
/**
 * @brief 装一个被调用者解析钩子：让池新建判定在模板实例上问到具体被调者。
 *
 * checker 这一侧问不到“某个实例的具体方法是谁”：一个模板体、多个实例，纯 AST 答案永远是
 * “否”，而保守答案是“是”。装上钩子后按实例的类型实参代换；没装时保持保守。
 *
 * @param fn  解析函数；传 `NULL` 回到保守行为。
 */
void setPoolCalleeResolver(FuncDef *(*fn)(Expr *e)) { poolCalleeResolve = fn; }

/**
 * @brief 判断“这一次调用能不能走到 `extc_pool_new`”。
 *
 * 与 `calleeMakesPool` 问的不是同一个问题：那一个问“结果会不会住在池上”（逃逸与提权），对
 * `@poolObject` 类型一律放宽；块级的 zone 钩子只需要“这次会不会建池”。未解析的被调者保守为
 * 真。
 *
 * @param f  被调者；NULL（未解析）返回 true。
 * @return   这次调用可能建池时 true。
 */
bool calleeCreatesPool(FuncDef *f) {
    if (!f) return true;                       /* unresolved => conservative */
    if (planMakesPool(f)) return true;
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return anIsAssoc(f) && f->owner && anMakesPoolAny(f->owner);
}


static bool exprMakesPool(Expr *e, bool descendBlocks);

/**
 * @brief 判断语句里是否有表达式可能创建池。
 *
 * @param s              语句；NULL 返回 false。
 * @param descendBlocks  是否走进嵌套块与 domain：它们是独立的 place、有自己的 zone，块级问题
 *                       在那里停止。
 * @return               可能创建池时 true。
 */
bool stmtMakesPool(Stmt *s, bool descendBlocks) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMakesPool(s->u.yield_.value, descendBlocks);
    case ST_TRAP:   return exprMakesPool(s->u.trap_.msg, descendBlocks);
    case ST_VAR:    return exprMakesPool(s->u.var.init, descendBlocks);
    case ST_ASSIGN: return exprMakesPool(s->u.assign.value, descendBlocks) ||
                           exprMakesPool(s->u.assign.target, descendBlocks);
    case ST_IF:     return exprMakesPool(s->u.ifs.cond, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.thenBody, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.elseBody, descendBlocks);
    case ST_DOMAIN:
        /* A domain block is a place of its own, exactly like a block: what happens inside is that
         * block's business, and the transitive question walks in. */
        if (!descendBlocks) return false;
        return exprMakesPool(s->u.domain_.callee, descendBlocks) ||
               stmtMakesPool(s->u.domain_.body, descendBlocks);
    case ST_WHILE:  return exprMakesPool(s->u.whiles.cond, descendBlocks) ||
                           stmtMakesPool(s->u.whiles.body, descendBlocks);
    case ST_RETURN: return exprMakesPool(s->u.ret.value, descendBlocks);
    case ST_EXPR:   return exprMakesPool(s->u.expr.expr, descendBlocks);
    case ST_BLOCK:
        /* A block is a `place` of its own with its own zone, so the block-local question
         * stops here: whatever happens inside is that block's business. The transitive
         * question walks in. */
        if (!descendBlocks) return false;
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtMakesPool(*(Stmt **)vecAt(&s->u.block.stmts, i), descendBlocks)) return true;
        return false;
    case ST_MATCH:
        if (exprMakesPool(s->u.match.scrutinee, descendBlocks)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtMakesPool((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, descendBlocks)) return true;
        return false;
    default: return false;
    }
}

/**
 * @brief 判断表达式是否可能创建池。
 *
 * 解析后的三种调用形状都要查（含 `EX_ASSOC`）。`flush` / `print` / `println` 是按名字分派的
 * 内建，checker 没有为它们解析被调者，必须在这里点名排除，否则保守分支会把 `makesPool` 扩散
 * 到半个库。
 *
 * @param e              表达式；NULL 返回 false。
 * @param descendBlocks  是否走进嵌套块与 domain。
 * @return               可能创建池时 true。
 */
static bool exprMakesPool(Expr *e, bool descendBlocks) {
    if (!e) return false;
    /* The three shapes a resolved call takes. `EX_ASSOC` belongs here for the reason its
     * counterpart in `exprCallsNeedsHome` documents: an associated function records its
     * callee through `planCallee` too, and leaving it out is how a summary quietly becomes wrong. */
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        /* `flush()` / `print(x)` / `println(x)` are builtins dispatched by name, and the
         * checker leaves `e->func` null for them (it returns before resolving a callee).
         * They cannot create a pool; naming them here keeps the conservative branch below
         * from spreading `makesPool` over half the library. Measured before this case
         * existed: `io::flushOut`, `io::writeBytesRaw` and `io::errWrite` were all marked,
         * purely because each of them calls `flush()`. */
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
            const char *bn = e->u.call.callee->u.ident.name;
            if (bn && (strcmp(bn, "flush") == 0 || strcmp(bn, "print") == 0 ||
                       strcmp(bn, "println") == 0))
                return false;
        }
        /* 这里问的是"**这一次调用能不能走到 `extc_pool_new`**"（zone 的决定，以及
         * `makesPool` 闭包本身），不是"结果会不会住在池上" —— 后者是 `calleeMakesPool`
         * 的问题（逃逸与提权那两处用它）。两者的差别见 `calleeCreatesPool`。
         *
         * 泛型体里对**类型参数**的协议方法（`k.hash()`）在节点上**故意没有 func**，
         * 所以这一侧还要问解析钩子（由 codegen / 闭包的实例那一轮装上）。 */
        FuncDef *cf = planCallee(e);
        if (!cf && e->kind == EX_METHOD && poolCalleeResolve) cf = poolCalleeResolve(e);
        if (calleeCreatesPool(cf)) return true;
    }
    switch (e->kind) {
    case EX_BIN: return exprMakesPool(e->u.bin.left, descendBlocks) ||
                       exprMakesPool(e->u.bin.right, descendBlocks);
    case EX_UN:  return exprMakesPool(e->u.un.operand, descendBlocks);
    case EX_REF: return exprMakesPool(e->u.ref.operand, descendBlocks);
    case EX_EXT: return exprMakesPool(e->u.ext_.call, descendBlocks);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprMakesPool(e->u.deref.operand, descendBlocks);
    case EX_SIGN:  return exprMakesPool(e->u.sign.operand, descendBlocks);
    case EX_CONV:  return exprMakesPool(e->u.conv.operand, descendBlocks);
    case EX_TRY:   return exprMakesPool(e->u.try_.operand, descendBlocks);
    case EX_INDEX: return exprMakesPool(e->u.index.obj, descendBlocks) ||
                          exprMakesPool(e->u.index.index, descendBlocks);
    /* The bounds are expressions too, and every walker in this file that forgot them had a
     * real defect (see the note on `exprUsesCname`); a call in a bound can create a pool. */
    case EX_SLICE: return exprMakesPool(e->u.slice.obj, descendBlocks) ||
                          exprMakesPool(e->u.slice.lo, descendBlocks) ||
                          exprMakesPool(e->u.slice.hi, descendBlocks);
    case EX_FIELD: return exprMakesPool(e->u.field.obj, descendBlocks);
    case EX_COALESCE:
        return exprMakesPool(e->u.coalesce.main, descendBlocks) ||
               exprMakesPool(e->u.coalesce.fallback, descendBlocks);
    case EX_METHOD: {
        if (exprMakesPool(e->u.method.recv, descendBlocks)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.method.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_CALL: {
        /* The callee as well: `f()()` is not legal today, but the walk costs nothing and a
         * shape left out is exactly how a summary goes wrong. */
        if (exprMakesPool(e->u.call.callee, descendBlocks)) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.call.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.assoc.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_NEW: return exprMakesPool(e->u.new_.count, descendBlocks);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprMakesPool((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, descendBlocks)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprMakesPool((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value,
                              descendBlocks)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.arraylit.elems, i), descendBlocks)) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.enumval.args, i), descendBlocks)) return true;
        return false;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.gencall.args, i), descendBlocks)) return true;
        return false;
    /* `dyn Trait(x)` **is** a pool site: `extc_dyn_put` lazily builds an object table in the zone it
     * is handed, and that table holds the copied payload. So the block that constructs a `dyn` value
     * needs a zone of its own -- otherwise the table (and every payload in it) hangs off the enclosing
     * function's zone and cannot be dropped at the end of the block or of the iteration.
     *
     * The payload walk is subsumed by `true`: whatever the payload builds, the enclosing block needs a
     * zone for this value anyway. */
    case EX_DYN: return true;
    default: return false;
    }
}
