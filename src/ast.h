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

/* ⭐ 定案 68：**"分配到本函数的家 arena"这个决定，用一个哨兵表示** ——
 * 它跟 `homeDepth` 里那个 `-1` 是同一个意思，但这两个字段现在**都由检查器填**、
 * codegen 只翻译 ⇒ 别再靠"函数有没有家"去反推 ✗（那就是"两个权威"）✓ */
#define ARENA_HOME (-1)

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
    /* ⭐ 定案 63（PLAN #38）+ **定案 68**：**这个 `new` 分配进哪一只 arena？**
     *
     * ⭐ **层号的唯一权威**（2026-09-22 主人拍板：「checker 操作完之后应该把全部生成信息
     * 给 codegen，codegen 就不用再做校验」）⇒ 这个数由**检查器算定**，
     * codegen 只负责翻译成 C 文本（`arenaRefAt`），**不再自己判断 `hasHome`** ✗
     *
     * 默认 = 语句所在的块深度（A2 按块细化：出块就回收 ✓）；
     * 逃逸检查发现"这个东西被存进了活得更久的地方" ⇒ **提升**到那一层 ✓
     *
     *     ARENA_HOME（= -1）= 本函数的**家** arena（`*__extc_home`，调用者选的那只）✓
     *     >0 = 本函数第 k 层块 —— 出那块时 release ✓
     *     0  = "还没定"（只在检查过程中出现；codegen 永远看不到它）✓
     *
     * ⚠️ **闭包之后要再定一次**：`needsHome` 的传递闭包是**所有函数体查完之后**才算的，
     * 而查体时 `c->curFunc->needsHome` 只有"直接判据"（体里有 `new` + 返回含引用）✗
     * ⇒ 那些"因为调用了有家函数才有家"的函数里，检查器当时算出的是**块层**，
     *   而生成的 C 按"有家"分配（家更长命 ⇒ 只会更安全 ⇒ 以前**没有洞**，但**有两个权威**）✗
     * ⇒ 现在 `checkModule` 在闭包跑完之后把这些函数里每个 `new` 站点改写成 `ARENA_HOME`
     *   （站点记在 `FuncDef.newSites` 里，见 `check_top.c`）✓✓
     *
     * 只有 `EX_NEW` 走这条路（`alloc<T>(n)` = 当前块，见它自己的那一支 ——
     * 它也是检查器算的，只是不参与"提升"/"家"）✓
     * 见 `DECISIONS.md` 定案 63 / 定案 68、`check_escape.c` 的 `promoteInto` ✓ */
    int       arenaLevel;
    /* ⭐ 定案 65：这个 `new` 是 `@overwrite` 的站点 —— **存储只有一块**
     * （函数帧那层、懒分配、每次执行清零复用）⇒ 层数按"函数体"算，不是按语句所在块 ✓ */
    bool      reuse;
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
     * 取值依据：**最浅的那个 `mut ref` 实参**所指对象住在哪只 arena 里
     * （"新东西的寿命跟着你给我的那条链走" —— ARENA.md §1.2 那条规则 ✓）
     *
     * ⚠️ 这个字段是给**检查器自己**用的（算规则 ④ 的 `h`，故意保守：
     *    `0` 那一档按深度 0 查 ⇒ 宁可误拒 ✓）。
     *    **codegen 不看它** —— codegen 看 `arenaArg`（已解析好的那一个）✓ */
    int       homeDepth;
    /* ⭐ 定案 68：**调用点最终传哪只 arena** —— 由检查器算定，codegen 只翻译 ✓
     *     ARENA_HOME（= -1）⇒ `__extc_home`（我的家，祖先那只）✓
     *     >=1              ⇒ `&__extc_a[这个块层]` ✓
     * 跟 `homeDepth` 的关系：`homeDepth` 是"保守判据"（0 那一档按 0 查，防漏 UB），
     * 这里是"实际执行的选择" ⇒ 两个数**故意可以不同**，而且都在检查器里定完 ⇒
     * codegen 一句判断都不用做（以前它会看 `g->hasHome` 兜底 ✗）✓ */
    int       arenaArg;
    /* ⭐ 定案 68：这一处**还没定**（检查时只算出"按当前块"）——
     * "我有家就传家"这一条要看 `needsHome` 的**传递闭包**（查完所有函数体才有）
     * ⇒ 只能等 `checkModule` 的收尾 pass 再把它改成 `ARENA_HOME` ✓
     * （`arenaArg` 此刻记着"当前块层号"，万一那个 pass 判出"没家"就照它用 ✓）*/
    bool      arenaArgPending;
    /* ⭐ 定案 70：这个引用是**限定名改写来的**吗？（`io::readLine` ⇒ 平名字）
     * 装载器只在这一路上置位 ⇒ 检查器拿它区分"用户写了限定名"与"用户漏了限定名" ✓ */
    bool      qualified;
    /* 显式转换：要不要**运行时检查**（整数收窄 / 换符号 / 浮点转整数）？
     * 能证明装得下就不查（P：编译期能证明的运行时不留痕迹）✓ */
    bool      convCheck;

    union {
        long long ival;
        double    fval;
        bool      bval;
        struct { const char *text; } str;                 /* 不含引号，转义原样 */
        struct { const char *name;
                 /* ⭐ 模块 mangle 前**源码里写的那个名字**（诊断要回显它 ✓）
                  * 为什么必须单独存一个：`name` 会被装载器改成 mangle 名
                  * （`open` → `lib$open`），而报错得指着用户写的那个词 ——
                  * 不留它，消息就变成"请写 `lib::lib$open`"，纯属胡说 ✗（真踩过）*/
                 const char *srcName;
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
                 const char *cname;
                 /* ⭐ 定案 65：`@overwrite var n = new T` —— **复用一块存储**：
                  * 在函数帧那层分配一次、每次执行到这句就清零复用 ✓（懒分配 ✓）
                  * 只对 `new` 合法（检查器保证）⇒ 见 DECISIONS 定案 65 ✓ */
                 bool overwrite; } var;
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
    /* ⭐ 定案 70（模块）：来自哪个文件 / 哪个模块 / 是不是 `@private` ✓ */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int          line;
};

struct StructDef {
    const char *name;
    Vec         typeParams;      /* const char* —— 泛型参数名，如 "T"；空 = 非泛型 */
    Vec         fields;          /* FieldDef* */
    Vec         methods;         /* FuncDef* —— 方法写在 struct 体内（定案 9） */
    Type       *type;            /* 非泛型 struct 的驻留类型（泛型见 ttGeneric） */
    bool        reserved;        /* 来自 prelude —— 不许用户重定义，也不许加方法 */
    /* ⭐ 定案 70（模块）：来自哪个文件 / 哪个模块 / 是不是 `@private` ✓ */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
};

struct FuncDef {
    const char *name;
    /* ⭐ PLAN #47：**泛型自由函数**（`fn f<T, U>(…)`）—— 模板自己的类型参数 ✓
     * （方法的类型参数在 `owner->typeParams` 里；这个字段只在自由函数上用 ✓）
     * 实例：`tmpl` 指回模板、`targs` 是这个实例的实参、`instName` 是它的 C 名字 ✓ */
    Vec         typeParams;      /* const char* */
    Vec         targs;           /* Type* —— 只有**实例**有 */
    FuncDef    *tmpl;            /* 非 NULL = 这是个实例（不是模板）✓ */
    const char *instName;        /* 实例的 C 名字（`max_i32`）✓ */
    Vec         params;          /* Param* */
    Type       *ret;             /* NULL 表示无返回值 */
    Stmt       *body;            /* ST_BLOCK */
    StructDef  *owner;           /* 方法所属的 struct；自由函数为 NULL */
    bool        isAssoc;         /* 写在 struct 体内但**不带 `self`** —— 关联函数 */
    /* ⭐ 定案 72（`LIBS.md` LQ3）：**外部声明**（`extern!("libc") fn …`）——
     * C 那边是黑盒 ⇒ 谁碰了它谁就得**签字**：
     *   hasEffects  —— 写了 `effects Addr=… Cont=…`（= 我保证它不干别的）
     *   没写        —— **按最坏情况算**（每个参数都可能被存下来 ⇒ 几乎不可用但安全 ✓）
     *   owned       —— 返回的内存归我 ⇒ **v1 直接报错**（要等"帧拥有资源"那套 ✓）*/
    bool        isExtern;
    const char *externLib;       /* `extern!("libc")` 里的那个名字（诊断用 ✓）*/
    bool        hasEffects;
    unsigned    extAddrMask, extContMask;
    bool        reserved;        /* 来自 prelude */
    /* ---- A3 逃逸提升：这个函数要不要一只"家"arena？----
     * 规则：**函数体里有分配，且返回类型含引用/视图** ⇒ 它多收一个隐藏参数
     * `extc_arena *__extc_home`，里面的 `new` 分配到**调用者选的那只** arena ✓
     * （调用点为此传 `__extc_home` 或 `&__extc_a[当前块]`）
     * 传递闭包也要（调用它的人得有东西可传）⇒ 见 check.c 里的不动点计算 ✓ */
    bool        needsHome;
    /* ⭐ 编译时长优化（2026-09-21）：**这个函数会不会往自己的块 arena 里放东西？**
     * 不会 ⇒ 连 `extc_arena __extc_a[N]` 和那串 `extc_arena_release` 都**不用吐** ✓
     * （真实例子里约一半函数属于这种"纯计算"函数，而 arena 样板占生成 C 的 ~22% 行 ✓）
     * 判据：体里有 `new`，或者调用了"有家"的函数（那种调用会写进我的块 arena）✓
     * ⚠️ 必须在 `needsHome` 的**传递闭包跑完之后**算（不然会漏掉"我调的人有家"）✓ */
    bool        mayUseArena;
    /* ⭐ 定案 68：**本函数体里"等闭包之后再定一次"的节点**（按检查顺序，`Expr*`）：
     *   · `EX_NEW`  ⇒ 有家 ⇒ `arenaLevel = ARENA_HOME`（每处分配都进家）✓
     *   · 调用点     ⇒ `arenaArgPending` 的那些 ⇒ 有家 ⇒ `arenaArg = ARENA_HOME` ✓
     * `needsHome` 的传递闭包跑完之后由 `checkModule` 的收尾 pass 处理 ✓
     * 为什么要有这张表：闭包是**查完所有函数体之后**才算的，那时再回头遍历 AST
     * 就要**再写一遍表达式遍历器**（这种"跟着 AST 形状走的 switch"每多一个都是一个
     * 会忘的地方 ✗ —— 本项目已经有 5 个了）⇒ 改成"查体时顺手记下标"✓ */
    Vec         arenaSites;
    /* ⭐ 定案 65（`@overwrite`）：本函数体里有几个复用站点，以及**格子放哪**。
     * 格子的寿命必须 ≥ "该站点被复用的整段过程"：
     *   · 站点在 `main` 里、或本函数**能（传递地）调到自己**（递归）
     *     ⇒ 格子放**自己的帧**（递归各激活一块 ✓ 否则子调用会踩父激活的存储 ✗）
     *   · 否则 ⇒ 格子由**调用点的帧**持有、当隐藏参数传进来（`void **` 不透明 ✓）
     *     —— 只有这样"在 callee 里 `new`、循环在 caller"才真的复用得上 ✓
     *     （那正是主人提的那个泄漏形状 ✗）*/
    int         owSites;
    bool        owLocal;
    /* ⭐ 甲′（PLAN #31/#33）：**这个函数（传递地）会不会分配？**
     *
     * 为什么要单独一个字段：`needsHome` 的传递闭包是**所有函数查完之后**才跑的，
     * 而调用者在查自己函数体的时候就得知道"这一刀会不会往我的容器里塞新存储" ✗
     * ⇒ 按**名字/AST**惰性算一遍并缓存：0 = 还没算，1 = 会，2 = 不会，3 = **正在算**（环保护，
     * 环上保守地当"会" ✓）见 check_top.c 的 `funcAllocates` ✓ */
    int         allocState;
    /* ⭐ PLAN #22：这个函数（传递地）**会不会打印**？
     * 用来判断"同一语句里更靠前的调用有没有可观测的副作用"——`??` 的临时变量会跳到它前面 ✗
     * 0 = 没算，1 = 会，2 = 不会，3 = 正在算（环保护 ⇒ 当"会" ✓）*/
    int         mayPrintState;
    /* ⭐ PLAN #42(c)（2026-09-22）：**这个方法/函数被调用过吗？**
     * 泛型实例化现在**只复查/生成"真被调到的"方法** ✓
     * 理由：实例化过去会把类型的**所有**方法体都按实例复查一遍 ✗ ⇒
     *   `slice<T>::==` 从没被用到，却要求 `T: ==` ⇒ 于是**任何 struct 进容器都要写一句 `fn ==`** ✗✗
     * ⚠️ 这是**保守近似**（"模板体里被调过"就算用过 ⇒ 宁可多查多生成 ✓ 不能少 ✓）： */
    bool        used;
    /* ⭐ 档1（ARENA-FORMAL §3.4）：**效果摘要** —— 这个函数往它的" mut ref "
     * 参数里存了什么？位 i = 第 i 个参数 ✓
     *   addrMask  : 存了「实参 j 的地址/字段地址」（地址流 ⇒ 要 `R_slot(arg_j) ⊒ H`）
     *   contMask  : 存了「从实参 j 读出来的指针」（内容流 ⇒ 要 `ρ_j ⊒ H`）
     *   otherMask : 装不了引用却能流出去的东西（拿不准 ⇒ 保守）
     * 还有 addrFromLocal：存了「本帧局部的地址」（那类调用点本来就该被挡）✓ */
    unsigned    addrMask, contMask, otherMask;      /* 目的地 = 形参所指的容器 */
    unsigned    homeAddrMask, homeContMask;         /* 目的地 = 本函数**新分配**的对象（家内存）*/
    unsigned    freshCount;                         /* 只是统计：这次收集看到几个 fresh 局部 */
    /* ⭐ 1.2a（PLAN-REGION §6）：摘要的**传递闭包**状态
     *   0 = 还没算，1 = 算完了（effComplete 说它可不可信），3 = **正在算**（环保护）
     *   effUnknown：有没有"解析不出来"的调用（那摘要就永远不完整 ⇒ 保守）✓ */
    int         effState;
    bool        effComplete;
    bool        effUnknown;

    bool        addrFromLocal;
    Vec         callees;      /* FuncDef*：它调了谁（画调用图用，§8.5 的 SCC）*/
    /* ⭐ 定案 70（模块）：这个声明**来自哪个文件**、属于哪个模块？
     *   ctx       —— 它那个文件的 Ctx（报错要走它：才能指对文件、印对源码行 ✓）
     *   modName   —— 模块短名（`use foo` 里的 `foo`）；根文件 = NULL；prelude = NULL
     *   isPrivate —— `@private`：别的模块引用它 ⇒ **编译期报错** ✓（默认公开 ✓）*/
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
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
    /* ⭐ 定案 70（模块）：这个声明**来自哪个文件**、属于哪个模块？
     *   ctx       —— 它那个文件的 Ctx（报错要走它：才能指对文件、印对源码行 ✓）
     *   modName   —— 模块短名（`use foo` 里的 `foo`）；根文件 = NULL；prelude = NULL
     *   isPrivate —— `@private`：别的模块引用它 ⇒ **编译期报错** ✓（默认公开 ✓）*/
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
} GlobalDef;

/* ⭐ 定案 70（2026-09-22，主人拍板）：**语义导入**（不是 C 的文本包含 ✗）
 *     use std::io        ⇒ 装载器去找 std/io.extc、解析它，
 *                          并在**检查之前**把本文件里 `io::name` 解析成平名字 ✓
 *     短名 = 路径最后一段（v1 不做 `as` 别名）；环 = 编译期错误 ✓ */
typedef struct { const char *from; const char *to; } Alias;

typedef struct {
    const char *path;      /* "std::io"（原样，报错用）*/
    const char *shortName; /* "io" —— 引用时写 `io::name` ✓ */
    const char *file;      /* 装载器填：解析到的文件 */
    int         line;
} UseDecl;

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*（type 枚举） */
    Vec funcs;                   /* FuncDef*  */
    Vec globals;                 /* GlobalDef* —— 顶层 let / var */
    Vec uses;                    /* UseDecl* —— 定案 70 */
    /* ⭐ 模块 mangle 的**裸名回程票**（`pair` → `liba$pair`）：由装载器填，`ttResolve` 查 ✓ */
    Vec aliases;                 /* Alias* */
} Module;

void moduleInit(Module *m, Arena *a);

/* 首参数名为 `self` 的函数就是方法 */
bool funcIsMethod(const FuncDef *f);

#endif /* EXTC_AST_H */
