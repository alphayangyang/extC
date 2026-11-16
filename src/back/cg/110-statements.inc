/* ---------------------------------------------------------------- statements */

static void genStmt(CG *g, Stmt *s);

/* Declared here because statements register their local declarations as they are emitted
 * (see DeadLocal): the definition sits with the other unreferenced-definition helpers. */
static void localDef(CG *g, size_t before, const char *name, size_t own);

/**
 * @brief Write a value destined for an `f32` slot as a real float.
 *
 * `let e: f32 = 3.14` would otherwise emit `float e = 3.14;`: the literal is a `double`, the
 * assignment narrows implicitly and clang reports `-Wimplicit-float-conversion`. An explicit cast
 * leaves the value unchanged and makes the narrowing non-implicit.
 *
 * @param g       The generator state, whose arena the cast text is built in.
 * @param target  The target slot's type; the cast is added only for a built-in `f32`.
 * @param src     The source expression, whose type decides whether a cast is needed; NULL means
 *                return as is.
 * @param val     The already generated source value text, which must be evaluated only once.
 * @return        `(float)val`, or `val` unchanged (when the target is not `f32` or the source is
 *                not `f64`).
 */
static const char *asF32(CG *g, Type *target, Expr *src, const char *val) {
    if (!target || target->kind != TY_BUILTIN || !target->name || strcmp(target->name, "f32") != 0)
        return val;
    if (!src || !src->type || src->type->kind != TY_BUILTIN || !src->type->name) return val;
    if (strcmp(src->type->name, "f64") != 0) return val;
    return arenaPrintf(g->arena, "(float)%s", val);
}

/**
 * @brief Strip the one redundant pair of parentheses that wraps a whole condition.
 *
 * A statement carries its own parentheses and the expression printer adds another pair around a
 * comparison, so `if x == y` prints as `if ((x == y))` and clang reports
 * `-Wparentheses-equality`.
 *
 * Only the pair wrapping the whole condition is stripped: a `)` met inside a string literal that
 * does not close at the end makes the text come back unchanged -- the cost is one redundant pair
 * of parentheses and the meaning is unchanged.
 *
 * @param g  The generator state; the new string is built in its arena while stripping.
 * @param e  The condition text; returned as is when it is NULL or does not start with `(`.
 * @return   The text without the outer parentheses, or `e` unchanged.
 */
static const char *cgCond(CG *g, const char *e) {
    if (!e || e[0] != '(') return e;
    int depth = 0;
    for (size_t i = 0; e[i]; i++) {
        if (e[i] == '(') depth++;
/**
 * @brief Find the `)` that exactly closes the outermost `(`, that is, the end of the condition.
 *
 * If characters remain after that `)`, the pair does not wrap the whole condition and the text is
 * returned unchanged; otherwise the pair is removed and the rest handed back.
 */
        else if (e[i] == ')' && --depth == 0) {
            if (e[i + 1]) return e;                  /* not the whole condition */
            Buf b;
            bufInit(&b, g->arena);
            bufPutn(&b, e + 1, i - 1);
            return bufCstr(&b);
        }
    }
    return e;
}

