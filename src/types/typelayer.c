/**
 * @file typelayer.c
 * @brief The type layer's implementation: it answers only **read-only questions** about types
 *        and nodes, and holds no checker state.
 *
 * Dependencies stay one-way: codegen may depend on "what a type is", never on the checker's
 * working state.
 */
#include "typelayer.h"

/**
 * @brief Tell whether `op` is one of the six comparison operators: `==` `!=` `<` `<=` `>` `>=`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

/**
 * @brief Tell whether `op` is one of the logical operators: `&&` `||`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

/**
 * @brief Tell whether `op` is one of the equality operators: `==` `!=`.
 *
 * The pair shares everything that is special about it: one native rule, the `!=` -> `==`
 * fallback, and the diagnostics that talk about comparability rather than ordering. The test
 * exists only here, and every one of those places asks it.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isEqualityOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
}

/**
 * @brief Tell whether `name` is an overloadable operator name (a name a type may define
 * several times).
 *
 * The overloadable set is the language's operator set, which is exactly the set the parser
 * allows as a method name. The test exists only here, so the parser, the duplicate-definition
 * check, and lookup cannot disagree about "which names are operators".
 *
 * @param name  Operator name.
 * @return      `true` when it is a comparison, arithmetic, or stream operator.
 */
bool isOverloadableOp(const char *name) {
    return isCmpOp(name) || isArithOp(name) ||
           strcmp(name, "<<") == 0 || strcmp(name, ">>") == 0;
}

/**
 * @brief Tell whether a type parameter (`T`) occurs in this type, whether directly or nested.
 *
 * The checker defers work on such types: they cannot be decided yet, and have to wait until the
 * instance is known.
 *
 * @param t  Type.
 * @return   `true` when it mentions a type parameter.
 */
bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/**
 * @brief Is `t` one of the two containers in the prelude that carry real semantics, identified
 * by name and argument count?
 *
 * They are reserved definitions that users cannot redefine, so identifying them by name is
 * safe. `?` needs their tag field names, which is the same division of labour as the `data` +
 * `len` view protocol: the language knows the protocol, the library provides the structure.
 *
 * @param t      Type.
 * @param name   The reserved container name (`option` or `result`).
 * @param nargs  The number of type arguments it must carry.
 * @return       `true` when `t` is exactly that container with exactly `nargs` arguments.
 */
bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* Generic structs (`slice<T>`, `varArray<T>`) carry `sdef`; `option` / `result` are enums
     * whose instances are `TY_ENUM` carrying concrete arguments and an `edef`. */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/**
 * @brief Can `t` answer `op` with `rhs`, natively or through a method?
 *
 * This is the per-instance rule applied after deferral, and it deliberately uses the same
 * predicate as the concrete-type path: writing one rule twice drifts, and the drift shows up
 * as generics accepting what their own instances reject. Each family answers "native"
 * differently, exactly as the concrete path does:
 *   - `==` / `!=`: numbers, `bool`, and payload-free enums are native;
 *   - ordering operators: only numbers are native;
 *   - arithmetic operators: numbers are native, and `%` only for integers.
 * A user method is accepted for any of them, and an array's `==` is derived by the compiler as
 * long as its elements have one.
 *
 * A user method's signature was validated where it is defined (`checkOperatorSig`), so finding
 * it by name is enough here.
 *
 * @param tt   Type table.
 * @param t    The type the operator works on.
 * @param op   Operator name, one of `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%`.
 * @param rhs  Right operand type; needed to decide whether an array derives `==`.
 * @return     `true` when it is usable; `false` when `t` is NULL.
 */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    bool isEq = isEqualityOp(op);

    if (isEq ? cmpIsNative(t) : ttIsNumeric(t)) {
        if (strcmp(op, "%") != 0 || ttIsInteger(t)) return true;
    }
    /* Two `any` values compare natively: the comparison walks the descriptor, since the type of the
     * contents is only known at run time. Comparing an `any` with a concrete value is not allowed
     * here -- unbox it first, so the type expectation is spelled out where a mismatch can trap with
     * the user's own line as the position. */
    if (isEq && ttBase(t) && ttBase(t)->kind == TY_ANY && rhs && ttBase(rhs)->kind == TY_ANY)
        return true;
    /* An array's `==` is derived by the compiler provided its elements are comparable, and it
     * can only be compared with the **same** array type: only asking recursively whether an
     * element is comparable with `rhs` would let `[1]i64 == i32` through, and the generated
     * descriptor comparison would then read 8 bytes out of a 4-byte operand. */
    if (isEq && t->kind == TY_ARRAY)
        return rhs && ttEquals(ttBase(t), ttBase(rhs)) &&
               typeSupportsOp(tt, t->inner, op, t->inner);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(tt, b, op, rhs, isEq && strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
}

/**
 * @brief The method named `name` on a type; only methods in the type's own body are
 * considered, not free functions.
 *
 * @param st    The type that carries the method.
 * @param name  Method name.
 * @return      The method; `NULL` when there is none.
 */
FuncDef *findMethod(Type *st, const char *name) {
    /* `Type.mholder` holds methods that hang off a **type without a body of its own**: a
     * builtin scalar (`impl i64 { ... }`) and a generic **instance** (`impl slice<u8> { ... }`).
     * It is consulted first because it is the more specific of the two -- `structOf` answers
     * with the generic declaration's body, whose methods every instance shares. */
    StructDef *sd = st ? st->mholder : NULL;
    if (!sd) sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/**
 * @brief The operator method on `st` whose right operand is `rhs`.
 *
 * Operators are the only place where overloading by name is allowed: the signature shape is
 * fixed (`self: ref T` plus one operand), the only thing left to distinguish is the operand's
 * type, and keying on it exactly (no conversions, no candidate ranking) is what makes an
 * overload set decidable without overload resolution.
 *
 * @param tt    Type table.
 * @param st    Left operand type.
 * @param name  Operator name, such as `<<`.
 * @param rhs   Right operand type; passing NULL accepts the first method with the same name
 *              (for the cases that do not care about the operand type).
 * @return      The method; `NULL` when there is none.
 */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    /* A method of a generic type spells its parameters in terms of the *owner's* type
     * parameters (`slice<T>::==` takes `other: ref slice<T>`), so the receiver's type
     * arguments have to be substituted before the operand type can be compared. */
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

/**
 * @brief Does this enum have at least one variant with a payload, such as `| circle(f64)`?
 *
 * In C such a type is not an `enum` but a tagged struct: it cannot be compared with `==`, and
 * printing shows only the variant name.
 *
 * @param td  Enum definition.
 * @return    `true` when it has a payload-carrying variant; `false` when `td` is NULL.
 */
bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

/**
 * @brief Is `op` one of the five overloadable arithmetic operators: `+` `-` `*` `/` `%`?
 *
 * `-` is only binary subtraction: extC cannot overload unary operators, so a method named `-`
 * is reached by `a - b` and never by `-a`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isArithOp(const char *op) {
    return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
           strcmp(op, "/") == 0 || strcmp(op, "%") == 0;
}

/**
 * @brief Can C itself compare this type: numbers, `bool`, and payload-free enums.
 *
 * `str` is deliberately not in this set: its `==` would degrade to pointer comparison, so it is
 * required to provide an `eq` method.
 *
 * @param t  Type.
 * @return   `true` when C's `==` compares values rather than addresses; `false` when `t` is
 *           NULL.
 */
bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    /* A payload-free enum is an integer in C and can be compared directly. One with a payload
     * cannot: it is `struct { tag; union }`, and C structs do not support `==`, so `match` is
     * required; deriving `_eq` would mean recursively comparing payloads, and it is not
     * derived. */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
}

/**
 * @brief The struct definition behind `TY_STRUCT` / `TY_GENERIC`; other kinds have none.
 *
 * @param t  Type.
 * @return   The struct definition; NULL when `t` is NULL or of another kind.
 */
StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/**
 * @brief Find a type's operator method.
 *
 * `!=` is the special case: when there is no `!=` method it falls back to `==` and negates, and
 * codegen does the negation. A generic receiver's arguments are substituted before matching.
 *
 * @param tt        Type table.
 * @param b         The type that carries the method (`mholder`).
 * @param sym       Operator name to find (`==`, `<`, `<<`, ...).
 * @param rhs       Right operand type; operators match on it exactly, which is what makes
 *                  overloading decidable.
 * @param fallback  Fallback operator name; may be `NULL`.
 * @return          The method found; `NULL` when there is none.
 */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback) {
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}
