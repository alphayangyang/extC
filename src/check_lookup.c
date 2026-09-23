/* Name lookup and non-null narrowing for `?ref T`.
 *
 * Resolving a name to a binding lives here, together with the facts a comparison against
 * null establishes and the small predicates the rest of the checker is built from.
 */

#include <string.h>
#include "check_internal.h"

/* ----------------------------------------------- non-null narrowing for `?ref T` */
/* Resolve a name to a binding, innermost scope first and module-level bindings last. */
/* Defer the size check of `new T[n]` to instantiation, for a body whose type parameter
 * has no size yet. */
/* True when the type can carry a reference, directly or inside an aggregate. */
/* The depth of the home arena to pass at this call site, derived from the arguments and
 * the parameters of the callee. */
/* True when a binding has been proved non-null at the current position.
 *
 * Params:
 *   c     - checker
 *   cname - the generated C name of the binding, because narrowing follows the binding
 *           rather than the name written in the source
 *
 * Returns:
 *   True while the proof is still in effect; false for NULL and for a name that was never
 *   proved.
 */
bool isNarrowed(Checker *c, const char *cname) {
    if (!cname) return false;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) return true;
    return false;
}

/* Record that a binding has been proved non-null at the current position; recording the
 * same name twice changes nothing. */
void pushNarrow(Checker *c, const char *cname) {
    if (!cname || isNarrowed(c, cname)) return;
    *(const char **)vecPush(&c->narrow) = cname;
}

/* Drop the non-null proof of a binding, which an assignment or a retarget invalidates.
 *
 * The proofs are kept as a stack, so everything from the entry for this binding onwards
 * is cut. A fact about a path that begins with the binding, such as `h.p`, goes with it:
 * any write to the root `h`, or taking its address, can make "h.p is not null" stop
 * holding.
 *
 * Params:
 *   c     - checker
 *   cname - the generated C name of the binding; NULL is ignored
 */
void unNarrow(Checker *c, const char *cname) {
    if (!cname) return;
    for (size_t i = 0; i < c->narrow.len; i++) {
        const char *k = *(const char **)vecAt(&c->narrow, i);
        if (strcmp(k, cname) == 0) { c->narrow.len = i; return; }
        /* A fact about a path, such as `h.p`, is dropped as soon as the prefix matches:
         * any write to the root, or taking its address, can make it stop holding. */
        size_t l = strlen(cname);
        if (strncmp(k, cname, l) == 0 && k[l] == '.') { c->narrow.len = i; return; }
    }
}

/* The name a condition proves non-null, or NULL when it proves nothing.
 *
 * `p != null` proves it on the taken branch, and `p == null` proves it on the else
 * branch, which is what turns `if p == null { return }` into a guard. Only a nullable
 * reference qualifies, since comparing a non-nullable one against null is an error
 * reported by the binary-operator check.
 *
 * Params:
 *   c        - checker
 *   cond     - the condition to inspect
 *   whenTrue - set to true when the proof holds on the taken branch, false when it holds
 *              on the else branch
 *
 * Returns:
 *   The generated C name of the binding, or the path key `root.field` for a field of a
 *   local root, or NULL.
 */
const char *narrowTarget(Checker *c, Expr *cond, bool *whenTrue) {
    if (!cond || cond->kind != EX_BIN) return NULL;
    const char *op = cond->u.bin.op;
    if (strcmp(op, "!=") != 0 && strcmp(op, "==") != 0) return NULL;
    Expr *var = NULL;
    if (cond->u.bin.right->kind == EX_NULL)      var = cond->u.bin.left;
    else if (cond->u.bin.left->kind == EX_NULL)  var = cond->u.bin.right;
    else return NULL;
    /* Path narrowing, `if h.p != null { ... }`, restricted to the sound case: the root
     * has to be a local whose address was never taken, because then no alias can change
     * its fields and the fact "h.p is not null" stays true.
     *
     * A parameter cannot be the root, since the caller may hold an alias or another
     * `mut ref` to the same object, and neither can a binding whose address was taken.
     * This is the same alias restriction that the depth bookkeeping relies on. */
    if (var && var->kind == EX_FIELD && var->u.field.obj->kind == EX_IDENT) {
        Sym *rs = lookup(c, var->u.field.obj->u.ident.name);
        if (!rs || !rs->type || rs->depth < 1 || rs->addressed) return NULL;
        if (!var->type || var->type->kind != TY_REF || !var->type->nullable) return NULL;
        size_t kn = strlen(rs->cname) + strlen(var->u.field.name) + 2;
        char *key = (char *)arenaAlloc(c->arena, kn);
        snprintf(key, kn, "%s.%s", rs->cname, var->u.field.name);
        *whenTrue = (strcmp(op, "!=") == 0);
        return key;
    }
    if (!var || var->kind != EX_IDENT) return NULL;
    Sym *sy = lookup(c, var->u.ident.name);
    if (!sy || !sy->type || sy->type->kind != TY_REF || !sy->type->nullable) return NULL;
    *whenTrue = (strcmp(op, "!=") == 0);
    return sy->cname;
}

/* Record every non-null fact a condition proves.
 *
 * Both sides of an `&&` are walked, because reaching the branch where the condition holds
 * means both sides held: `if p != null && p.next != null { ... }` is the shape this
 * exists for, and longer chains need it even more.
 *
 * Params:
 *   c    - checker
 *   cond - the condition whose facts are recorded; NULL is ignored
 */
void narrowFactsOf(Checker *c, Expr *cond) {
    if (!cond) return;
    if (cond->kind == EX_BIN && strcmp(cond->u.bin.op, "&&") == 0) {
        narrowFactsOf(c, cond->u.bin.left);
        narrowFactsOf(c, cond->u.bin.right);
        return;
    }
    bool whenTrue = false;
    const char *tg = narrowTarget(c, cond, &whenTrue);
    if (tg && whenTrue) pushNarrow(c, tg);
}

/* True when a block always leaves its enclosing function or loop.
 *
 * This is what recognises the guard form `if p == null { return }`: reaching the code
 * after the `if` then means `p` is not null.
 *
 * Params:
 *   s - a statement, normally the then-branch of an `if`
 *
 * Returns:
 *   True when the last statement of the block is a return, break or continue. An empty
 *   block does not exit.
 */
bool blockExits(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_BLOCK) {
        if (s->u.block.stmts.len == 0) return false;
        return blockExits(*(Stmt **)vecAt(&s->u.block.stmts, s->u.block.stmts.len - 1));
    }
    return s->kind == ST_RETURN || s->kind == ST_BREAK || s->kind == ST_CONTINUE;
}

/* Report an error when a nullable reference is dereferenced without a non-null proof.
 *
 * A nullable reference cannot be dereferenced directly; it has to be compared against
 * null first, so that the proof is visible in the syntax rather than implied.
 *
 * Params:
 *   c    - checker
 *   t    - the type being dereferenced
 *   node - the expression, used for its line
 *   what - short noun phrase naming the operation, for the message
 *
 * Returns:
 *   True when the dereference is rejected; false when `t` is not a nullable reference.
 */
bool rejectNullableDeref(Checker *c, Type *t, Expr *node, const char *what) {
    if (!t || t->kind != TY_REF || !t->nullable) return false;
    ckError(c, node->line,
            "write `if p != null { ... }` (or `if p == null { return }`) first: inside"
            " that branch the compiler knows it is not null and generates no runtime check",
            "`%s` is a nullable reference (`?ref T`), so %s needs a non-null one",
            typeStr(c, t), what);
    return true;
}

/* Declare a binding in the innermost scope.
 *
 * Params:
 *   c      - checker
 *   name   - source name; the generated C name is derived from it
 *   t      - declared type
 *   mut    - true for `var`, false for `let`
 *   shadow - whether a binding of the same name in the same scope may be shadowed. It is
 *            set for `let`, which only introduces a name, and clear for `var`, which
 *            declares storage; two `var`s of the same name in one scope are almost always
 *            meant to be `a = ...`. Parameters count as `var`, because the argument is a
 *            writable copy.
 *   line   - source line, for diagnostics
 *   depth  - lexical depth of the new binding, which is `c->scopes.len` at the
 *            declaration
 *
 * Returns:
 *   The new binding, or the existing one when a shadow was refused. The caller writes
 *   `cname` back into the AST: into a `Param`, or into a declaration statement.
 */
Sym *declare(Checker *c, const char *name, Type *t, bool mut,
                    bool shadow, int line, int depth) {
    Scope *top = *(Scope **)vecAt(&c->scopes, c->scopes.len - 1);

    if (!shadow) {
        for (size_t i = 0; i < top->syms.len; i++) {
            Sym *old = *(Sym **)vecAt(&top->syms, i);
            if (strcmp(old->name, name) == 0) {
                ckError(c, line,
                        "`var` declares storage, so two `var`s with the same name in one scope "
                        "is almost always a typo -- write `x = ...` to assign the existing one, "
                        "or `let x = ...` if you really mean a new name",
                        "`%s` is already declared in this scope", name);
                return old;
            }
        }
    }

    Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
    s->name = name;
    s->cname = cNameFor(c, name);
    s->type = t;
    s->mut = mut;
    s->depth = depth;
    /* A reference-typed binding starts with the depth of its slot as the pointee depth,
     * the conservative answer. Whoever knows the initializer narrows this to the real
     * depth afterwards, so `var cur: ?ref node = head` ends up at 0, since the cursor
     * points at something on the caller's side. */
    s->refDepth = (t && typeContainsRef(c->tt, t)) ? depth : 0;
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
    *(Sym **)vecPush(&c->allSyms) = s;      /* also recorded for the self-check mode */
    return s;
}

/* Resolve a name to a binding, innermost scope first and module-level bindings last, so
 * that a local shadows a global.
 *
 * Params:
 *   c    - checker
 *   name - the source name to resolve
 *
 * Returns:
 *   The innermost binding with that name, or NULL.
 *
 * Notes:
 *   - Within one scope the newest `let` wins, so the list is scanned backwards. In the
 *     other order the second `a` in `let a = 1  let a = a + 1` would resolve to the first.
 *   - Module-level bindings are kept in a list of their own rather than in a scope: the
 *     depth of a function body is the number of open scopes, so giving globals a scope
 *     would push every local one level deeper.
 */
Sym *lookup(Checker *c, const char *name) {
    for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *s = *(Scope **)vecAt(&c->scopes, i);
        /* Scanned backwards, so the newest `let` of the scope wins. The other order
         * would resolve the second `a` of `let a = 1  let a = a + 1` back to the first. */
        for (size_t j = s->syms.len; j-- > 0; ) {
            Sym *sym = *(Sym **)vecAt(&s->syms, j);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    /* Module-level bindings have depth 0 and are deliberately not kept in a scope: a
     * function body computes its depth from the number of open scopes, so a scope for
     * the globals would push every local one level deeper. */
    for (size_t i = 0; i < c->globals.len; i++) {
        Sym *sym = *(Sym **)vecAt(&c->globals, i);
        if (strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

/* ------------------------------------------------------------------- lookups */

/* Turn a mangled function name into the name the user wrote: `pair$make` becomes
 * `pair::make`, while a name from the root module is returned unchanged.
 *
 * The effect dump used to print `f->name` directly, so debug output was full of internal
 * encodings. Keeping mangled names out of diagnostics is the same rule.
 *
 * Params:
 *   name    - the mangled name, or NULL
 *   modName - the module prefix, empty or NULL in the root module
 *
 * Returns:
 *   A name suitable for display. The result may point at a static buffer, which the next
 *   call that needs it overwrites.
 */
const char *checkFnDisplay(const char *name, const char *modName) {
    if (!name) return "?";
    if (!modName || !*modName) return name;    /* root module: already the source name */
    const char *d = strchr(name, '$');
    if (!d) return name;                       /* no mangling prefix, as for `extern!` */
    static char buf[512];
    size_t nl = (size_t)(d - name);
    if (nl + 2 + strlen(d + 1) + 1 > sizeof buf) return name;
    memcpy(buf, name, nl);
    buf[nl] = ':'; buf[nl + 1] = ':';
    strcpy(buf + nl + 2, d + 1);
    return buf;
}

/* Find a module-level function by name.
 *
 * Params:
 *   c    - checker
 *   name - the resolved, flat name
 *
 * Returns:
 *   The matching template or plain function, or NULL. An instance of a generic function is
 *   not a name a program can write, so it never matches.
 */
FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (f->tmpl) continue;               /* an instance is not a name; the template is */
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/* Report an error when a name of another module is used without its module qualifier.
 *
 * Without this rule `use greet` would be decoration, since the flat name table already
 * reaches everything, and `@private` would not protect anything either.
 *
 * Params:
 *   c         - checker
 *   what      - the name being referenced
 *   whatMod   - the module that owns it
 *   qualified - whether the reference came from a qualified name. A qualifier is
 *               rewritten into a flat name while the module is loaded, so this flag is
 *               the only trace left of how the name was written
 *   line      - source line to report
 *
 * Notes:
 *   - Names from the prelude, which is imported automatically, and names from the same
 *     file are exempt.
 */
void requireQualified(Checker *c, const char *what, const char *whatMod, bool qualified, int line) {
    if (qualified) return;
    if (!whatMod) return;
    if (!c->curFunc || !c->curFunc->modName) {
        if (c->curFunc) {                        /* root file: always qualified */
            ckError(c, line++, "Write `mod::name` (and `use mod` at the top of the file)."
                               " A module's `@private` names are not reachable here at all.",
                    "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
        }
        return;
    }
    if (strcmp(c->curFunc->modName, whatMod) == 0) return;   /* the own module */
    ckError(c, line, "Write `mod::name` (and `use mod` at the top of the file)."
                     " A module's `@private` names are not reachable here at all.",
            "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
}

/* Find a field of a struct by name, or NULL. */
FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}

/* The StructDef behind a type: both TY_STRUCT and TY_GENERIC point at one, and anything
 * else has none. */
StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/* Find a method by name on a struct or generic type. Methods live in the body of the type
 * they belong to, so no free function is considered. */
FuncDef *findMethod(Type *st, const char *name) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/* Find a variant of an enum by name, or NULL. */
Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* The type of the k-th payload of a variant.
 *
 * For an instance of a generic enum the payload type is substituted with the type
 * arguments: the payload of `just` in `maybe<i64>` is `i64`, not `T`.
 */
Type *payloadType(TypeTable *tt, Type *et, Variant *v, size_t k) {
    Type *pt = *(Type **)vecAt(&v->types, k);
    if (et && et->kind == TY_ENUM && et->edef &&
        et->edef->typeParams.len > 0 && et->targs.len == et->edef->typeParams.len)
        pt = ttSubstitute(tt, pt, &et->edef->typeParams, &et->targs);
    return pt;
}

/* True when the enum has at least one variant carrying a payload, as in `| circle(f64)`.
 *
 * Such a type is not a C `enum` but a tagged struct, so it cannot be compared with `==`,
 * and printing it prints only the variant name.
 */
bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

/* True when the type can carry a reference, directly or inside an aggregate.
 *
 * This is a different question from "does the type have a zero value", and the two were
 * once answered by one function, which is how a hole appeared:
 *
 *   1. can this type carry a reference? Every variant and every field has to be looked
 *      at.
 *   2. does this type have a zero value? For a payload-carrying enum only tag 0 matters,
 *      because a zero value is tag 0 with the payload zeroed.
 *
 * Merged into one question, `type box = | empty | holding(slice<u8>)` answered "no
 * reference" from its first variant alone, so `exprRefDepth` returned early with 0, the
 * depth of the payload was never computed, and `return box.holding(local[..])` compiled
 * into a dangling reference.
 *
 * Params:
 *   tt - type table, used to substitute the arguments of a generic instance
 *   t  - the type to inspect; NULL carries no reference
 *
 * Returns:
 *   True when any part of the type can hold a reference.
 *
 * Notes:
 *   - A generic instance has to be substituted before it is walked, or `T` looks harmless
 *     and the check passes on a type that carries a reference.
 */
bool typeContainsRef(TypeTable *tt, Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind == TY_ARRAY) return typeContainsRef(tt, t->inner);
    /* Any variant payload can carry a reference, so all of them are inspected. */
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsRef(tt, payloadType(tt, t, va, i))) return true;
        }
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeContainsRef(tt, ttSubstitute(tt, ft, sp, sa))) return true;
    }
    return false;
}

/* True when the type has no zero value.
 *
 * A `ref` has none, and neither does any aggregate that contains one. For a
 * payload-carrying enum the zero value is tag 0 with the payload zeroed, so only the
 * first variant matters and the order of the variants is significant:
 * `| nothing | holding(slice<u8>)` can be zero-initialized while
 * `| holding(slice<u8>) | nothing` cannot.
 *
 * Params:
 *   tt - type table, used to substitute the arguments of a generic instance
 *   t  - the type to inspect
 *
 * Returns:
 *   True when zero-initializing a value of this type would produce a null reference.
 *
 * Notes:
 *   - A generic instance has to be substituted before it is walked: the payload of
 *     `option<i64>` is fine, while the payload of `option<slice<u8>>` contains a
 *     reference. Without the substitution `T` looks harmless, the check passes, and the
 *     generated C names a reference that has no zero value.
 */
bool typeLacksZeroValue(TypeTable *tt, Type *t) {
    if (!t) return false;
    /* The zero value of `?ref T` is null, which is the reason it exists: the `next` of a
     * list or a tree node finally has one. */
    if (t->kind == TY_REF) return !t->nullable;
    if (t->kind == TY_ARRAY) return typeLacksZeroValue(tt, t->inner);
    if (t->kind == TY_ENUM && t->edef) {
        if (t->edef->variants.len == 0) return false;
        Variant *v0 = *(Variant **)vecAt(&t->edef->variants, 0);
        for (size_t i = 0; i < v0->types.len; i++)
            if (typeLacksZeroValue(tt, payloadType(tt, t, v0, i))) return true;
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeLacksZeroValue(tt, ttSubstitute(tt, ft, sp, sa))) return true;
    }
    return false;
}

/* True when evaluating this expression twice is observably the same.
 *
 * `??` becomes a C conditional expression in which the subject appears twice, so a
 * subject with a side effect would run twice: `f() ?? -1` would call `f` twice. The test
 * is a conservative syntactic one and refuses anything containing a call, `?` or `??`
 * rather than trying to decide purity, since asking the user for a `let r = f()` line is
 * better than quietly changing how often a function runs.
 */
bool repeatablePure(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL:
    case EX_IDENT:
    case EX_ENUMVAL:
        return true;
    case EX_FIELD: return repeatablePure(e->u.field.obj);
    case EX_SIGN:  return repeatablePure(e->u.sign.operand);
    case EX_DEREF: return repeatablePure(e->u.deref.operand);
    case EX_INDEX: return repeatablePure(e->u.index.obj) &&
                          repeatablePure(e->u.index.index);
    case EX_UN:    return repeatablePure(e->u.un.operand);
    case EX_BIN:   return repeatablePure(e->u.bin.left) &&
                          repeatablePure(e->u.bin.right);
    default:       return false;   /* calls, methods, `?`, `??`, constructions: all refused */
    }
}

/* True when an address can be taken of this expression: a variable, a field, or `*p`,
 * which is the place `p` points at. */
bool isLvalue(Expr *e) {
    return e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_DEREF;
}

/* Build the type `slice<elem>`; the view protocol itself is declared in the prelude. */
Type *sliceOf(Checker *c, Type *elem) {
    if (!c->sliceDef) return ttError(c->tt);
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Type **)vecPush(&args) = elem;
    return ttGeneric(c->tt, c->sliceDef, &args);
}

/* Build an integer literal node.
 *
 * The compiler uses it to fill in a bound the user omitted, as in `a[2..]`, so that the
 * code generator sees two literal bounds and can tell that the range needs no runtime
 * check.
 */
Expr *intLit(Checker *c, long long v, int line) {
    Expr *e = exprNew(c->arena, EX_INT, line);
    e->u.ival = v;
    e->type = c->tI32;
    return e;
}

/* Read an expression as a literal integer.
 *
 * `-1` counts as one, since it is a unary minus around an integer literal, and a negative
 * bound is the mistake this exists to catch: `a[-1..n]` has to be rejected at compile
 * time rather than at run time.
 *
 * Params:
 *   e   - the expression to read
 *   out - receives the value when `e` is a literal
 *
 * Returns:
 *   True when `e` is an integer literal, possibly negated.
 */
bool asIntLit(Expr *e, long long *out) {
    if (!e) return false;
    if (e->kind == EX_INT) { *out = e->u.ival; return true; }
    if (e->kind == EX_UN && e->u.un.op && strcmp(e->u.un.op, "-") == 0 &&
        e->u.un.operand && e->u.un.operand->kind == EX_INT) {
        *out = -e->u.un.operand->u.ival;
        return true;
    }
    return false;
}

/* True when the expression is a place, that is a chain of bindings, fields, indexes and
 * slices.
 *
 * Slicing a fixed array requires one, because the address of an element is taken. The
 * reason is concrete: `f()[1..2]` would slice the temporary storage of a return value and
 * produce a view of an object that is already dead. Slicing a string literal is exempt,
 * because what is sliced there is a slice value whose bytes have static lifetime.
 */
bool isPlace(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlace(e->u.field.obj);
    case EX_INDEX: return isPlace(e->u.index.obj);
    case EX_SLICE: return isPlace(e->u.slice.obj);
    default:       return false;
    }
}

/* The binding at the root of a place, found by walking through fields, indexes and
 * slices.
 *
 * `let` governs the root: in `p.x = 1`, `a[i] = 1` and `v[0] = 1` the root is the binding
 * itself. Checking only a bare identifier would miss `let p: point  p.x = 1`, which is the
 * most common way of forgetting `var`.
 *
 * Params:
 *   c - checker
 *   e - the place expression
 *
 * Returns:
 *   The root binding, or NULL when the walk ends at something that is not a binding.
 *
 * Notes:
 *   - The rule is shallow: it governs the name, not the data. Copying a `let` view into a
 *     `var`, or passing it into a function, still lets the other side write the same
 *     memory. Governing the data would mean putting mutability into the type system, as in
 *     `&` and `&mut`.
 */
Sym *placeRoot(Checker *c, Expr *e) {
    while (e) {
        if (e->kind == EX_IDENT)  return lookup(c, e->u.ident.name);
        if (e->kind == EX_FIELD)  { e = e->u.field.obj; continue; }
        if (e->kind == EX_INDEX)  { e = e->u.index.obj; continue; }
        if (e->kind == EX_SLICE)  { e = e->u.slice.obj; continue; }
        return NULL;
    }
    return NULL;
}

/* True when the path to this place crosses a read-only reference, including the type of
 * the place itself. */

/* True when a place can be written, which also decides whether taking a reference to it
 * gives `mut ref T` or `ref T`.
 *
 * There are three sources of writability, matching where the value came from:
 *   1. a binding: `var` is writable and `let` is read-only
 *   2. a parameter: `mut ref T` is writable and `ref T` is not, as the type says
 *   3. a field or an element: the root decides, exactly as it does for an assignment
 *
 * Params:
 *   c - checker
 *   e - the place expression; NULL is not writable
 *
 * Returns:
 *   True when writing through this place is allowed.
 */
bool isWritablePlace(Checker *c, Expr *e) {
    if (!e) return false;
    /* `*p` is writable exactly when the reference itself is a `mut ref`: the permission
     * comes from the type of the reference. */
    if (e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        return ot && ot->kind == TY_REF && ot->mut;
    }
    /* A read-only reference on the way blocks the write. */
    if (pathHasReadonlyRef(e)) return false;
    /* The expression is itself a reference, so only `mut ref` is writable. */
    if (e->type && e->type->kind == TY_REF) return e->type->mut;
    /* A view needs a writable type of its own, `mut slice<T>`. A string literal takes
     * this path too: its type is a read-only view, so it is not writable. */
    if (e->type && e->type->kind == TY_GENERIC && ttIsViewType(e->type) && !e->type->mut)
        return false;
    /* A binding, a field or an element: the root has to be `var`. */
    Sym *root = placeRoot(c, e);
    return root && root->mut;
}

/* Before a write into a place, its root has to be `var`; `requireMutable` performs that
 * check and returns true once it has reported the error, so the caller gives up. */
