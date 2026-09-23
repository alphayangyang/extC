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
    vecInit(&m->uses, a, sizeof(void *));
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
bool funcIsMethod(const FuncDef *f) {
    if (!f || f->params.len == 0) return false;
    Param *p0 = *(Param **)vecAt((Vec *)&f->params, 0);
    return strcmp(p0->name, "self") == 0;
}
