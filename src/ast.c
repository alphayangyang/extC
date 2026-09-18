#include "ast.h"

#include <string.h>

Type *typeName(Arena *a, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->isRef = false;
    t->name = name;
    t->inner = NULL;
    return t;
}

Type *typeRef(Arena *a, Type *inner) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->isRef = true;
    t->name = NULL;
    t->inner = inner;
    return t;
}

Type *typeBase(Type *t) {
    while (t && t->isRef) t = t->inner;
    return t;
}

void typeRender(const Type *t, Buf *out) {
    if (!t) { bufPuts(out, "void"); return; }
    if (t->isRef) {
        bufPuts(out, "ref ");
        typeRender(t->inner, out);
        return;
    }
    bufPuts(out, t->name);
}

Expr *exprNew(Arena *a, ExprKind kind, int line) {
    Expr *e = (Expr *)arenaAllocZero(a, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    return e;
}

Stmt *stmtNew(Arena *a, StmtKind kind, int line) {
    Stmt *s = (Stmt *)arenaAllocZero(a, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

void moduleInit(Module *m, Arena *a) {
    vecInit(&m->structs, a, sizeof(void *));
    vecInit(&m->funcs, a, sizeof(void *));
}

bool funcIsMethod(const FuncDef *f) {
    if (!f || f->params.len == 0) return false;
    Param *p0 = *(Param **)vecAt((Vec *)&f->params, 0);
    return strcmp(p0->name, "self") == 0;
}
