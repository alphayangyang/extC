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
    TY_ARRAY,       /* 固定数组：`[15]i32` —— 长度是类型的一部分 */
    TY_ERROR        /* 类型检查失败时的哑类型：抑制级联报错 */
} TypeKind;

struct Type {
    TypeKind    kind;
    const char *name;    /* TY_UNRESOLVED / TY_BUILTIN / TY_STRUCT / TY_ENUM；
                          * TY_GENERIC 时是**修饰过的名字**（见 ttMangle） */
    Type       *inner;   /* TY_REF */
    bool        nullable;/* TY_REF：`?ref T` —— **可能为空**（`null` 是它的零值，见 DECISIONS 定案 ㊻）。
                          * 非空时零值不存在；可空时零值 = null。
                          * `?T`（非 ref）在 parser 里就变成 `option<T>`，不走这个标记。 */
    bool        mut;     /* TY_REF：可写？
                          * **只读是默认**（安全是默认）；`mut ref T` 才是可写。
                          * 见 DECISIONS「引用语义定案」与 REFS.md §3。 */
    StructDef  *sdef;    /* TY_STRUCT / TY_GENERIC */
    TypeDef    *edef;    /* TY_ENUM */
    Vec         targs;   /* TY_GENERIC：类型实参（Type*） */
    const char *param;   /* TY_PARAM：参数名，如 "T" */
    int         tpIndex; /* TY_PARAM：第几个参数 */
    int64_t     asize;   /* TY_ARRAY：长度（编译期常量） */
};

Type *typeNamed(Arena *a, const char *name);   /* TY_UNRESOLVED（可能带 targs）*/
Type *typeRef(Arena *a, Type *inner);
Type *typeParam(Arena *a, const char *name, int idx);
Type *typeArray(Arena *a, int64_t n, Type *elem);   /* TY_ARRAY */

/* ---------------------------------------------------------------- 表达式 */

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STR, EX_IDENT,
    EX_BIN, EX_UN, EX_CALL, EX_METHOD, EX_FIELD, EX_STRUCTLIT,
    EX_INDEX,     /* a[i] —— 索引 */
    EX_SLICE,     /* a[i..j] —— 切一个视图出来 */
    EX_ARRAYLIT,  /* [1, 2, 3] —— 数组字面量 */
    EX_REF,       /* ref x —— 取引用（T3：ref 从类型修饰升级成表达式） */
    EX_ASSOC,     /* option<i64>::some(x) —— 关联函数（不带 self 的函数） */
    EX_GENCALL,   /* alloc<i32>(n) —— 泛型调用（目前只有内置原语用） */
    EX_TRY,       /* e? —— 失败就顺着往上抛（只在三处语句位置上合法） */
    EX_DEREF,     /* `*p` —— 显式解引用：读=p指的值，写=p指的地方 */
    EX_ENUMVAL,   /* Status.warn —— 由 check 把 EX_FIELD 改写成这个 */
    EX_CONV,      /* `i32(x)` / `f64(y)` —— **显式转换**（PLAN #23）：
                   * extC 只自动做无损失拓宽，收窄/换符号/整数↔浮点都必须写出来 ✓
                   * 语法写成 `T(x)` 而不是 C 的 `(T)x`：extC 的 parser **不查符号表**，
                   * `(T)x` 会跟"括号表达式"二义 ✗（C 靠符号表才分得开）*/
    EX_NEW,       /* `new T` / `new T[n]` / `new [N]T` —— 从**当前块的 arena** 拿一块
                   * **清零**的地方（PLAN A1）。内容：
                   *   `new T`     ⇒ `mut ref T`（一个 T 的地方）
                   *   `new [N]T`  ⇒ `mut ref [N]T`
                   *   `new T[n]`  ⇒ `mut slice<T>`（n 个元素 —— 这就是"造 buffer"）✓ */
    EX_COALESCE,  /* `a ?? b` —— **可能没有就兜底**（`?` 那一族的第三个记号）：
                   *   `opt ?? 兜底`   ⇒ 有就给值，没有就给兜底
                   *   `r ?? 兜底`     ⇒ 成功给值，失败给兜底
                   *   `p ?? q`（?ref）⇒ null 就给另一个引用
                   * 语义 = `match a { 有(v) => v  _ => b }`，但**只算一边** ✓ */
    EX_SIGN,      /* `e!` —— **我签字**（定案 1.3 的 `!`）："
                   *   `opt!` / `r!`  ⇒ 直接给我载荷（编译期**不检查**有没有）；
                   *   `p!`（`?ref T`）⇒ 我知道非空，给我 `ref T` ✓
                   * 没有任何运行时痕迹 —— 签字的定义就是"错了算我的"（P′ 的对偶）*/
    EX_NULL       /* `null` —— **可空引用的零值**（`?ref T`）。
                   * 只能出现在「上下文已经说清楚是哪个 `?ref T`」的地方，
                   * 类型由 adoptContextType 寄放（跟 `[]` 数组字面量一个套路）✓ */
} ExprKind;

struct Expr {
    ExprKind kind;
    int      line;

    /* ---- 由类型检查 pass 填写 ---- */
    Type     *type;
    FuncDef  *func;     /* EX_CALL / EX_METHOD 解析到的函数；`==` 时是 eq 方法 */
    FieldDef *field;    /* EX_FIELD 解析到的字段 */
    Type     *assocOwner; /* EX_ASSOC：解析到的**实例**类型（用来修饰 C 名字） */
    bool      needEq;   /* `==` 的操作数含类型参数 → 推迟到实例化再检查 */
    bool      deref;    /* 这个表达式在**值位置**被用到，而它的类型是 `ref T`
                         * ⇒ 生成 `*(...)`。
                         * 这是形状 3「值位置自动解引用」的落点：类型检查阶段
                         * 把 `ref T` 当 `T` 用（权限由 `ref` / `mut ref` 承担），
                         * 代码生成阶段就补一次解引用。见 DECISIONS 引用语义定案。 */
    /* 逃逸检查用：这个表达式里的引用**指向的活物**有多深？
     *   0 = 参数 / 静态数据 / 未知（函数返回的引用由被调用者自己的检查担保）
     *   >0 = 某个局部变量的块深度
     * 只有类型里**含引用**的表达式才有意义。见 REFS.md §4。 */
    int       refDepth;
    /* 这个值里的引用是不是「**从外面借来的**」（参数来的、或函数调用回来的）？
     * 借来的东西不能存进比这次调用活得更长的地方 —— 编译器不知道它的真实寿命。
     * 这是 BOOTSTRAP §8 里那条 ④（参数洗白）。 */
    bool      borrowed;
    /* `??` 的主体**不是**"没有副作用的东西" ⇒ codegen 必须在所在语句之前
     * 吐一个临时变量（先求值一次），再对临时变量做三元。
     * 由**检查器**判定并标记（它已经算过 repeatablePure），codegen 只管照做 ✓
     * 标记的含义："两边都不能重复求值，主体只能算一次" */
    bool      needTemp;
    /* ---- A3 第二半（出参）：这个调用点该传哪只 arena 给"有家"的被调用者？----
     *   0  = 没标（退回老规则：我有家传家、没有传当前块）
     *  -1  = 传我的 `__extc_home`（祖先那只）
     *  >=1 = 传 `&__extc_a[这个块深度]` ✓
     * 取值依据：**最浅的那个 `mut ref` 实参**所指对象住在哪只 arena 里
     * （"新东西的寿命跟着你给我的那条链走" —— ARENA.md §1.2 那条规则 ✓）*/
    int       homeDepth;
    /* 显式转换：要不要**运行时检查**（整数收窄 / 换符号 / 浮点转整数）？
     * 能证明装得下就不查（P：编译期能证明的运行时不留痕迹）✓ */
    bool      convCheck;

    union {
        long long ival;
        double    fval;
        bool      bval;
        struct { const char *text; } str;                 /* 不含引号，转义原样 */
        struct { const char *name;
                 /* 解析到的绑定的 C 名字（遮蔽时会跟 `name` 不同）。
                  * 由类型检查阶段填 —— 名字的**解析**是检查器的活，
                  * 代码生成只管照着印。见 DECISIONS 定案 47。 */
                 const char *cname; } ident;
        struct { const char *op; Expr *left, *right; } bin;
        struct { const char *op; Expr *operand; } un;
        struct { Expr *callee; Vec args; } call;          /* args: Expr* */
        struct { Expr *recv; const char *name; Vec args; } method;
        struct { Expr *obj; const char *name; } field;
        struct { const char *name; Vec inits; } lit;      /* inits: FieldInit* */
        struct { Expr *obj; Expr *index; } index;
        struct { Expr *obj; Expr *lo; Expr *hi; } slice;   /* lo / hi 可为 NULL */
        struct { Vec elems; bool rest; } arraylit;         /* elems: Expr*；rest = 末尾有 ... */
        struct { Expr *operand; } ref;
        struct { Expr *operand; } deref;
        struct { Expr *operand; } sign;   /* `e!` —— 我签字 */
        struct { Expr *main, *fallback; } coalesce;   /* `a ?? b` */
        struct { const char *typeName; Type *type; Expr *count; } new_;  /* `new T[n]` */
        struct { const char *typeName; Type *type; Expr *operand; } conv; /* `i32(x)` */        /* 关联函数调用：`typeName<targs>::name(args)`
         * 写全类型是**故意**的 —— 不靠上下文猜（见 DECISIONS 定案 27）。 */
        struct { const char *typeName; Vec targs; const char *name; Vec args; } assoc;
        struct { Expr *operand; } try_;   /* `e?` */
        struct { const char *name; Vec targs; Vec args; } gencall;
        struct { const char *typeName; const char *variant; Vec args; } enumval;
        /*   ↑ args 空 = 无载荷变体（`status.ok`）；非空 = 带载荷构造（`shape.circle(2.0)`）*/
    } u;
};

typedef struct { const char *name; Expr *value; } FieldInit;

Expr *exprNew(Arena *a, ExprKind kind, int line);

/* ---------------------------------------------------------------- 语句 */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK, ST_MATCH
} StmtKind;

/* `match` 的一条分支：`circle => { ... }`
 * （带载荷之后会多一个「绑定载荷的名字」列表，见 IO.md / BOOTSTRAP §8 第 3 步。）*/
typedef struct {
    const char *variant;   /* 变体名；`_` 表示兜底（暂时不支持，先留着位置）*/
    Vec         binds;     /* const char* —— 绑定载荷的名字：`circle(r) => ...` 里的 `r` */
    Stmt       *body;
    int         line;
} MatchArm;

struct Stmt {
    StmtKind kind;
    int      line;

    Type    *type;      /* ST_VAR：变量声明的最终类型（由 check 填写） */

    union {
        struct { const char *name; Type *ann; Expr *init; bool mut;
                 /* 生成 C 时用的名字（同一层 `let` 遮蔽 ⇒ `a` → `a__2`）。
                  * 由类型检查阶段填，见 DECISIONS 定案 47。 */
                 const char *cname; } var;
        struct { Expr *target; Expr *value; } assign;
        struct { Expr *cond; Stmt *thenBody; Stmt *elseBody; } ifs;
        struct { Expr *cond; Stmt *body; } whiles;
        struct { Expr *value; } ret;
        struct { Expr *expr; } expr;
        struct { Vec stmts; } block;                      /* stmts: Stmt* */
        struct { Expr *scrutinee; Vec arms; } match;      /* arms: MatchArm* */
    } u;
};

Stmt *stmtNew(Arena *a, StmtKind kind, int line);

/* ---------------------------------------------------------------- 顶层 */

typedef struct {
    const char *name;
    const char *cname;   /* 生成 C 时用的名字（同上，见 DECISIONS 定案 47） */
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
    /* **载荷**：`| circle(f64) | rect(f64, f64)` 里括号中的类型（types: Type*）。
     * 空 = 无载荷变体（`| outOfRange`）。位置式，不带字段名 ——
     * `match` 绑定也是位置的：`circle(r) => ...` ✓ */
    Vec         types;
} Variant;

struct TypeDef {                 /* type status = | ok | warn | error */
    const char  *name;
    Vec          typeParams;     /* const char* —— 泛型参数名（`type option<T>`）*/
    Vec          variants;       /* Variant* */
    Type        *type;           /* 驻留后的类型，由 check 填写 */
    bool         reserved;       /* 来自 prelude —— 不许用户重定义 */
    int          line;
};

struct StructDef {
    const char *name;
    Vec         typeParams;      /* const char* —— 泛型参数名，如 "T"；空 = 非泛型 */
    Vec         fields;          /* FieldDef* */
    Vec         methods;         /* FuncDef* —— 方法写在 struct 体内（定案 9） */
    Type       *type;            /* 非泛型 struct 的驻留类型（泛型见 ttGeneric） */
    bool        reserved;        /* 来自 prelude —— 不许用户重定义，也不许加方法 */
    int         line;
};

struct FuncDef {
    const char *name;
    Vec         params;          /* Param* */
    Type       *ret;             /* NULL 表示无返回值 */
    Stmt       *body;            /* ST_BLOCK */
    StructDef  *owner;           /* 方法所属的 struct；自由函数为 NULL */
    bool        isAssoc;         /* 写在 struct 体内但**不带 `self`** —— 关联函数 */
    bool        reserved;        /* 来自 prelude */
    /* ---- A3 逃逸提升：这个函数要不要一只"家"arena？----
     * 规则：**函数体里有分配，且返回类型含引用/视图** ⇒ 它多收一个隐藏参数
     * `extc_arena *__extc_home`，里面的 `new` 分配到**调用者选的那只** arena ✓
     * （调用点为此传 `__extc_home` 或 `&__extc_a[当前块]`）
     * 传递闭包也要（调用它的人得有东西可传）⇒ 见 check.c 里的不动点计算 ✓ */
    bool        needsHome;
    /* ⭐ 甲′（PLAN #31/#33）：**这个函数（传递地）会不会分配？**
     *
     * 为什么要单独一个字段：`needsHome` 的传递闭包是**所有函数查完之后**才跑的，
     * 而调用者在查自己函数体的时候就得知道"这一刀会不会往我的容器里塞新存储" ✗
     * ⇒ 按**名字/AST**惰性算一遍并缓存：0 = 还没算，1 = 会，2 = 不会，3 = **正在算**（环保护，
     * 环上保守地当"会" ✓）见 check_top.c 的 `funcAllocates` ✓ */
    int         allocState;
    int         line;
};

/* 全局变量 / 常量（顶层 `let` / `var`）。
 *
 * **全局 = 深度 0** —— 它活得比谁都长。所以逃逸规则自动禁止把局部的东西存进全局
 * （`0 ≥ 1` 为假 ⇒ 编译错误），**不需要为全局写任何特殊规则**。
 * 定长的全局**不需要 arena**：它就是 C 的静态对象。 */
typedef struct {
    const char *name;
    Type       *ann;         /* 类型标注（可省，从初始化式推） */
    Expr       *init;        /* 初始化式；NULL = 零初始化 */
    bool        mut;         /* var = true */
    bool        reserved;
    int         line;
} GlobalDef;

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*（type 枚举） */
    Vec funcs;                   /* FuncDef*  */
    Vec globals;                 /* GlobalDef* —— 顶层 let / var */
} Module;

void moduleInit(Module *m, Arena *a);

/* 首参数名为 `self` 的函数就是方法 */
bool funcIsMethod(const FuncDef *f);

#endif /* EXTC_AST_H */
