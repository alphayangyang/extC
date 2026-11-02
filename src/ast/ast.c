/* Construction of AST nodes, plus the one structural predicate that needs nothing
 * but the tree itself.
 *
 * Every constructor takes a zeroed node from the arena and sets only the kind and
 * the line number, so a field the parser never fills in is zero rather than
 * indeterminate.
 */

#include "ast.h"

#include <string.h>

/**
 * @brief Build an unresolved type name (TY_UNRESOLVED).
 *
 * @param a     Arena that owns the node.
 * @param name  The type name as written in the source; the pointer is kept as is, the text
 *              is not copied.
 * @return      A TY_UNRESOLVED type. Resolving it, together with the type arguments the
 *              parser attaches afterwards, into an interned type is the checker's job.
 */
Type *typeNamed(Arena *a, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_UNRESOLVED;
    t->name = name;
    return t;
}

/**
 * @brief Build `ref inner`, the ordinary reference type.
 *
 * @param a      Arena that owns the node.
 * @param inner  The referenced type.
 * @return       A TY_REF that is neither writable nor nullable; the parser sets `mut` /
 *               `nullable` when the source writes `mut ref` / `?ref`.
 */
Type *typeRef(Arena *a, Type *inner) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/**
 * @brief Build a fixed-length array type `[n]elem` (TY_ARRAY).
 *
 * The length is a compile-time constant and part of the type, so `[3]i32` and `[4]i32` are
 * two different types.
 *
 * @param a     Arena that owns the node.
 * @param n     Number of elements.
 * @param elem  Element type.
 * @return      A TY_ARRAY type.
 */
Type *typeArray(Arena *a, int64_t n, Type *elem) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_ARRAY;
    t->asize = n;
    t->inner = elem;
    return t;
}

/**
 * @brief Build a function type `fn(params...) -> ret` (TY_FN) from the pieces the parser
 * hands over.
 *
 * The parameter and return types may still be TY_UNRESOLVED names, so this only builds the
 * shape: interning happens in `ttResolve` / `ttFn` (the split that keeps `ast.c`'s
 * constructors out of the type table).
 *
 * @param a       Arena that owns the node and the parameter vector.
 * @param params  Parameter types, in order (`Type *` elements); the vector is copied.
 * @param ret     Return type; must not be NULL, and `void` is passed as a real type too.
 * @return        A TY_FN type whose parts may still be unresolved names.
 */
Type *typeFn(Arena *a, Vec *params, Type *ret) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_FN;
    vecInit(&t->params, a, sizeof(void *));
    for (size_t i = 0; i < params->len; i++)
        *(Type **)vecPush(&t->params) = *(Type **)vecAt(params, i);
    t->ret = ret;
    return t;
}

/**
 * @brief Build a type parameter (TY_PARAM).
 *
 * Both `param` and `name` record this name; type equality and type printing identify it by
 * `param`.
 *
 * @param a     Arena that owns the node.
 * @param name  Parameter name, such as `"T"`.
 * @param idx   Its position in the parameter list that declares it.
 * @return      A TY_PARAM type.
 */
Type *typeParam(Arena *a, const char *name, int idx) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_PARAM;
    t->param = name;
    t->name = name;
    t->tpIndex = idx;
    return t;
}

/**
 * @brief Build an expression node.
 *
 * The node comes from the arena already zeroed; only `kind` and the line number are set, so
 * a field the parser does not fill is zero rather than indeterminate. The results of
 * resolution (`type`, `func`, `field`, and the escape-analysis depth) are filled in later by
 * the checker.
 *
 * @param a     Arena that owns the node.
 * @param kind  Expression kind; only the corresponding union member is usable.
 * @param line  Source line number (1-based), kept for diagnostics.
 * @return      The zeroed Expr.
 */
Expr *exprNew(Arena *a, ExprKind kind, int line) {
    Expr *e = (Expr *)arenaAllocZero(a, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    return e;
}

/**
 * @brief Build a statement node.
 *
 * The node comes from the arena already zeroed; only `kind` and the line number are set.
 *
 * @param a     Arena that owns the node.
 * @param kind  Statement kind; only the corresponding union member is usable.
 * @param line  Source line number (1-based), kept for diagnostics.
 * @return      The zeroed Stmt.
 */
Stmt *stmtNew(Arena *a, StmtKind kind, int line) {
    Stmt *s = (Stmt *)arenaAllocZero(a, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

/**
 * @brief Prepare an empty module.
 *
 * Declarations are appended as the parser and loader discover them, so a Module can end up
 * holding, in order, the prelude, the user's file, and then every module that file imports.
 *
 * @param m  Module to initialize; the caller owns the storage.
 * @param a  Arena the vectors allocate from.
 */
void moduleInit(Module *m, Arena *a) {
    vecInit(&m->structs, a, sizeof(void *));
    vecInit(&m->types, a, sizeof(void *));
    vecInit(&m->funcs, a, sizeof(void *));
    vecInit(&m->globals, a, sizeof(void *));
    vecInit(&m->traits, a, sizeof(void *));
    vecInit(&m->impls, a, sizeof(void *));
    vecInit(&m->uses, a, sizeof(void *));
    vecInit(&m->opens, a, sizeof(Open));
}

/**
 * @brief Build an `EX_IDENT`: the identifier records the name exactly as it was written.
 *
 * `name` and `srcName` are set together and are the same pointer at this point; renaming
 * happens after the parser.
 *
 * @param a     Arena that owns the node.
 * @param name  The name as written.
 * @param line  Source line number (1-based).
 * @return      The zeroed identifier node with both names filled in.
 */
Expr *exprIdent(Arena *a, const char *name, int line) {
    Expr *e = exprNew(a, EX_IDENT, line);
    e->u.ident.name = name;
    e->u.ident.srcName = name;
    return e;
}

/**
 * @brief Build a binary operation node with `op` and both operands filled in at once.
 *
 * Both the parser's `mkBin` and the checker's lowering go through here, so "build a Bin node"
 * does not end up hand-written in seven or eight places. `type` is left for the checker to
 * fill (lowering that already knows the type fills it at the call site).
 *
 * @param a      Arena that owns the node.
 * @param op     Operator text (`"+"`, `"<"`, `"-"`, ...), with the arena's lifetime.
 * @param left   Left operand.
 * @param right  Right operand.
 * @param line   Source line number (1-based).
 * @return       The new `EX_BIN` node.
 */
Expr *exprBinNew(Arena *a, const char *op, Expr *left, Expr *right, int line) {
    Expr *e = exprNew(a, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = left;
    e->u.bin.right = right;
    return e;
}

/**
 * @brief Build a floating-point literal node.
 *
 * @param a      Arena that owns the node.
 * @param value  Literal value (also stored into `u.fval`, `f64`).
 * @param line   Source line number (1-based).
 * @return       The new `EX_FLOAT` node; `type` is left for the caller to fill.
 */
Expr *exprFloatNew(Arena *a, double value, int line) {
    Expr *e = exprNew(a, EX_FLOAT, line);
    e->u.fval = value;
    return e;
}

/**
 * @brief Build an integer literal node.
 *
 * @param a      Arena that owns the node.
 * @param value  Literal value (also stored into `u.ival`).
 * @param line   Source line number (1-based).
 * @return       The new `EX_INT` node; `type` is left for the caller to fill.
 */
Expr *exprIntNew(Arena *a, long long value, int line) {
    Expr *e = exprNew(a, EX_INT, line);
    e->u.ival = value;
    return e;
}

/**
 * @brief Build a boolean literal node (`true` / `false`).
 *
 * @param a      Arena that owns the node.
 * @param value  The literal's value.
 * @param line   Source line number (1-based).
 * @return       The new `EX_BOOL` node.
 */
Expr *exprBoolNew(Arena *a, bool value, int line) {
    Expr *e = exprNew(a, EX_BOOL, line);
    e->u.bval = value;
    return e;
}

/**
 * @brief Build an `EX_CALL` node with the callee filled in and the argument list initialized
 * empty.
 *
 * @param a       Arena that owns the node.
 * @param callee  Callee expression.
 * @param line    Source line number (1-based).
 * @return        The new `EX_CALL` node; `u.call.args` can be appended to with `vecPush`.
 */
Expr *exprCallNew(Arena *a, Expr *callee, int line) {
    Expr *e = exprNew(a, EX_CALL, line);
    e->u.call.callee = callee;
    vecInit(&e->u.call.args, a, sizeof(Expr *));
    return e;
}

/**
 * @brief Replace the index of an index expression in place (used by lowering: `out[i]` ->
 * `out[i - lo]`).
 *
 * Only the index slot is written; `obj`, `type`, and the other fields are untouched. The
 * caller is responsible for the new index having been checked.
 *
 * @param e      Must be an `EX_INDEX` node.
 * @param index  The new index expression.
 */
void exprIndexRetarget(Expr *e, Expr *index) {
    e->u.index.index = index;
}

/**
 * @brief Record the name of the module a `FuncDef` belongs to.
 *
 * Functions the checker synthesizes (the `par` worker) have no module context from parsing,
 * and this fills in the name that later diagnostics and emission need.
 *
 * @param f        Function definition to write.
 * @param modName  Module name, with the arena's lifetime; may be NULL.
 */
void funcDefSetModName(FuncDef *f, const char *modName) {
    f->modName = modName;
}

/**
 * @brief Mark or clear a statement's "parallel loop block" flag.
 *
 * Only the parser sets the flag; the checker clears it after lowering. The price of not
 * adding a `StmtKind` is this one boolean, which either side can write.
 *
 * @param s        Statement node to write.
 * @param parLoop  The new value.
 */
void stmtSetParLoop(Stmt *s, bool parLoop) {
    s->parLoop = parLoop;
}

/**
 * @brief Build a method call node `recv.name(args...)`.
 *
 * Only syntax facts are filled in (receiver, name, argument list): method resolution,
 * borrow checks, and arena bookkeeping all happen on the checker's `EX_METHOD` path, so the
 * checker can re-examine this node as if the user had written it.
 *
 * @param a     Arena to allocate from.
 * @param recv  Receiver expression.
 * @param name  Method name (statically stored).
 * @param line  Source line number.
 * @return      The new node, with `args` initialized empty.
 */
Expr *exprMethodNew(Arena *a, Expr *recv, const char *name, int line) {
    Expr *e = exprNew(a, EX_METHOD, line);
    e->u.method.recv = recv;
    e->u.method.name = name;
    vecInit(&e->u.method.args, a, sizeof(void *));
    return e;
}

/**
 * @brief Build a slice node `obj[lo..hi]`.
 *
 * `lo` / `hi` may be `NULL`, meaning that end was not written; for a fixed array the checker
 * fills an omitted end with a literal (`a[..]` -> `a[0..N]`), so the node has the same shape
 * as one the parser produced.
 *
 * @param a     Arena to allocate from.
 * @param obj   Expression being sliced.
 * @param lo    Lower bound, or `NULL`.
 * @param hi    Upper bound, or `NULL`.
 * @param line  Source line number.
 * @return      The new node.
 */
Expr *exprSliceNew(Arena *a, Expr *obj, Expr *lo, Expr *hi, int line) {
    Expr *e = exprNew(a, EX_SLICE, line);
    e->u.slice.obj = obj;
    e->u.slice.lo  = lo;
    e->u.slice.hi  = hi;
    return e;
}

/**
 * @brief Rewrite an expression **in place** into a module function call
 * `modPrefix::name(args...)` (`EX_ASSOC`).
 *
 * The value f-string's specifier slot is desugared by the parser into a call to a reserved
 * name, and the checker redirects it to `stl::stringio`'s `padI64` / `padF64` / `padText`
 * according to the value's static type: that layer knows both the value's type and the
 * specifier as written, and it is the only one that can pick the padding primitive. Once
 * redirected to an `EX_ASSOC`, module import checking, mangling, and plan-side bookkeeping
 * all take the existing free-function-call path.
 *
 * @param a          Arena to allocate from (for the empty targs table).
 * @param e          Expression node to rewrite (in place).
 * @param typeName   The type-name segment (such as `"stl"`).
 * @param modPrefix  Module prefix (such as `"std::text::stringio"`).
 * @param name       Function name (statically stored).
 * @param args       The new argument list (a vector of pointers).
 */
void exprRetargetAssoc(Arena *a, Expr *e, const char *typeName, const char *modPrefix,
                       const char *name, Vec *args) {
    e->kind = EX_ASSOC;
    e->u.assoc.typeName = typeName;
    e->u.assoc.modPrefix = modPrefix;
    e->u.assoc.name = name;
    e->u.assoc.args = *args;
    vecInit(&e->u.assoc.targs, a, sizeof(Type *));
    e->u.assoc.isCall = true;
    e->type = NULL;
}

/**
 * @brief Deep-copy an expression subtree (used by fusion lowering).
 *
 * See the declaration of the same name in `ast.h`: only subtrees of a pure expression shape
 * are fully copied, and other shapes degrade to a shallow copy.
 *
 * @param a    Arena to allocate from.
 * @param src  Source expression; NULL returns NULL.
 * @return     The new subtree.
 */
Expr *exprClone(Arena *a, Expr *src) {
    if (!src) return NULL;
    Expr *n = arenaAllocZero(a, sizeof(Expr));
    *n = *src;
    switch (src->kind) {
    case EX_BIN:
        n->u.bin.left  = exprClone(a, src->u.bin.left);
        n->u.bin.right = exprClone(a, src->u.bin.right);
        break;
    case EX_UN:
        n->u.un.operand = exprClone(a, src->u.un.operand);
        break;
    case EX_CONV:
        n->u.conv.operand = exprClone(a, src->u.conv.operand);
        break;
    case EX_FIELD:
        n->u.field.obj = exprClone(a, src->u.field.obj);
        break;
    case EX_INDEX:
        n->u.index.obj   = exprClone(a, src->u.index.obj);
        n->u.index.index = exprClone(a, src->u.index.index);
        break;
    case EX_SLICE:
        n->u.slice.obj = exprClone(a, src->u.slice.obj);
        n->u.slice.lo  = exprClone(a, src->u.slice.lo);
        n->u.slice.hi  = exprClone(a, src->u.slice.hi);
        break;
    case EX_SIGN:
        n->u.sign.operand = exprClone(a, src->u.sign.operand);
        break;
    case EX_COALESCE:
        n->u.coalesce.main     = exprClone(a, src->u.coalesce.main);
        n->u.coalesce.fallback = exprClone(a, src->u.coalesce.fallback);
        break;
    case EX_REF:
        n->u.ref.operand = exprClone(a, src->u.ref.operand);
        break;
    case EX_DEREF:
        n->u.deref.operand = exprClone(a, src->u.deref.operand);
        break;
    default:
        break;   /* leaves and "must not fuse" shapes: a shallow copy is enough */
    }
    return n;
}

/**
 * @brief Tell whether a function is a method.
 *
 * The rule is the name: a first parameter named `self` makes it a method; nothing else in a
 * declaration distinguishes a method from a free function.
 *
 * @param f  Function to inspect.
 * @return   `true` when the first parameter is named `self`; `false` when `f` is NULL or has
 *           no parameters.
 */
bool funcIsMethod(const FuncDef *f) {
    if (!f || f->params.len == 0) return false;
    /* vecAt takes a non-const Vec; here the elements are only read, never written. */
    Param *p0 = *(Param **)vecAt((Vec *)&f->params, 0);
    return strcmp(p0->name, "self") == 0;
}

/**
 * @brief Replace the statement list of a block statement (used by fusion lowering).
 *
 * @param s      Must be an `ST_BLOCK` node.
 * @param stmts  The new statement list (a `Vec` of `Stmt*`, copied in by value).
 */
void stmtBlockSetStmts(Stmt *s, Vec *stmts) {
    s->u.block.stmts = *stmts;
}

/**
 * @brief Build an `obj[index]` index node (used by lowering of whole-array expressions).
 *
 * @param a      Arena to allocate from.
 * @param obj    Expression being indexed.
 * @param index  Index expression.
 * @param line   Source line attached to the node.
 * @return       The new `EX_INDEX` node.
 */
Expr *exprIndexNew(Arena *a, Expr *obj, Expr *index, int line) {
    Expr *e = exprNew(a, EX_INDEX, line);
    e->u.index.obj   = obj;
    e->u.index.index = index;
    return e;
}

/**
 * @brief Build an `obj.name` field access node (used by lowering to read `v.len`).
 *
 * @param a     Arena to allocate from.
 * @param obj   Expression being accessed.
 * @param name  Field name (not copied).
 * @param line  Source line attached to the node.
 * @return      The new `EX_FIELD` node.
 */
Expr *exprFieldNew(Arena *a, Expr *obj, const char *name, int line) {
    Expr *e = exprNew(a, EX_FIELD, line);
    e->u.field.obj  = obj;
    e->u.field.name = name;
    return e;
}

/**
 * @brief Build a string literal node, copying the text into the arena.
 *
 * @param a     Arena to allocate from.
 * @param text  String contents.
 * @param line  Source line attached to the node.
 * @return      The new `EX_STR` node.
 */
Expr *exprStrNew(Arena *a, const char *text, int line) {
    Expr *e = exprNew(a, EX_STR, line);
    size_t n = strlen(text);
    char *copy = arenaAlloc(a, n + 1);
    memcpy(copy, text, n + 1);
    e->u.str.text = copy;
    return e;
}

/**
 * @brief Build a unary expression node.
 *
 * @param a        Arena to allocate from.
 * @param op       Operator text (statically stored).
 * @param operand  Operand.
 * @param line     Source line attached to the node.
 * @return         The new `EX_UN` node.
 */
Expr *exprUnNew(Arena *a, const char *op, Expr *operand, int line) {
    Expr *e = exprNew(a, EX_UN, line);
    e->u.un.op      = op;
    e->u.un.operand = operand;
    return e;
}

/**
 * @brief Build an `e!` node (the checker assigns the payload's type).
 *
 * @param a        Arena.
 * @param operand  Expression being signed.
 * @param line     Line number.
 * @return         The new signature expression.
 */
Expr *exprSignNew(Arena *a, Expr *operand, int line) {
    Expr *e = exprNew(a, EX_SIGN, line);
    e->u.sign.operand = operand;
    return e;
}

/**
 * @brief Build a local variable declaration (`let`/`var x: ann = init`).
 *
 * The type annotation may be `NULL` (inferred from the initializer), giving the same shape as
 * a declaration the parser produced. Temporaries the compiler creates itself (the early
 * unwrapping of a value-position `?`, and so on) go through here, so `u.var` is not written
 * outside the builder.
 *
 * @param a     Arena to allocate from.
 * @param name  Variable name; must live at least as long as this module.
 * @param ann   Type annotation; may be `NULL`.
 * @param init  Initializer; may be `NULL` (`var x: T`).
 * @param mut   `true` = `var`, `false` = `let`.
 * @param line  Line number.
 * @return      The new declaration statement.
 */
Stmt *stmtVarNew(Arena *a, const char *name, Type *ann, Expr *init, bool mut, int line) {
    Stmt *s = stmtNew(a, ST_VAR, line);
    s->u.var.name = name;
    s->u.var.ann  = ann;
    s->u.var.init = init;
    s->u.var.mut  = mut;
    return s;
}
/**
 * @brief Build an assignment statement (`target = value`; an empty `op` is a plain
 * assignment).
 *
 * @param a       Arena to allocate from.
 * @param target  Assignment target.
 * @param value   Right-hand side.
 * @param op      The compound assignment's operator text (such as `"+="`); `NULL` for a plain
 *                assignment.
 * @param line    Source line attached to the node.
 * @return        The new `ST_ASSIGN` node; `opExpr` is left for the checker.
 */
Stmt *stmtAssignNew(Arena *a, Expr *target, Expr *value, const char *op, int line) {
    Stmt *s = stmtNew(a, ST_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value  = value;
    s->u.assign.op     = op;
    s->u.assign.opExpr = NULL;
    return s;
}
/**
 * @brief Replace an assignment statement's right-hand side and operator (used by lowering
 * rewrites).
 *
 * @param s      The assignment statement (`ST_ASSIGN`).
 * @param value  The new right-hand side.
 * @param op     The new compound operator; NULL for a plain `=`.
 */
void stmtAssignSetValue(Stmt *s, Expr *value, const char *op) {
    if (!s || s->kind != ST_ASSIGN) return;
    s->u.assign.value = value;
    s->u.assign.op = op;
}


/**
 * @brief Build a `trap(msg)` statement.
 *
 * @param a     Arena to allocate from.
 * @param msg   Message expression.
 * @param line  Source line attached to the node.
 * @return      The new `ST_TRAP` node.
 */
Stmt *stmtTrapNew(Arena *a, Expr *msg, int line) {
    Stmt *s = stmtNew(a, ST_TRAP, line);
    s->u.trap_.msg = msg;
    return s;
}

/**
 * @brief Build an `if cond { thenBody } else { elseBody }` statement.
 *
 * @param a         Arena to allocate from.
 * @param cond      Condition expression.
 * @param thenBody  Statements run when the condition holds.
 * @param elseBody  Statements run when it does not; NULL means there is no `else`.
 * @param line      Source line attached to the node.
 * @return          The new `ST_IF` node.
 */
Stmt *stmtIfNew(Arena *a, Expr *cond, Stmt *thenBody, Stmt *elseBody, int line) {
    Stmt *s = stmtNew(a, ST_IF, line);
    s->u.ifs.cond     = cond;
    s->u.ifs.thenBody = thenBody;
    s->u.ifs.elseBody = elseBody;
    return s;
}

/**
 * @brief Build the desugared block of a `for var in 0..hi { body }` (used by lowering of
 * whole-array expressions).
 *
 * The shape matches the parser's range `for`: `[loop variable declaration, upper bound
 * declaration, while]`, where the `while` body is `[body, step]` with `forStep` attached.
 * The checker's range-variable widening and the `par` lowering both recognise this shape.
 *
 * @param a     Arena to allocate from.
 * @param var   Loop variable name.
 * @param hi    Upper bound expression (half-open range).
 * @param body  Loop body (a block).
 * @param line  Source line attached to the node.
 * @return      The outer block statement.
 */
Stmt *stmtRangeForNew(Arena *a, const char *var, Expr *hi, Stmt *body, int line) {
    Stmt *decl = stmtNew(a, ST_VAR, line);
    decl->u.var.name      = var;
    decl->u.var.ann       = NULL;
    decl->u.var.init      = exprIntNew(a, 0, line);
    decl->u.var.mut       = true;      /* the loop itself writes it */
    decl->u.var.overwrite = false;

    Stmt *hiDecl = stmtNew(a, ST_VAR, line);
    hiDecl->u.var.name      = "__extc_hi";
    hiDecl->u.var.ann       = NULL;
    hiDecl->u.var.init      = hi;
    hiDecl->u.var.mut       = false;
    hiDecl->u.var.overwrite = false;

    Expr *cond = exprBinNew(a, "<", exprIdent(a, var, line), exprIdent(a, "__extc_hi", line), line);
    Stmt *step = stmtAssignNew(a, exprIdent(a, var, line), exprIntNew(a, 1, line), "+=", line);

    Stmt *loopBody = stmtNew(a, ST_BLOCK, line);
    vecInit(&loopBody->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&loopBody->u.block.stmts) = body;
    *(Stmt **)vecPush(&loopBody->u.block.stmts) = step;
    loopBody->forStep = step;          /* label target for `continue` */

    Stmt *w = stmtNew(a, ST_WHILE, line);
    w->u.whiles.cond = cond;
    w->u.whiles.body = loopBody;

    Stmt *outer = stmtNew(a, ST_BLOCK, line);
    vecInit(&outer->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&outer->u.block.stmts) = decl;
    *(Stmt **)vecPush(&outer->u.block.stmts) = hiDecl;
    *(Stmt **)vecPush(&outer->u.block.stmts) = w;
    return outer;
}

/* ---------------------------------------------------------------- walking */

/**
 * @brief Call the expression callback on one subexpression; no callback or a NULL
 * subexpression counts as passing.
 *
 * @param v  The callback and its context.
 * @param e  Subexpression to visit; NULL, or no expression callback set, returns `true`
 *           directly.
 * @return   The callback's result; `false` stops the whole walk.
 */
static bool visitExpr(const AstVisit *v, Expr *e) {
    if (!e || !v->expr) return true;
    return v->expr(v->ctx, e);
}
/**
 * @brief Call the statement callback on one substatement; with no statement callback set it
 * keeps going down into that node's children.
 *
 * @param v  The callback and its context.
 * @param s  Substation to visit; NULL returns `true` directly.
 * @return   The callback's result; `false` stops the whole walk.
 */
static bool visitStmt(const AstVisit *v, Stmt *s) {
    if (!s) return true;
    if (v->stmt) return v->stmt(v->ctx, s);
    return astWalkStmtChildren(s, v);
}
/**
 * @brief Call the expression callback on every element of an expression vector, in order.
 *
 * @param v   The callback and its context.
 * @param xs  Expression vector (`Expr *` elements).
 * @return    `true` when everything was visited; `false` as soon as some callback returns
 *            `false`.
 */
static bool visitExprList(const AstVisit *v, Vec *xs) {
    for (size_t i = 0; i < xs->len; i++)
        if (!visitExpr(v, *(Expr **)vecAt(xs, i))) return false;
    return true;
}

/**
 * @brief Visit the direct children of expression `e`.
 *
 * The callback visits a **child** and never the node it was called for; returning false from
 * the callback stops the whole walk. With no expression callback set it returns `true`
 * immediately (skipped).
 *
 * @param e  Expression to walk; NULL returns `true` directly.
 * @param v  The callbacks and their context.
 * @return   `true` when the walk completed; `false` when a callback returned false.
 */
/*@@all-kinds*/   bool astWalkExprChildren(Expr *e, const AstVisit *v) {
    if (!e) return true;
    switch (e->kind) {
    /* two children */
    case EX_BIN:      return visitExpr(v, e->u.bin.left) && visitExpr(v, e->u.bin.right);
    case EX_INDEX:    return visitExpr(v, e->u.index.obj) && visitExpr(v, e->u.index.index);
    case EX_COALESCE: return visitExpr(v, e->u.coalesce.main) &&
                             visitExpr(v, e->u.coalesce.fallback);
    /* one child */
    case EX_UN:    return visitExpr(v, e->u.un.operand);
    case EX_REF:   return visitExpr(v, e->u.ref.operand);
    case EX_DEREF: return visitExpr(v, e->u.deref.operand);
    case EX_SIGN:  return visitExpr(v, e->u.sign.operand);
    case EX_CONV:  return visitExpr(v, e->u.conv.operand);
    case EX_TRY:   return visitExpr(v, e->u.try_.operand);
    case EX_FIELD: return visitExpr(v, e->u.field.obj);
    case EX_NEW:   return visitExpr(v, e->u.new_.count);
    case EX_DYN:   return visitExpr(v, e->u.dynv.payload);
    case EX_EXT:   return visitExpr(v, e->u.ext_.call);
    case EX_LAMBDA:   /* the body, plus the env field values: those reads happen where the lambda is written */
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (!visitExpr(v, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return false;
        return visitStmt(v, e->u.lambda.body);
    /* a receiver, or a list */
    case EX_METHOD:  return visitExpr(v, e->u.method.recv) && visitExprList(v, &e->u.method.args);
    /* The callee counts as a child only when it is not a **declaration name**: walking into a
     * direct call `g(x)` would report the function as "used as a value"; calling through a
     * value `f(x)` (`f` holds a `fn`) reads that binding, and must be walked like any other
     * child. */
    case EX_CALL:
        return (!e->u.call.callee || e->u.call.callee->kind != EX_IDENT ||
                visitExpr(v, e->u.call.callee)) &&
                            visitExprList(v, &e->u.call.args);
    case EX_ASSOC:   return visitExprList(v, &e->u.assoc.args);
    case EX_GENCALL: return visitExprList(v, &e->u.gencall.args);
    case EX_ENUMVAL: return visitExprList(v, &e->u.enumval.args);
    case EX_ARRAYLIT:return visitExprList(v, &e->u.arraylit.elems);
    case EX_SLICE:   return visitExpr(v, e->u.slice.obj) && visitExpr(v, e->u.slice.lo) &&
                            visitExpr(v, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (!visitExpr(v, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return false;
        return true;
    /* no children */
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_IDENT: case EX_NULL:
        return true;
    }
    return true;
}

/**
 * @brief Visit the direct children of statement `s`.
 *
 * Same convention as `astWalkExprChildren`: the callback visits a child, and returning false
 * stops the whole walk. With no statement callback set it keeps going down into the node's
 * children.
 *
 * @param s  Statement to walk; NULL returns `true` directly.
 * @param v  The callbacks and their context.
 * @return   `true` when the walk completed; `false` when a callback returned false.
 */
/*@@all-kinds*/   bool astWalkStmtChildren(Stmt *s, const AstVisit *v) {
    if (!s) return true;
    switch (s->kind) {
    case ST_VAR:    return visitExpr(v, s->u.var.init);
    case ST_ASSIGN: return visitExpr(v, s->u.assign.target) && visitExpr(v, s->u.assign.value);
    case ST_RETURN: return visitExpr(v, s->u.ret.value);
    case ST_YIELD:  return visitExpr(v, s->u.yield_.value);
    case ST_TRAP:   return visitExpr(v, s->u.trap_.msg);
    /* `d.run { … }`: the block is the domain's body, so both children are walked. */
    case ST_DOMAIN: return visitExpr(v, s->u.domain_.callee) && visitStmt(v, s->u.domain_.body);
    case ST_EXPR:   return visitExpr(v, s->u.expr.expr);
    case ST_IF:     return visitExpr(v, s->u.ifs.cond) &&
                           visitStmt(v, s->u.ifs.thenBody) && visitStmt(v, s->u.ifs.elseBody);
    case ST_WHILE:  return visitExpr(v, s->u.whiles.cond) && visitStmt(v, s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (!visitStmt(v, *(Stmt **)vecAt(&s->u.block.stmts, i))) return false;
        return true;
    case ST_MATCH:
        if (!visitExpr(v, s->u.match.scrutinee)) return false;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (!visitStmt(v, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return false;
        return true;
    case ST_BREAK: case ST_CONTINUE: return true;   /* no children */
    }
    return true;
}

/**
 * @brief Build a second struct definition that shares `src`'s fields and methods under another name.
 *
 * @param a     Arena to allocate from.
 * @param src   The struct to copy.
 * @param name  The C name, also used as the source name, of the copy.
 * @return      The copy.
 */
StructDef *structDefNamedCopy(Arena *a, const StructDef *src, const char *name) {
    StructDef *out = arenaAllocZero(a, sizeof *out);
    *out = *src;
    out->name = name;
    out->srcName = name;
    return out;
}

/**
 * @brief Build a parameter that carries `src`'s identity but another type.
 *
 * @param a     Arena to allocate from.
 * @param src   The parameter to copy.
 * @param type  The type of the copy.
 * @return      The copy.
 */
Param *paramWithType(Arena *a, const Param *src, Type *type) {
    Param *out = arenaAllocZero(a, sizeof *out);
    *out = *src;
    out->type = type;
    return out;
}
