/* The type layer's implementations. See typelayer.h for why the file exists and what may
 * live here: pure questions about types and nodes, no checker state.
 *
 * These functions were moved out of `check.c` / `check_escape.c` / `check_lookup.c`
 * unchanged -- same code, a name that says who may call it. */
#include "typelayer.h"

/* Report whether `op` is one of the six comparison operators. */
bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

/* Report whether `op` is `&&` or `||`. */
bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

/* Report whether `op` is `==` or `!=`.
 *
 * The pair shares everything that is special about it: one native rule, the `!=` -> `==`
 * fallback, and the diagnostics that talk about comparability rather than ordering. The
 * test used to be spelled out at each of those places. */
bool isEqualityOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
}

/* Report whether this name is an operator that a type may define more than once.
 *
 * The overloadable set is the language's operator set, which is exactly the set the parser
 * allows as a method name. Kept in one function so the parser, the duplicate check and the
 * lookup cannot disagree about which names are operators.
 *
 * Returns:
 *   True for the comparison operators, the arithmetic ones and the stream operators. */
bool isOverloadableOp(const char *name) {
    return isCmpOp(name) || isArithOp(name) ||
           strcmp(name, "<<") == 0 || strcmp(name, ">>") == 0;
}

/* Report whether a type mentions a type parameter, directly or inside a field.
 *
 * Params:
 *   t - the type to inspect
 *
 * Returns:
 *   True when the type cannot be judged yet, so the check has to be deferred until the
 *   instance is known.
 */
bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/* Whether a type is one of the two prelude containers that carry real semantics,
 * recognised by name and argument count.
 *
 * They are reserved definitions that a user may not redefine, so recognising them by
 * name is safe. `?` needs their tag field names, which is the same division of labour
 * as the view protocol of `data` plus `len`: the language knows the protocol, and the
 * library provides the structure.
 *
 * Params:
 *   t     - the type to test
 *   name  - the reserved container name (`option` or `result`)
 *   nargs - the number of type arguments it must carry
 *
 * Returns:
 *   True when `t` is that container with exactly that many type arguments. */
bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* a generic struct: `slice<T>`, `varArray<T>` */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* A generic enum: `option<T>` and `result<T,E>` are enums, and an instance has the
     * type `TY_ENUM` carrying concrete type arguments. */
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/* Whether this type supports the overloadable operator `op`, natively or through a
 * method whose name is the operator.
 *
 * This is the deferred per-instance rule, and it is deliberately the *same* predicate the
 * concrete-type path in check_expr.c applies: two copies of one rule drift, and the drift
 * would show up as a generic accepting what its own instance rejects. Each family answers
 * "native" differently, exactly as the concrete path does:
 *   - `==` / `!=` are native for numbers, `bool` and payload-free enums;
 *   - the ordering operators are native for numbers only;
 *   - the arithmetic operators are native for numbers, and `%` for integers.
 * A user method is accepted for any of them, and an array's `==` is derived by the
 * compiler as long as its elements have one.
 *
 * The signature of a user method was already validated at its definition site
 * (checkOperatorSig), so finding it by name is enough here.
 *
 * Params:
 *   t  - the type the operator is applied to
 *   op - the operator name, one of `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%`
 *
 * Returns:
 *   True when the operator is available for this type. */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    bool isEq = isEqualityOp(op);

    if (isEq ? cmpIsNative(t) : ttIsNumeric(t)) {
        if (strcmp(op, "%") != 0 || ttIsInteger(t)) return true;
    }
    /* The compiler derives `==` for arrays, provided the elements can be compared -- and only
     * against the **same** array type. This used to recurse into the element and ask whether the
     * element compares with `rhs`, so `[1]i64 == i32` was approved at instantiation time (the
     * deferred re-check runs once `T` is known and the element `i64` does compare with `i32`);
     * the emitted descriptor comparison then read 8 bytes out of a 4-byte operand.
     * ASan: stack-buffer-overflow in `extc_eq`, found by tools/fuzz.py in a mutation of
     * examples/generic-free-fn.extc (docs/topics/HARDENING.md 三点七). */
    if (isEq && t->kind == TY_ARRAY)
        return rhs && ttEquals(ttBase(t), ttBase(rhs)) &&
               typeSupportsOp(tt, t->inner, op, t->inner);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(tt, b, op, rhs, isEq && strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
}

/* Find a method by name on a struct or generic type. Methods live in the body of the type
 * they belong to, so no free function is considered. */
FuncDef *findMethod(Type *st, const char *name) {
    /* `Type.mholder` holds methods that hang off a **type without a body of its own**: a builtin
     * scalar (`impl i64 { ... }`) and a generic **instance** (`impl slice<u8> { ... }`). It is
     * consulted first because it is the more specific of the two -- `structOf` answers with the
     * generic declaration's body, whose methods every instance shares. */
    StructDef *sd = st ? st->mholder : NULL;
    if (!sd) sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/* Find the operator method of a type that takes exactly `rhs` on the right.
 *
 * Operators are the one place where a name may appear more than once: their signature
 * shape is fixed -- `self: ref T` plus one operand -- so the only thing left to key on is
 * the operand's type, and keying on it exactly (no conversions, no candidate ranking) is
 * what makes an overload set decidable without overload resolution.
 *
 * Params:
 *   st   - the type whose methods are searched
 *   name - the operator name, for example `<<`
 *   rhs  - the type of the right operand; NULL accepts the first method with that name
 *          (used where the operand type is not interesting, such as reporting)
 *
 * Returns:
 *   The matching method, or NULL when this type has no such operator for that operand. */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    /* A method of a generic type spells its parameters in terms of the *owner's* type
     * parameters (`slice<T>::==` takes `other: ref slice<T>`), so the receiver's type
     * arguments have to be substituted before the operand type can be compared -- without
     * that, every operator of every generic type looked unmatched. */
    Vec *sp = NULL, *sa = NULL;
    Type *b = ttBase(st);
    if (b && b->kind == TY_GENERIC && b->sdef == sd && sd->typeParams.len == b->targs.len) {
        sp = &sd->typeParams;
        sa = &b->targs;
    }
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) != 0) continue;
        if (!rhs || m->params.len < 2) return m;
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        Type *pt = sp ? ttSubstitute(tt, p1->type, sp, sa) : p1->type;
        if (ttEquals(ttBase(pt), ttBase(rhs))) return m;
    }
    return NULL;
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

/* Report whether `op` is one of the five arithmetic operators that can be overloaded.
 *
 * `-` is here as binary subtraction only: extC has no unary operator overloading, so a
 * method named `-` is reached by `a - b` and never by `-a`. */
bool isArithOp(const char *op) {
    return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
           strcmp(op, "/") == 0 || strcmp(op, "%") == 0;
}

/* Whether C itself can compare this type: numbers, `bool`, and enums.
 *
 * `str` is deliberately not in this set, because its `==` would degrade into a pointer
 * comparison, and an `eq` method is required instead.
 *
 * Returns:
 *   True when a C `==` on this type compares values rather than addresses. */
bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    /* An enum with no payload is an integer in C, so it can be compared directly.
     *
     * One with a payload cannot: in C it is a `struct { tag; union }`, and C structs do not
     * support `==`. Use `match` instead. A derived `_eq` would have to compare the payloads
     * recursively and is not done for now. */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
}

/* The StructDef behind a type: both TY_STRUCT and TY_GENERIC point at one, and anything
 * else has none. */
StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/* Find an operator method defined on a type.
 *
 * `!=` is the special case: when there is no `!=` method, the lookup falls back to `==`
 * and negates it, which codegen does.
 *
 * Params:
 *   tt       - the type table (a generic receiver's arguments are substituted)
 *   b        - the type, with any wrapper already stripped
 *   sym      - the operator name to look for (`==`, `<`, `<<`, ...)
 *   rhs      - the type of the right operand; operators are matched on it exactly, and
 *              this is what makes a name being defined more than once decidable
 *   fallback - operator name to try when `sym` is absent, or NULL for none
 *
 * Returns:
 *   The method definition, or NULL when the type defines neither name for that operand. */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback) {
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}
