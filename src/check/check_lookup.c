/**
 * @file check_lookup.c
 * @brief Name lookup and non-null narrowing for `?ref T`.
 *
 * Resolving a name to a binding lives here, together with the facts a comparison against
 * null establishes and the small predicates the rest of the checker is built from.
 */

#include <stdint.h>
#include <string.h>
#include "dbg.h"
#include "check_internal.h"
#include "plan.h"          /* the plan side: "is this an instance" is a plan question */

/* One recursion cap for the type walkers below: an enum may hold **itself**
 * (`type list = | cons(i64, list) | nil`), and such a walk would not terminate. Past the
 * cap each walker answers conservatively. */
#define ZERO_VALUE_DEPTH_LIMIT 64

/* ----------------------------------------------- non-null narrowing for `?ref T` */
/**
 * @brief Tell whether a generated name is proven non-null at the current point.
 *
 * The proof follows the binding rather than the name written in the source, so the query
 * uses the binding's generated C name.
 *
 * @param c      Checker; the proof table lives inside it.
 * @param cname  Generated C name of the binding.
 * @return       True while the proof still holds; false when `cname` is NULL or the
 *               binding was never proven.
 */
bool isNarrowed(Checker *c, const char *cname) {
    if (!cname) return false;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) return true;
    return false;
}

/**
 * @brief Record that a binding is proven non-null at the current point.
 *
 * The generated C name is recorded, not the source name, because narrowing follows the
 * binding. Recording the same name twice changes nothing.
 *
 * @param c      Checker.
 * @param cname  Generated C name of the binding; NULL is ignored.
 */
void pushNarrow(Checker *c, const char *cname) {
    if (!cname || isNarrowed(c, cname)) return;
    *(const char **)vecPush(&c->narrow) = cname;
}

/**
 * @brief Drop the non-null proofs of a binding and of the paths prefixed by it.
 *
 * Proofs are kept as a stack and truncated from this binding's entry on: a write to the
 * root, or taking its address, invalidates a path fact such as `h.p`. Does nothing when
 * the binding was never proven.
 *
 * @param c      Checker.
 * @param cname  Generated C name of the binding; NULL is ignored.
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

/**
 * @brief Return the binding name a condition proves non-null.
 *
 * `p != null` holds in the true arm and `p == null` in the false arm; the latter is what
 * turns `if p == null { return }` into a guard. The field path `h.p` holds only when the
 * root is a local whose address was never taken, since otherwise an alias could change it.
 *
 * @param c         Checker.
 * @param cond      Condition expression to examine.
 * @param whenTrue  Out parameter: true when the proof holds in the true arm, false when it
 *                  holds in the false arm.
 * @return          The binding's generated C name, or the field-path key `root.field`;
 *                  NULL when the condition proves nothing.
 */
const char *narrowTarget(Checker *c, Expr *cond, bool *whenTrue) {
    if (!cond || cond->kind != EX_BIN) return NULL;
    const char *op = cond->u.bin.op;
    if (!isEqualityOp(op)) return NULL;
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

/**
 * @brief Record every non-null fact a condition can prove.
 *
 * Both sides of `&&` are walked: reaching the arm where the condition holds shows that both
 * sides hold. Only facts that hold when the condition is true are recorded.
 *
 * @param c     Checker.
 * @param cond  Condition to take facts from; NULL is ignored.
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

/**
 * @brief Tell whether a statement always leaves its block.
 *
 * This is the basis for recognising guards such as `if p == null { return }`: code after the
 * `if` proves the condition was false. A block is judged recursively by its last statement;
 * an empty block does not leave.
 *
 * @param s  Statement, usually the then arm of an `if`.
 * @return   True when the statement is itself a return, break, or continue, or a block whose
 *           last statement is.
 */
bool blockExits(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_BLOCK) {
        if (s->u.block.stmts.len == 0) return false;
        return blockExits(*(Stmt **)vecAt(&s->u.block.stmts, s->u.block.stmts.len - 1));
    }
    return s->kind == ST_RETURN || s->kind == ST_BREAK || s->kind == ST_CONTINUE;
}

/**
 * @brief Reject dereferencing a nullable reference without a non-null proof.
 *
 * A nullable reference cannot be dereferenced directly: it must first be compared with null,
 * so the proof lives in the syntax instead of relying on inference.
 *
 * @param c     Checker.
 * @param t     Type being dereferenced.
 * @param node  The expression, for its line number.
 * @param what  Operation being checked, named in the diagnostic.
 * @return      True when `t` is a nullable reference, in which case the error has been
 *              reported; false otherwise.
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

/**
 * @brief Declare a binding in the innermost scope and return it.
 *
 * A reference-typed binding takes its slot's depth as the pointee depth, the conservative
 * answer, which whoever knows the initializer narrows afterwards. The caller is responsible
 * for writing the generated C name back onto the AST.
 *
 * @param c       Checker.
 * @param name    Source name; the generated C name is derived from it.
 * @param t       Declared type.
 * @param mut     True for `var`, false for `let`.
 * @param shadow  Whether a binding of the same name may shadow an existing one in this
 *                scope.
 * @param line    Source line, for diagnostics.
 * @param depth   Lexical depth of the new binding.
 * @return        The new binding; when a same-named `var` is rejected, the binding that
 *                already exists.
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
    anSetRefDepth(s, (t && typeContainsRef(c->tt, t)) ? depth : 0);
    /* The default of `minPointee` is "not recorded yet" (-1), not 0: 0 is a strong claim
     * (the value could point at the caller's storage), and using it as the default would
     * require every `(*x).f = v` to be promoted to home. The declaration site and every
     * retarget record it. */
    s->minPointee = -1;
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
    *(Sym **)vecPush(&c->allSyms) = s;      /* also recorded for the self-check mode */
    return s;
}

/**
 * @brief Resolve a binding by name, searching the innermost scope first and module-level
 *        bindings last.
 *
 * A later-declared `let` in the same scope wins, so the scan runs backwards. Module-level
 * bindings are stored separately rather than in a scope of their own, or the function body
 * depth would gain a level for every global.
 *
 * @param c     Checker.
 * @param name  Source name to resolve.
 * @return      The innermost binding with that name; NULL when there is none.
 */
Sym *lookup(Checker *c, const char *name) {
            for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *s = *(Scope **)vecAt(&c->scopes, i);
        /* Scanned backwards so the newest `let` of the scope wins; the other order would
         * resolve the second `a` of `let a = 1  let a = a + 1` back to the first. */
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

/**
 * @brief Rewrite a mangled function name into the form the user wrote, for diagnostics.
 *
 * The result may point into a static buffer that the next call overwrites. A NULL `name`
 * returns `"?"`; an empty module name (the root module) or a name without a `$` prefix is
 * returned unchanged.
 *
 * `FN(f)` is its diagnostic alias: `FN(f)` expands to
 * `checkFnDisplay(f->name, f->modName)`.
 *
 * @param name     Mangled name; may be NULL.
 * @param modName  Module prefix; empty or NULL in the root module.
 * @return         A display name of the form `mod::fn`; `name` when the buffer is too small.
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

/**
 * @brief Look up a module-level function by name in the module.
 *
 * Instances of a generic function are not names the program can write, so they are not
 * matched; the template itself can be.
 *
 * @param c     Checker.
 * @param name  Flattened function name.
 * @return      The matching template or plain function; NULL when there is none.
 */
FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (planTemplate(f)) continue;       /* an instance is not a name; the template is */
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/**
 * @brief Tell whether an import has already brought a name into the current module's scope.
 *
 * A wildcard entry covers every public name of that module; a named entry covers only the
 * one it writes. The comparison uses the module-name pairs the loader records, so a `use` in
 * one file does not leak into another. The `opens` table is scanned linearly.
 *
 * @param m         Merged module whose `opens` table the loader filled.
 * @param importer  Module performing the reference; NULL for the root file.
 * @param opened    Module that owns the name.
 * @param what      Name to look for.
 * @return          True when that module brought the name in with `use opened::*` or
 *                  `use opened::{what}`; false otherwise, and false when `m` is NULL.
 */
bool nameBrought(Module *m, const char *importer, const char *opened, const char *what) {
    if (!m) return false;
    for (size_t i = 0; i < m->opens.len; i++) {
        Open *o = (Open *)vecAt(&m->opens, i);
        if (!o->opened || strcmp(o->opened, opened) != 0) continue;
        /* A wildcard entry (no name) covers every public name; a list entry covers the
         * one it names and nothing else. `use mod::{a}` must not make `b` legal, which
         * is the whole reason to list names instead of opening the module. */
        if (o->name && (!what || strcmp(o->name, what) != 0)) continue;
        if (o->importer == NULL && importer == NULL) return true;
        if (o->importer && importer && strcmp(o->importer, importer) == 0) return true;
    }
    return false;
}

/**
 * @brief Report an error when another module's name is used without a module qualifier.
 *
 * The same file, the current module, and the automatically imported prelude are exempt;
 * `use mod::*` or `use mod::{name}` also passes once it has brought the name into the
 * current module's scope. `@private` is still enforced at the declaration, so opening a
 * module is not a way around it.
 *
 * @param c          Checker.
 * @param what       Name being referenced.
 * @param whatMod    Module that owns it.
 * @param qualified  Whether this reference came from a qualified name; qualified names are
 *                   rewritten to flat names at load time, so this flag is the only trace of
 *                   how it was written.
 * @param line       Source line for the error.
 */
void requireQualified(Checker *c, const char *what, const char *whatMod, bool qualified, int line) {
    if (qualified) return;
    if (!whatMod) return;
    /* This has to be answered **before** the unconditional root-file rule below: the root
     * file has no module name of its own, so every bare reference from it would be rejected.
     * The pair carries the importer, so an `open` in one file never leaks into another. */
    if (c->curFunc && nameBrought(c->m, c->curFunc->modName, whatMod, what)) return;
    if (!c->curFunc || !c->curFunc->modName) {
        if (c->curFunc) {                        /* root file: qualified by default */
            ckError(c, line++, "Write `mod::name` (and `use mod` at the top of the file), or"
                               " put the module's names in scope with `use mod::*`."
                               " A module's `@private` names are not reachable here at all.",
                    "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
        }
        return;
    }
    if (strcmp(c->curFunc->modName, whatMod) == 0) return;   /* the own module */
    ckError(c, line, "Write `mod::name` (and `use mod` at the top of the file), or put the"
                     " module's names in scope with `use mod::*`."
                     " A module's `@private` names are not reachable here at all.",
            "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
}

/**
 * @brief Look up a field by name in a struct definition.
 *
 * @param sd    Struct definition to search.
 * @param name  Field name.
 * @return      The matching field definition; NULL when the name is absent.
 */
FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}







/**
 * @brief Look up a variant by name in an enum definition.
 *
 * @param td    Enum definition to search.
 * @param name  Variant name.
 * @return      The matching variant; NULL when `td` is NULL or the name is absent.
 */
Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/**
 * @brief Return the type of the k-th payload of a variant.
 *
 * When the enum is a generic instance, type parameters in the payload type are replaced by
 * the type arguments: the payload of `just` in `maybe<i64>` is `i64`, not `T`.
 *
 * @param tt  Type table, used for substitution.
 * @param et  Enum instance type; for a non-instance the payload type is returned as is.
 * @param v   Variant definition.
 * @param k   Payload index, from 0.
 * @return    The type of the payload at that position.
 */
Type *payloadType(TypeTable *tt, Type *et, Variant *v, size_t k) {
    Type *pt = *(Type **)vecAt(&v->types, k);
    if (et && et->kind == TY_ENUM && et->edef &&
        et->edef->typeParams.len > 0 && et->targs.len == et->edef->typeParams.len)
        pt = ttSubstitute(tt, pt, &et->edef->typeParams, &et->targs);
    return pt;
}



static bool typeContainsProtoAt(TypeTable *tt, Type *t, const char *name, int depth);

/**
 * @brief Tell whether a type contains the named protocol marker at any depth.
 *
 * The marker may appear in the type itself, in a type argument, or behind a view, array, or
 * reference; both the generic spelling and the instantiated spelling count. To keep a
 * self-referential type from making the walk non-terminating, it descends only through those
 * positions and does not expand struct fields; past the recursion limit it returns the
 * conservative answer, true.
 *
 * @param tt    Type table.
 * @param t     Type to check; NULL counts as not containing the marker.
 * @param name  Marker name, for example `coroutine`.
 * @return      True when the marker is found.
 */
bool typeContainsProto(TypeTable *tt, Type *t, const char *name) {
    return typeContainsProtoAt(tt, t, name, 0);
}

/**
 * @brief Look for a protocol marker below a given depth, answering true conservatively past
 *        the recursion limit.
 *
 * @param tt     Type table.
 * @param t      Type to check; NULL counts as not containing the marker.
 * @param name   Marker name.
 * @param depth  Current recursion depth.
 * @return       True when the marker is found; also true past the depth limit.
 */
static bool typeContainsProtoAt(TypeTable *tt, Type *t, const char *name, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (isProtoType(t, name, 1)) return true;
    /* Three spellings reach here: the generic itself (`TY_GENERIC` + sdef), a plain name that has not
     * been resolved into a struct yet, and the instance (whose `sdef` is the prelude's `coroutine`). */
    if (t->sdef && t->sdef->name && strcmp(t->sdef->name, name) == 0) return true;
    if (t->name && strcmp(t->name, name) == 0) return true;
    if (t->kind == TY_ARRAY || t->kind == TY_REF) return typeContainsProtoAt(tt, t->inner, name, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        if (typeContainsProtoAt(tt, *(Type **)vecAt(&t->targs, i), name, depth + 1)) return true;
    /* Struct fields are deliberately **not** walked: substituting a generic's own `T` hands
     * back a type that mentions itself, so the walk would not terminate. The shapes that
     * matter are covered above -- a concrete marker appears as the type itself, inside
     * `targs`, or behind a view/array/reference. A field *declared* with the marker is
     * caught where fields are resolved instead. */
    return false;
}

static bool typeContainsRefAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief Tell whether a type can carry a reference.
 *
 * A reference itself, an array element, any variant payload, or a struct field carrying a
 * reference all count. The recursion is depth-limited, and past the limit it conservatively
 * answers yes. Generic instances substitute their type arguments first, or `T` would look
 * harmless and the check would miss.
 *
 * @param tt  Type table; substitutes the arguments of generic instances.
 * @param t   Type to check; NULL carries no reference.
 * @return    True when any part of the type can hold a reference.
 */
bool typeContainsRef(TypeTable *tt, Type *t) {
    return typeContainsRefAt(tt, t, 0);
}

/**
 * @brief Tell whether a type can hold a **counted rc reference** (`typeContainsRef` minus
 *        the view-handle layer).
 *
 * The `data` field of `slice<T>` is a `ref`, but a **view handle is not itself a counted rc
 * slot**: handing a view to a black box, or dropping it from a local, does not make a holder
 * count of some rc object disappear -- the storage a view points at is managed by the slab,
 * pool, or region itself. A reference inside a view's **element or argument**
 * (`slice<ref T>`) still counts, and a view nested in an aggregate still counts (that goes
 * through `typeContainsRef`, which recurses struct fields).
 *
 * The region acyclicity test (iv) uses this criterion; otherwise "print one line of text"
 * (`println(slice<u8>)`) would pin the whole program as cyclic, which mistakes a view for an
 * rc reference. Whether a view is a leak source is a different question (slab/pool lifetime)
 * and lies outside this criterion, "references live only in counted heap slots".
 *
 * @param tt  Type table.
 * @param t   Type to check; NULL holds no reference.
 * @return    True when it can hold a counted rc reference.
 */
bool typeHoldsCountedRef(TypeTable *tt, Type *t) {
    if (!t) return false;
    if (ttIsViewType(t)) {
        for (size_t i = 0; i < t->targs.len; i++)
            if (typeHoldsCountedRef(tt, *(Type **)vecAt(&t->targs, i))) return true;
        return false;
    }
    return typeContainsRef(tt, t);
}

/**
 * @brief Look for a reference below a given depth, answering true conservatively past the
 *        recursion limit.
 *
 * @param tt     Type table.
 * @param t      Type to check; NULL carries no reference.
 * @param depth  Current recursion depth.
 * @return       True when a reference is found; also true past the depth limit.
 */
static bool typeContainsRefAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind == TY_ARRAY) return typeContainsRefAt(tt, t->inner, depth + 1);
    /* Any variant payload can carry a reference, so all of them are inspected. */
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsRefAt(tt, payloadType(tt, t, va, i), depth + 1)) return true;
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
        if (typeContainsRefAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

static bool typeContainsFnAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief Tell whether a type is a function type or contains one internally.
 *
 * The same family of walks as `typeContainsRef`, present only for the wording of one
 * diagnostic: `fn(i64) -> i64` has no zero value, but not because of a reference. The
 * recursion is depth-limited too and answers yes conservatively past the limit.
 *
 * @param tt  Type table; substitutes the arguments of generic instances.
 * @param t   Type to check; NULL contains no function type.
 * @return    True when the type is TY_FN itself, or an array, a variant payload, or a struct
 *            field contains a function type.
 */
bool typeContainsFn(TypeTable *tt, Type *t) {
    return typeContainsFnAt(tt, t, 0);
}

/**
 * @brief Look for a function type below a given depth, answering true conservatively past
 *        the recursion limit.
 *
 * @param tt     Type table.
 * @param t      Type to check; NULL contains no function type.
 * @param depth  Current recursion depth.
 * @return       True when a function type is found; also true past the depth limit.
 */
static bool typeContainsFnAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (t->kind == TY_FN) return true;
    if (t->kind == TY_ARRAY) return typeContainsFnAt(tt, t->inner, depth + 1);
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsFnAt(tt, payloadType(tt, t, va, i), depth + 1)) return true;
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
        if (typeContainsFnAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

static bool typeLacksZeroValueAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief Tell whether a type has no zero value.
 *
 * A non-null reference and a function type have no zero value (zeroing the latter yields a
 * null function pointer), and neither does an aggregate containing one. For an enum with
 * payloads only the first variant matters, because its zero value is tag 0 with the payload
 * zeroed, so variant order is significant: `| nothing | holding(slice<u8>)` can be zeroed,
 * `| holding(slice<u8>) | nothing` cannot. The recursion is depth-limited and answers yes
 * conservatively past the limit.
 *
 * @param tt  Type table; substitutes the arguments of generic instances.
 * @param t   Type to check.
 * @return    True when zeroing the type would produce a null reference or null function
 *            pointer.
 */
bool typeLacksZeroValue(TypeTable *tt, Type *t) {
    return typeLacksZeroValueAt(tt, t, 0);
}

/**
 * @brief Decide the zero-value question below a given depth, answering true conservatively
 *        past the recursion limit.
 *
 * @param tt     Type table.
 * @param t      Type to check.
 * @param depth  Current recursion depth.
 * @return       True when the type has no zero value; also true past the depth limit.
 */
static bool typeLacksZeroValueAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    /* The zero value of `?ref T` is null, which is the reason it exists: the `next` of a
     * list or a tree node finally has one. */
    if (t->kind == TY_REF) return !t->nullable;
    /* A function type has none either: the zero value of a code pointer is a **null function
     * pointer**, so `var f: fn(i64) -> i64` with no initializer would jump to address zero when
     * called. `option<fn …>` is how "may be absent" is written. */
    if (t->kind == TY_FN) return true;
    if (t->kind == TY_ARRAY) return typeLacksZeroValueAt(tt, t->inner, depth + 1);
    if (t->kind == TY_ENUM && t->edef) {
        if (t->edef->variants.len == 0) return false;
        Variant *v0 = *(Variant **)vecAt(&t->edef->variants, 0);
        for (size_t i = 0; i < v0->types.len; i++)
            if (typeLacksZeroValueAt(tt, payloadType(tt, t, v0, i), depth + 1)) return true;
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
        if (typeLacksZeroValueAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

/**
 * @brief Tell whether re-evaluating this expression makes no observable difference.
 *
 * The body of a `??` appears twice in the conditional expression generated for it, so a body
 * with side effects would run twice. The criterion is a conservative syntactic test: an
 * expression containing a call, `?`, or `??` is rejected outright rather than inferring
 * purity.
 *
 * @param e  Expression to check.
 * @return   True when the expression consists only of literals, bindings, fields, indexes,
 *           slices, and unary or binary operators.
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

/**
 * @brief Tell whether the storage behind this binding was taken in the current frame.
 *
 * The answer decides whether a reference may point into a view: a block allocated with
 * `new T[n]` here, or a slice of such a block, cannot be moved by anybody, so an element
 * reference into it is as safe as one into a fixed-size array. A view of a container's plate
 * is refused, because growing the container resizes or replaces that plate.
 *
 * @param c  Checker; bindings are looked up through it.
 * @param e  Expression; NULL is not frame storage.
 * @return   True when the expression names storage this frame owns.
 */
bool exprIsFrameStorage(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NEW:
        return e->u.new_.count != NULL;   /* `new T[n]`: the block lives in this frame */
    case EX_SLICE:
        return exprIsFrameStorage(c, e->u.slice.obj);
    case EX_INDEX:
        return exprIsFrameStorage(c, e->u.index.obj);
    case EX_IDENT: {
        Sym *s = lookup(c, e->u.ident.name);
        return s && (s->frameStore || s->stableView);
    }
    default:
        return false;
    }
}

/**
 * @brief Tell whether the address of this expression can be taken.
 *
 * A place is storage the compiler can name: a binding, a field, `*p`, an element of a
 * fixed-size array, or an element of a view whose storage this frame owns. Those three kinds
 * of storage share the property the depth check is built on: they cannot be moved while the
 * frame lives, so a reference into them cannot be invalidated.
 *
 * Elements of container-owned views are refused: the plate behind a view is resized when the
 * container grows, so the address would go stale. The containers hand out values, not
 * addresses, for that same reason.
 *
 * @param c  Checker; bindings and their storage provenance are looked up through it.
 * @param e  Expression; NULL is not a place.
 * @return   True when the expression is a place that a reference may point at.
 */
bool isLvalue(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_DEREF) return true;
    if (e->kind == EX_INDEX) {
        Expr *obj = e->u.index.obj;
        if (!obj || !obj->type) return false;
        if (obj->type->kind == TY_ARRAY) return isLvalue(c, obj);
        if (ttIsViewType(obj->type)) return exprIsFrameStorage(c, obj);
        return false;
    }
    return false;
}

/**
 * @brief Build the `slice<elem>` type from the declaration in the prelude.
 *
 * @param c     Checker; holds the prelude's slice definition.
 * @param elem  Element type.
 * @return      The view type; the error type when the prelude has no slice definition.
 */
Type *sliceOf(Checker *c, Type *elem) {
    if (!c->sliceDef) return ttError(c->tt);
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Type **)vecPush(&args) = elem;
    return ttGeneric(c->tt, c->sliceDef, &args);
}

/**
 * @brief Make an integer literal expression node.
 *
 * The compiler uses this to fill in slice bounds the user omitted, so that codegen sees two
 * literal bounds and knows the range needs no run-time check. The type follows the value:
 * `i32` while it fits, `i64` otherwise, because a length such as an array's `asize` can
 * exceed `INT32_MAX` and a literal typed `i32` would then have an out-of-range payload.
 *
 * @param c     Checker; the node is allocated from its arena.
 * @param v     Literal value.
 * @param line  Source line.
 * @return      The new `EX_INT` node.
 */
Expr *intLit(Checker *c, long long v, int line) {
    Expr *e = exprNew(c->arena, EX_INT, line);
    e->u.ival = v;
    e->type = (v > INT32_MAX || v < INT32_MIN) ? c->tI64 : c->tI32;
    return e;
}

/**
 * @brief Read an expression as an integer literal.
 *
 * An integer literal wrapped in a unary minus counts too, so a negative slice bound is
 * rejected at compile time instead of at run time.
 *
 * @param e    Expression to read.
 * @param out  Receives the literal value when it reads as one.
 * @return     True when `e` is an integer literal (possibly with a unary minus), in which
 *             case `out` is written; false otherwise.
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

/**
 * @brief Tell whether an expression is a place.
 *
 * A place is a chain of bindings, fields, indexes, and slices. Slicing a fixed-size array
 * requires the operand to be a place, because the element address is taken; otherwise the
 * slice would point into the temporary that holds the return value. String literals are the
 * exception: their bytes have static storage duration.
 *
 * @param e  Expression.
 * @return   True when the root of the chain is a binding and every step is a field, index, or
 *           slice; false otherwise.
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

/**
 * @brief Walk to the root identifier node following only the steps the mask allows.
 *
 * It follows the syntactic chain without resolving names or consulting scopes, and the
 * returned node is guaranteed to be EX_IDENT. An empty mask or an unknown bit is rejected by
 * an assertion in debug builds; when the walk cannot reach the root the caller treats it
 * conservatively as "not a place".
 *
 * @param e        Starting expression.
 * @param steps    Allowed-step mask, an OR of EXTC_ROOT_* bits.
 * @param maxHops  Hop limit; <= 0 means no limit.
 * @return         The identifier node at the root; NULL when the root cannot be reached or
 *                 the hop limit is exceeded.
 */
Expr *extcRootLeaf(Expr *e, unsigned steps, int maxHops) {
    /* The mask is an invariant: an empty mask or an unknown bit is a typo at the call site,
     * and "cannot reach the root" is treated by the caller as "not a place" (conservative),
     * so a typo would **not** be reported -- it would only make the analysis silently more
     * conservative, which is exactly what needs an assertion. */
    EXTC_DBG_ASSERT(steps != 0 &&
                    (steps & ~(EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_SLICE |
                               EXTC_ROOT_DEREF | EXTC_ROOT_SIGN)) == 0);
    for (int hops = 0; e; hops++) {
        if (maxHops > 0 && hops >= maxHops) return NULL;
        if (e->kind == EX_IDENT) return e;
        if (e->kind == EX_FIELD && (steps & EXTC_ROOT_FIELD)) { e = e->u.field.obj;     continue; }
        if (e->kind == EX_INDEX && (steps & EXTC_ROOT_INDEX)) { e = e->u.index.obj;     continue; }
        if (e->kind == EX_SLICE && (steps & EXTC_ROOT_SLICE)) { e = e->u.slice.obj;     continue; }
        if (e->kind == EX_DEREF && (steps & EXTC_ROOT_DEREF)) { e = e->u.deref.operand; continue; }
        if (e->kind == EX_SIGN  && (steps & EXTC_ROOT_SIGN))  { e = e->u.sign.operand;  continue; }
        return NULL;
    }
    return NULL;
}

/**
 * @brief Resolve the binding at the root of a place expression.
 *
 * Walk to the root identifier through fields, indexes, and slices, then resolve it to a
 * binding with a scope lookup; the mask is FIELD|INDEX|SLICE with no hop limit. Resolving a
 * name needs a scope, so at least one scope must be open (asserted in debug builds); after
 * every scope has closed the call can only match module-level bindings.
 *
 * @param c  Checker, which provides the scope stack.
 * @param e  Place expression.
 * @return   The binding of the root identifier; NULL when the walk does not reach an
 *           identifier or the name does not resolve.
 */
Sym *placeRoot(Checker *c, Expr *e) {
    EXTC_DBG_ASSERT_MSGF(c->scopes.len > 0,
                         "placeRoot with no open scope (phase=%s): needs a scope",
                        phaseName(c->phase));
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_SLICE, 0);
    if (!leaf) return NULL;
    Sym *sy = lookup(c, leaf->u.ident.name);
    /* Phase contract: after every scope has closed, only module-level bindings can be
     * resolved here. That is right and necessary for globals; but if a caller believes it is
     * resolving a **local**, it silently gets `NULL` -- or worse, a **same-named global**. So
     * keep a **counter sentinel** here: if the count of successful scope-less resolutions in
     * PHASE_POST changes, a new call site depends on this fallback. */
    if (sy && c->scopes.len == 0)
        EXTC_DBG_NOTEF("placeRoot(`%s`) in phase=%s with no scope → module-level binding",
                       leaf->u.ident.name, phaseName(c->phase));
    return sy;
}


/**
 * @brief Tell whether a write can go through this expression.
 *
 * Write permission has three sources: a binding depends on `var` versus `let`, a parameter on
 * whether its type is `mut ref` or `ref`, and a field or element on its root. `*p` is decided
 * separately, from the reference type itself; a read-only view also blocks a write.
 *
 * @param c  Checker.
 * @param e  Place expression; NULL is not writable.
 * @return   True when the write is allowed; false when the path crosses a read-only
 *           reference, the view is read-only, or the root is a `let`.
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
