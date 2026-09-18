/* extC 的 AST。
 *
 * week-0 就建**完整 AST**（不是 token 流重写）。
 * AST 本身不难 —— 难的是绕过它之后，arena 检查、符号表、好诊断全都没处放。
 */
#ifndef EXTC_AST_H
#define EXTC_AST_H

#include "base.h"

/* ---------------------------------------------------------------- 类型 */

typedef struct Type Type;
struct Type {
    bool        isRef;
    const char *name;    /* isRef == false */
    Type       *inner;   /* isRef == true  */
};

Type *typeName(Arena *a, const char *name);
Type *typeRef(Arena *a, Type *inner);
Type *typeBase(Type *t);                 /* 剥掉所有 ref */
void  typeRender(const Type *t, Buf *out);

/* ---------------------------------------------------------------- 表达式 */

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STR, EX_IDENT,
    EX_BIN, EX_UN, EX_CALL, EX_METHOD, EX_FIELD, EX_STRUCTLIT
} ExprKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int      line;
    union {
        long long ival;
        double    fval;
        bool      bval;
        struct { const char *text; } str;                 /* 不含引号，转义原样 */
        struct { const char *name; } ident;
        struct { const char *op; Expr *left, *right; } bin;
        struct { const char *op; Expr *operand; } un;
        struct { Expr *callee; Vec args; } call;          /* args: Expr* */
        struct { Expr *recv; const char *name; Vec args; } method;
        struct { Expr *obj; const char *name; } field;
        struct { const char *name; Vec inits; } lit;      /* inits: FieldInit* */
    } u;
};

typedef struct { const char *name; Expr *value; } FieldInit;

Expr *exprNew(Arena *a, ExprKind kind, int line);

/* ---------------------------------------------------------------- 语句 */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK
} StmtKind;

typedef struct Stmt Stmt;

struct Stmt {
    StmtKind kind;
    int      line;
    union {
        struct { const char *name; Type *ann; Expr *init; bool mut; } var;
        struct { Expr *target; Expr *value; } assign;
        struct { Expr *cond; Stmt *thenBody; Stmt *elseBody; } ifs;
        struct { Expr *cond; Stmt *body; } whiles;
        struct { Expr *value; } ret;
        struct { Expr *expr; } expr;
        struct { Vec stmts; } block;                      /* stmts: Stmt* */
    } u;
};

Stmt *stmtNew(Arena *a, StmtKind kind, int line);

/* ---------------------------------------------------------------- 顶层 */

typedef struct {
    const char *name;
    Type       *type;
    int         line;
} Param;

typedef struct {
    const char *name;
    Type       *type;
    int         line;
} FieldDef;

typedef struct {
    const char *name;
    Vec         fields;     /* FieldDef* */
    int         line;
} StructDef;

typedef struct {
    const char *name;
    Vec         params;     /* Param* */
    Type       *ret;        /* NULL 表示无返回值 */
    Stmt       *body;       /* ST_BLOCK */
    int         line;
} FuncDef;

typedef struct {
    Vec structs;            /* StructDef* */
    Vec funcs;              /* FuncDef*  */
} Module;

void moduleInit(Module *m, Arena *a);

/* 首参数名为 `self` 的函数就是方法 */
bool funcIsMethod(const FuncDef *f);

#endif /* EXTC_AST_H */
