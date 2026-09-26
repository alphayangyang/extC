/* Construction of AST nodes, plus the one structural predicate that needs nothing
 * but the tree itself.
 *
 * Every constructor takes a zeroed node from the arena and sets only the kind and
 * the line number, so a field the parser never fills in is zero rather than
 * indeterminate.
 */

#include "ast.h"

#include <string.h>

/* Create an unresolved type name.
 *
 * Params:
 *   a    - arena that owns the node
 *   name - the type name as written in the source; the pointer is kept as it is,
 *          the text is not copied
 *
 * Returns:
 *   A TY_UNRESOLVED type. Resolving it into an interned type, including any type
 *   arguments the parser attaches afterwards, is the checker's job.
 */
Type *typeNamed(Arena *a, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_UNRESOLVED;
    t->name = name;
    return t;
}

/* Create `ref inner`, the plain reference type.
 *
 * Params:
 *   a     - arena that owns the node
 *   inner - the referenced type
 *
 * Returns:
 *   A TY_REF type that is neither writable nor nullable; the parser sets `mut` or
 *   `nullable` when the source says `mut ref` or `?ref`.
 */
Type *typeRef(Arena *a, Type *inner) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/* Create the fixed array type `[n]elem`.
 *
 * Params:
 *   a    - arena that owns the node
 *   n    - element count; it is a compile-time constant and part of the type, so
 *          `[3]i32` and `[4]i32` are different types
 *   elem - element type
 *
 * Returns:
 *   A TY_ARRAY type.
 */
Type *typeArray(Arena *a, int64_t n, Type *elem) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_ARRAY;
    t->asize = n;
    t->inner = elem;
    return t;
}

/* Create a reference to a generic parameter by name.
 *
 * Params:
 *   a    - arena that owns the node
 *   name - the parameter name, such as "T"
 *   idx  - its position in the enclosing parameter list
 *
 * Returns:
 *   A TY_PARAM type.
 */
Type *typeParam(Arena *a, const char *name, int idx) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_PARAM;
    t->param = name;
    t->name = name;
    t->tpIndex = idx;
    return t;
}

/* Create an expression node.
 *
 * Params:
 *   a    - arena that owns the node
 *   kind - the expression kind; only the matching member of the union may be used
 *   line - 1-based source line, kept for diagnostics
 *
 * Returns:
 *   A zeroed Expr. The fields that hold the results of resolution -- type, func,
 *   field, and the escape-analysis depths -- are filled in later by the checker.
 */
Expr *exprNew(Arena *a, ExprKind kind, int line) {
    Expr *e = (Expr *)arenaAllocZero(a, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->storedAt = -1;      /* "has this value been seen being published?" is not answered
                            * yet, and 0 is a real answer (it must outlive the frame) */
    return e;
}

/* Create a statement node.
 *
 * Params:
 *   a    - arena that owns the node
 *   kind - the statement kind; only the matching member of the union may be used
 *   line - 1-based source line, kept for diagnostics
 *
 * Returns:
 *   A zeroed Stmt.
 */
Stmt *stmtNew(Arena *a, StmtKind kind, int line) {
    Stmt *s = (Stmt *)arenaAllocZero(a, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

/* Prepare an empty module.
 *
 * Params:
 *   m - module to initialise; the caller owns the storage
 *   a - arena the vectors allocate from
 *
 * Notes:
 *   - Declarations are appended as the parser and the loader find them, so one
 *     Module can end up holding the prelude, then the user's file, then every
 *     module that file imported, in that order.
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

/* Is this function a method?
 *
 * Returns:
 *   True when the first parameter is named `self`. That name is the entire rule:
 *   nothing else in a declaration distinguishes a method from a free function.
 *
 * Notes:
 *   - The const is cast away only because vecAt takes a non-const Vec; the element
 *     is read and never written.
 */
Expr *exprIdent(Arena *a, const char *name, int line) {
    Expr *e = exprNew(a, EX_IDENT, line);
    e->u.ident.name = name;
    e->u.ident.srcName = name;      /* the parser's `forIdent` set both; one builder sets both */
    return e;
}

bool funcIsMethod(const FuncDef *f) {
    if (!f || f->params.len == 0) return false;
    Param *p0 = *(Param **)vecAt((Vec *)&f->params, 0);
    return strcmp(p0->name, "self") == 0;
}

/* ---------------------------------------------------------------- walking */

static bool visitExpr(const AstVisit *v, Expr *e) {
    if (!e || !v->expr) return true;
    return v->expr(v->ctx, e);          /* false = stop the whole walk */
}
static bool visitStmt(const AstVisit *v, Stmt *s) {
    if (!s) return true;
    if (v->stmt) return v->stmt(v->ctx, s);
    return astWalkStmtChildren(s, v);   /* no statement callback: keep descending */
}
static bool visitExprList(const AstVisit *v, Vec *xs) {
    for (size_t i = 0; i < xs->len; i++)
        if (!visitExpr(v, *(Expr **)vecAt(xs, i))) return false;
    return true;
}

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
    /* a receiver, or a list */
    case EX_METHOD:  return visitExpr(v, e->u.method.recv) && visitExprList(v, &e->u.method.args);
    case EX_CALL:    return visitExprList(v, &e->u.call.args);
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

/*@@all-kinds*/   bool astWalkStmtChildren(Stmt *s, const AstVisit *v) {
    if (!s) return true;
    switch (s->kind) {
    case ST_VAR:    return visitExpr(v, s->u.var.init);
    case ST_ASSIGN: return visitExpr(v, s->u.assign.target) && visitExpr(v, s->u.assign.value);
    case ST_RETURN: return visitExpr(v, s->u.ret.value);
    case ST_YIELD:  return visitExpr(v, s->u.yield_.value);
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
