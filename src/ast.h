/* extC 的 AST。
 *
 * 设计要点：**AST 是「带解析结果」的** —— 类型检查 pass 会把结果写回节点：
 *   Expr.type   表达式的类型
 *   Expr.func   调用解析到的函数
 *   Expr.field  字段访问解析到的字段
 *   Stmt.type   变量声明的最终类型
 * 这样代码生成就不需要任何类型推导逻辑了（T1 的目的）。
 */
#ifndef EXTC_AST_H
#define EXTC_AST_H

#include "base.h"

/* ---------------------------------------------------------------- 类型 */

typedef struct Type Type;
typedef struct TypeDef TypeDef;
typedef struct StructDef StructDef;
typedef struct FuncDef FuncDef;
typedef struct FieldDef FieldDef;
typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef enum {
    TY_UNRESOLVED,  /* parser 刚造出来的「类型名」，由 check 解析 */
    TY_VOID,
    TY_BUILTIN,     /* i32、bool、str… */
    TY_STRUCT,
    TY_ENUM,        /* type Status = | ok | warn */
    TY_REF,         /* ref T */
    TY_PARAM,       /* 泛型参数本身：模板里出现的 `T` */
    TY_GENERIC,     /* 泛型实例：`Pair<i32, u8>` */
    TY_ERROR        /* 类型检查失败时的哑类型：抑制级联报错 */
} TypeKind;

struct Type {
    TypeKind    kind;
    const char *name;    /* TY_UNRESOLVED / TY_BUILTIN / TY_STRUCT / TY_ENUM；
                          * TY_GENERIC 时是**修饰过的名字**（见 ttMangle） */
    Type       *inner;   /* TY_REF */
    StructDef  *sdef;    /* TY_STRUCT / TY_GENERIC */
    TypeDef    *edef;    /* TY_ENUM */
    Vec         targs;   /* TY_GENERIC：类型实参（Type*） */
    const char *param;   /* TY_PARAM：参数名，如 "T" */
    int         tpIndex; /* TY_PARAM：第几个参数 */
};

Type *typeNamed(Arena *a, const char *name);   /* TY_UNRESOLVED（可能带 targs）*/
Type *typeRef(Arena *a, Type *inner);
Type *typeParam(Arena *a, const char *name, int idx);

/* ---------------------------------------------------------------- 表达式 */

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STR, EX_IDENT,
    EX_BIN, EX_UN, EX_CALL, EX_METHOD, EX_FIELD, EX_STRUCTLIT,
    EX_REF,       /* ref x —— 取引用（T3：ref 从类型修饰升级成表达式） */
    EX_ENUMVAL    /* Status.warn —— 由 check 把 EX_FIELD 改写成这个 */
} ExprKind;

struct Expr {
    ExprKind kind;
    int      line;

    /* ---- 由类型检查 pass 填写 ---- */
    Type     *type;
    FuncDef  *func;     /* EX_CALL / EX_METHOD 解析到的函数；`==` 时是 eq 方法 */
    FieldDef *field;    /* EX_FIELD 解析到的字段 */
    bool      needEq;   /* `==` 的操作数含类型参数 → 推迟到实例化再检查 */

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
        struct { Expr *operand; } ref;
        struct { const char *typeName; const char *variant; } enumval;
    } u;
};

typedef struct { const char *name; Expr *value; } FieldInit;

Expr *exprNew(Arena *a, ExprKind kind, int line);

/* ---------------------------------------------------------------- 语句 */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK
} StmtKind;

struct Stmt {
    StmtKind kind;
    int      line;

    Type    *type;      /* ST_VAR：变量声明的最终类型（由 check 填写） */

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

struct FieldDef {
    const char *name;
    Type       *type;
    int         line;
};

typedef struct {
    const char *name;
    int         line;
} Variant;

struct TypeDef {                 /* type Status = | ok | warn | error */
    const char  *name;
    Vec          variants;       /* Variant* */
    Type        *type;           /* 驻留后的类型，由 check 填写 */
    int          line;
};

struct StructDef {
    const char *name;
    Vec         typeParams;      /* const char* —— 泛型参数名，如 "T"；空 = 非泛型 */
    Vec         fields;          /* FieldDef* */
    Vec         methods;         /* FuncDef* —— 方法写在 struct 体内（定案 9） */
    Type       *type;            /* 非泛型 struct 的驻留类型（泛型见 ttGeneric） */
    int         line;
};

struct FuncDef {
    const char *name;
    Vec         params;          /* Param* */
    Type       *ret;             /* NULL 表示无返回值 */
    Stmt       *body;            /* ST_BLOCK */
    StructDef  *owner;           /* 方法所属的 struct；自由函数为 NULL */
    int         line;
};

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*（type 枚举） */
    Vec funcs;                   /* FuncDef*  */
} Module;

void moduleInit(Module *m, Arena *a);

/* 首参数名为 `self` 的函数就是方法 */
bool funcIsMethod(const FuncDef *f);

#endif /* EXTC_AST_H */
