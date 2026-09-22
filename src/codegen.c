/* extC -> C 代码生成。
 *
 * T1 之后，这里**不做任何类型推理** —— 类型检查 pass 已经把结果写回 AST
 * （Expr.type / Expr.func / Expr.field / Stmt.type），这里只负责翻译。
 *
 * 生成的 C 是「无聊的 C」：没有宏魔法，没有优化花招。
 * `#line` 把 gcc 的报错映射回 .extc 的行号。
 */

#include "codegen.h"

#include <stdarg.h>
#include <stdlib.h>   /* exit：PLAN #27 的闭包超限要响亮报错 ✓ */
#include <stdio.h>
#include <string.h>

#include "types.h"

/* ---------------------------------------------------------------- 类型映射 */

typedef struct { const char *extc; const char *c; } TypeMap;

static const TypeMap C_TYPES[] = {
    { "i8",  "int8_t"  }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" },
    { "u8",  "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" },
    { "f32", "float"   }, { "f64", "double"  },
    { "bool", "bool"   },
    { NULL, NULL }
};

typedef struct { const char *extc; const char *fmt; const char *cast; } PrintFmt;

/* 「无格式串」的实现方式：格式由**编译器按静态类型选**，用户永远不写 "%d"。 */
static const PrintFmt PRINT_FMT[] = {
    { "i8",  "%d",   "(int)"       }, { "i16", "%d",   "(int)"       },
    { "i32", "%d",   "(int)"       }, { "i64", "%lld", "(long long)" },
    { "u8",  "%u",   "(unsigned)"  }, { "u16", "%u",   "(unsigned)"  },
    { "u32", "%u",   "(unsigned)"  }, { "u64", "%llu", "(unsigned long long)" },
    { "f32", "%g",   "(double)"    }, { "f64", "%g",   "(double)"    },
    { NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- 状态 */

typedef struct {
    Arena      *arena;
    Ctx        *ctx;
    Buf        *out;
    const char *path;
    bool        lineMap;

    TypeTable  *tt;
    Vec         structs;        /* StructDef* —— 非泛型 */
    Vec         funcs;          /* FuncDef*  —— 非泛型 struct 的方法 + 自由函数 */
    int         indent;

    /* 单态化上下文：生成某个泛型实例的代码时，把 T 换成实参 */
    Vec        *substParams;    /* const char* */
    Vec        *substArgs;      /* Type* */
    const char *ownerPrefix;    /* 方法名修饰用的实例名，如 "Pair_i32_u8" */

    /* 切片 helper（T5a-3）。见 SliceHelper 的注释：它们是**生成过程中才发现**
     * 需要的，所以函数体得先写进 body，最后再按「原型 → helper → 函数体」拼。 */
    Vec         helpers;        /* SliceHelper */
    Buf         body;

    /* `?` 展开要用：当前函数的返回类型（失败时要往上构造），
     * 以及临时变量编号（每个 `?` 一个，函数内唯一）。 */
    Type       *retType;
    int         tmpSeq;
    /* **语句前缀**（PLAN #19）：`??` 的主体不能重复求值时，先把它算进一个临时变量，
     * 那几行要吐在**所在语句之前**（C 没有语句表达式，只能这么干）。
     * `?` 早就在手写这套（genTryHead）；这里把它变成通用机制 ✓
     * `prefixBlk` = 现在在不在"语句里" —— 不在（比如文件作用域的初始化式）就不许吐 ✓ */
    Buf         prefix;
    int         prefixBlk;
    /* ---- arena 按**块**细化（PLAN A2）----
     * `__extc_a[k]` = 第 k 层块的 arena。k 是**编译期已知的块深度** ——
     * 跟检查器算引用的那个"词法深度"是同一个概念 ✓
     * `loopLevel[]` = 当前套着的各个循环体所在的块层（`break`/`continue` 要知道
     * 该释放到哪一层）*/
    int         blkLevel;
    bool        hasHome;   /* 当前函数有"家"arena ⇒ `new` 分配到 `*__extc_home`（A3）*/
    bool        noArena;   /* ⭐ 这个函数不会往自己的块 arena 里放东西
                            * ⇒ 连 `extc_arena __extc_a[N]` 和 release 都不吐 ✓
                            * （判据在检查器里算：`f->mayUseArena`，编译时长优化）*/
    int         loopLevel[64];
    int         loopLen;
    Vec         insts;          /* Type* —— **按 C 名字去重**后的泛型实例（见下）*/

    /* ---- ⭐ 描述表**按需**生成（编译时长优化第三步）----
     * 打印（以及以后的结构化 `==`）都要用它，但**只有真会被用到的类型**才需要 ✓
     * 收法：`descRef` 一边给出 `&X_desc` 一边把 X 登记进来 ⇒ 跑到不动点就是闭包；
     * 根 = `genPrint` 里那些 `extc_print(&x, &x_desc)` 的实参类型。
     *
     * ⚠️ 顺序上必须先**生成完函数体**才知道要哪些，所以描述区写在
     *    「原型区之后、函数体之前」（跟切片 helper 同一个套路 ✓）*/
    Vec         descs;          /* Type* —— 需要描述表的类型（按 C 名去重）*/
    Buf         desc;           /* 描述区（最后拼在 body 前面）*/
    Buf         rt;             /* 描述表类型 + 共享标量描述 —— 打印/比较任一需要就得有 ✓ */
    Buf         rtPrint;        /* `extc_print` —— 真打印过结构化类型才要 ✓ */
    Buf         rtEq;           /* `extc_eq` —— 真比过数组/切片才要 ✓ */
    /* ⭐ 第④步：**结构化 `==` 也走描述表** —— 这张表是"哪些类型要用 `extc_eq`"。
     * 闭包：容器要 eq ⇒ 元素也得能 eq（struct 就得给它生成一行适配器）✓ */
    Vec         eqNeed;         /* Type*（按 C 名去重）*/
    /* ⚠️ 判"要不要打印运行时"**不能**看 `descs.len`：
     *   `slice<u8>`（字符串字面量！）用的是**共享**的 `extc_desc_text`，
     *   压根不进 `descs` ⇒ 只看 descs 就会漏掉 `extc_print` / `extc_desc_text` ✗
     *   （真踩过：`println("x = ", n)` 这种最常见的一句就编不过）
     * 所以由 `genPrint` 直接举手 ✓ */
    bool        needRuntime;
} CG;

/* 一个切片 helper：把 `a[lo..hi]` 的边界检查＋视图构造收进一个函数。
 * 收进函数而不是写成表达式，是为了 **lo / hi 只求值一次**（表达式里 `hi - lo`
 * 会让两个界各出现两次）。 */
typedef struct {
    const char *name;
    char       *text;
} SliceHelper;

static void cgLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    for (int i = 0; i < g->indent; i++) bufPuts(g->out, "    ");
    bufPuts(g->out, tmp);
    bufPutc(g->out, '\n');
}

/* 往**语句前缀**里追加一行（缩进照旧，先攒着不输出）*/
static void pfLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    for (int i = 0; i < g->indent; i++) bufPuts(&g->prefix, "    ");
    bufPuts(&g->prefix, tmp);
    bufPutc(&g->prefix, '\n');
}

/* 把攒下的前缀吐出去 —— **必须在所在语句的第一行之前**调用 ✓
 * （调用点是程序里的"求值顺序保证"：前缀里的临时变量先算完，语句才开始 ✓）*/
static void flushPrefix(CG *g) {
    if (g->prefix.len == 0) return;
    bufPuts(g->out, bufCstr(&g->prefix));
    bufInit(&g->prefix, g->arena);
}

/* 单态化：把当前实例上下文里的 TY_PARAM 换成实参 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
}

static void substEnter(CG *g, Type *inst) {
    /* 泛型枚举实例没有 `sdef`（它的 owner 是 `edef`）*/
    g->substParams = inst->sdef ? &inst->sdef->typeParams : &inst->edef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
}

static void substLeave(CG *g) {
    g->substParams = NULL;
    g->substArgs   = NULL;
    g->ownerPrefix = NULL;
}


static const char *cType(CG *g, Type *t) {
    if (!t) return "void";

    /* 先**整体**替换一次 —— ref 和泛型实例里都可能嵌着 T。
     * （只处理裸 TY_PARAM 是不够的：`ref Pair<A, B>` 外层是 ref。）*/
    t = subst(g, t);

    if (t->kind == TY_PARAM) return "int";  /* 没有上下文可替换 —— 不该发生 */

    switch (t->kind) {
        case TY_REF:   return arenaPrintf(g->arena, "%s *", cType(g, t->inner));
        case TY_VOID:  return "void";
        case TY_STRUCT: return t->name;
        case TY_GENERIC: return t->name;    /* 已经是修饰过的名字 */
        case TY_ARRAY:  return t->name;     /* 同上：array_15_i32 */
        case TY_ENUM:  return t->name;      /* C 里就是一个 enum typedef */
        case TY_ERROR: return "int";
        case TY_BUILTIN:
            for (size_t i = 0; C_TYPES[i].extc; i++)
                if (strcmp(C_TYPES[i].extc, t->name) == 0) return C_TYPES[i].c;
            return "int";
        case TY_UNRESOLVED:
            return "int";       /* check 跑完之后不该出现 */
        case TY_PARAM:
            return "int";       /* 上面已经拦住了；这里只为消 -Wswitch */
    }
    return "int";
}

/* ---------------------------------------------------------------- 表达式 */

static bool isProtoType(Type *t, const char *name, size_t nargs);
static const char *genExpr(CG *g, Expr *e);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);

static bool isPlaceExpr(const Expr *e);

    
/* 方法在 C 里要加 struct 前缀 —— `Point_eq` 和 `Board_eq` 不能撞。
 * （在 extC 里它们本来就是两个不同的名字，见 DECISIONS 决策 9）*/
/* extC 的符号名 → C 标识符片段。
 * 运算符在 extC 里就叫 `==`，但 C 里不能这么拼，所以映射一下。
 *
 * 还有一类：**名字正好是 C 的关键字**（`fn double(...)` —— 写测试时就撞上了）。
 * extC 里 `double` 不是关键字（extC 的浮点是 `f64`），所以它是个完全合法的用户名字，
 * 但生成的 C 里 `int32_t double(int32_t);` 编不过，而且报错落在**生成的 C** 上，
 * 用户看不到自己的源码。加个 `__c` 后缀回避 ⇒ extC 源码里的名字一个字都不改 ✓
 * （关键字表在 base.c 的 `cIdentIsKeyword` —— 类型检查那边也要用同一份。）*/
/* 这个枚举有带载荷的变体吗？有 ⇒ C 里是 `struct { tag; union }` 而不是 `enum` */
static bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

static const char *cSymName(CG *g, const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq" }, { "!=", "ne" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    if (cIdentIsKeyword(name)) return arenaPrintf(g->arena, "%s__c", name);
    return name;
}

static const char *cFuncName(CG *g, FuncDef *f) {
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s", g->ownerPrefix, cSymName(g, f->name));
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(g, f->name));
    return cSymName(g, f->name);
}

/* 方法名修饰：接收者是泛型实例时用实例名（`Pair_i32_u8_getFirst`） */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s", rb->name, cSymName(g, f->name));
    /* ⚠️ 这里**不能**用 cFuncName —— 它带的是「当前正在生成的实例」前缀。
     * 被调用的方法可能属于另一个类型（在 Wrapper<Point> 里调 Point.==）。*/
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(g, f->name));
    return cSymName(g, f->name);
}

/* 一个类型是不是「字节视图」？是的话 `println` 按文本打印。
 *
 * 这是编译器知道的**唯一**一件跟 slice 有关的事，而且它是**输出约定**：
 * 「指向 byte 的视图」该按文本打印 —— 这是输出的基本操作，不是容器的实现。
 * 容器的定义、字段、方法全在 stdlib/prelude.extc 里（见 MIGRATION.md 的验收标准）。
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/* 视图的索引原语（每个视图类型一份）。
 *
 * **按值**收视图 —— 这样下标表达式只求值一次，而且不用为「值 / 引用」写两条路径。
 * 它带边界检查；越界就 trap（带 extC 的位置）。
 * prelude 里的 `get` / `==` / `find` 全都通过 `self[i]` 用它 ——
 * 也就是说**库里没有一行指针算术，也没有一行手工边界检查**（见 ARRAYS.md）。*/
/* 视图的下标原语：**返回指针**，调用点再解引用（`(*v_index(...))`）。
 *
 * 为什么不是按值返回？两个都会炸：
 *   ① `ref` 参数要 lvalue —— prelude 里 `slice<T>::==` 写 `self[i] != other[i]`，
 *      而 `fn ==(self: ref T, ...)` 收 `ref`，`&(按值返回的临时值)` 在 C 里非法，
 *      于是 `slice<struct>` 的比较根本编不出来（真 bug）。
 *   ② 赋值 `s[i] = x` / `s[i].field = x` 也需要 lvalue。
 * 返回指针两条都解决，而且视图本来就只有一种引用语义（`data: ref T` 里那个 `ref`）。 */
static void genViewIndexer(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    substEnter(g, inst);
    cgLine(g, "static inline %s *%s_index(%s v, int64_t i, const char *file, int line) {",
           cType(g, elem), inst->name, inst->name);
    g->indent++;
    cgLine(g, "if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);");
    cgLine(g, "return &v.data[i];");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g);
}

/* ---------------------------------------------------------------- 类型描述表
 *
 * 打印**不再派生代码**：每类型只出一份 `static const ExtcDesc`（数据 ✓），
 * 全程序共用一个 `extc_print`（运行时那一段，在生成文件开头）✓
 *
 * （旧方案见 git 历史：`genStructDebug` 给每个 struct 派生一整段 printf 序列。
 *   合成压测程序里那样堆出 2028 个 `_debug`、占生成 C 的 22.9% 行，而**一次都没打印** ✗）
 *
 * `descRef` 把 extC 类型映射到"描述它的那份常量"的地址：
 *   · 标量 / ref / slice<u8> ⇒ 全程序共享的那几只，不占额外空间 ✓
 *   · struct / 泛型实例 / 数组 / 枚举 ⇒ 自己的 `<C 名>_desc`
 *
 * ⚠️ 描述表之间会**互相引用**（`struct s { xs: slice<s> }` 就是一个环）——
 * 所以每个描述常量都在区首先有一句 `static const ExtcDesc X_desc;`
 * （C 的 tentative definition），于是定义顺序彻底无关 ✓ */
/* ⭐ **按需**：登记"这个类型需要一份描述"。按 **C 名**去重（可写视图/只读视图
 * 是同一个 C 结构体 ⇒ 一份 ✓）。标量 / `ref` / `slice<u8>` 用共享的那几只，不登记 ✓ */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/* ⭐ **按需**：登记"这个类型要用 `extc_eq`"（第④步）。按 C 名去重 ✓
 * 容器（数组/切片）比的时候元素也要能比 ⇒ 闭包在 `emitDescRegion` 里跑 ✓ */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

static const char *descRef(CG *g, Type *t) {
    if (!t) return "&extc_desc_i32";
    needDesc(g, t);                       /* ← 顺手登记依赖：这就是可达性闭包 ✓ */
    if (t->kind == TY_BUILTIN) return arenaPrintf(g->arena, "&extc_desc_%s", t->name);
    if (t->kind == TY_REF)     return "&extc_desc_ref";
    if (isByteView(t))         return "&extc_desc_text";
    /* 其余都有自己的一份（泛型实例用**自己的 C 名**：list_i32_desc）*/
    return arenaPrintf(g->arena, "&%s_desc", t->name);
}

/* struct 的描述：字段名 + **offsetof**（漏一个字段、算错一次布局，都编不过 ✓）
 * 显示名用 extC 原名字（泛型实例打的是**模板名**：`varArray { … }`，跟以前一致）*/
static void genStructDesc(CG *g, const char *cname, const char *disp, StructDef *sd,
                          const char *eqFn) {
    size_t n = sd->fields.len;
    if (n) {
        cgLine(g, "static const ExtcField %s_fields[] = {", cname);
        g->indent++;
        for (size_t i = 0; i < n; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            cgLine(g, "{ \"%s\", offsetof(%s, %s), %s },", fd->name, cname, fd->name,
                   descRef(g, subst(g, fd->type)));
        }
        g->indent--;
        cgLine(g, "};");
    }
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_STRUCT, \"%s\", sizeof(%s), %zu, %s, NULL, %s };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_fields", cname) : "NULL",
           eqFn ? eqFn : "NULL");
}

/* 数组：count 个 elem，**没有名字**（打印就是 `[1, 2, 3]` ✓）*/
static void genArrayDesc(CG *g, Type *arr) {
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
}

/* 视图（非字节）：一片元素 ⇒ `[a, b]`；字节视图共用 `extc_desc_text`，没有自己的一份 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
}

/* 枚举：只印**变体名**（定案 11），表外值印 `<类型名>` —— 跟旧的 `_name` 函数逐字节一致 ✓ */
static void genEnumDesc(CG *g, const char *cname, const char *disp, TypeDef *td) {
    size_t n = td->variants.len;
    if (n) {
        Buf b;
        bufInit(&b, g->arena);
        for (size_t j = 0; j < n; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&b, ", ");
            bufPrintf(&b, "\"%s\"", v->name);
        }
        cgLine(g, "static const char *const %s_variants[] = { %s };", cname, bufCstr(&b));
    }
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ENUM, \"%s\", sizeof(%s), %zu, %s, NULL };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_variants", cname) : "NULL");
}

/* ------------------------------------------------------------------ 按需出描述表
 *
 * ⭐ 第三步的关键：**只有真会被打印的类型**才出描述 ✓
 *    （合成压测程序 N=1000：一个结构体都没打印 ⇒ 描述区整块为空 ✓）
 *
 * 根 = `genPrint` 里 `extc_print(&x, &x_desc)` 的实参类型（`descRef` 登记的）；
 * 闭包 = 描述之间互相引用（struct 字段 / 数组元素 / 切片元素）⇒ 跑到不动点 ✓
 *
 * ⚠️ 两个必须交代的东西：
 *  ① **顺序**：要哪些描述得先**生成完函数体**才知道（`genPrint` 在函数体里），
 *     而 C 要求"先定义后使用" ⇒ 描述区写在「原型区之后、函数体之前」，
 *     最后跟切片 helper 一样拼回去 ✓
 *  ② **环**：`struct s { xs: slice<s> }` ⇒ `s_desc → slice_s_desc → s_desc`。
 *     所以先出**全部**前向声明（C 的 tentative definition，合法 ✓），
 *     定义顺序就无关了 —— 为此得先"空跑一遍"把集合收全（阶段 A）✓
 *     空跑没有副作用（那几个 emitter 只写 `g->out` + arena）✓
 */
static bool eqNeeded(CG *g, Type *t);
static void closeEqNeeds(CG *g);
static FuncDef *findOpMethod(Type *t, const char *sym, const char *fallback);
static void genEqAdapter(CG *g, Type *t, FuncDef *m);

static void emitDescDefs(CG *g) {
    /* ⚠️ 循环条件每次重读 `len`：`descRef` 会在定义过程中追加新依赖 ✓ */
    for (size_t i = 0; i < g->descs.len; i++) {
        Type *t = *(Type **)vecAt(&g->descs, i);
        /* 这一格 eq 只有"真被 `extc_eq` 递归到的 struct"才填（其余留 NULL ✓）*/
        const char *eqFn = eqNeeded(g, t) ? arenaPrintf(g->arena, "%s_eqD", t->name)
                                          : NULL;
        if (t->kind == TY_STRUCT && t->sdef) {
            genStructDesc(g, t->name, t->name, t->sdef, eqFn);
        } else if (t->kind == TY_ENUM && t->edef) {
            genEnumDesc(g, t->name, t->name, t->edef);
        } else if (t->kind == TY_ARRAY) {
            genArrayDesc(g, t);
        } else if (t->kind == TY_GENERIC && t->sdef) {
            substEnter(g, t);              /* 字段里的 T 要换成实参 ✓ */
            if (isView(t)) genViewDesc(g, t);
            else           genStructDesc(g, t->name, t->sdef->name, t->sdef, eqFn);
            substLeave(g);
        } else {
            /* 漏了一种就**响亮地炸** —— 静默少出一个描述等于生成的 C 里
             * 引用一个不存在的符号 ✗（项目纪律：不许静默少生成）*/
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: no descriptor generator for this type");
        }
    }
}

/* 这个类型要不要生成 / 填 `eq`？（按 C 名查 ✓）*/
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/* eq 的**闭包**：容器要 eq ⇒ 元素也得能 eq ✓
 * （struct 到此为止 —— 它靠 `d->eq` 委托给用户写的 `fn ==`，不再往下走 ✓）*/
static void closeEqNeeds(CG *g) {
    for (size_t i = 0; i < g->eqNeed.len; i++) {   /* 循环条件重读：会追加 ✓ */
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind == TY_ARRAY) { needEq(g, t->inner); continue; }
        if (t->kind == TY_GENERIC && isView(t) && !isByteView(t))
            needEq(g, *(Type **)vecAt(&t->targs, 0));
    }
}

static void emitDescRegion(CG *g) {
    /* 没人打印结构化类型、也没比过数组 ⇒ 描述表/打印/比较整块都不要 ✓ */
    if (g->descs.len == 0 && g->eqNeed.len == 0) return;
    closeEqNeeds(g);
    Buf *saved = g->out;
    g->out = &g->desc;
    emitDescDefs(g);                       /* 阶段 A：空跑，把依赖收全（输出丢掉）*/
    bufInit(&g->desc, g->arena);

    /* ---- 结构化 `==` 的**适配器**（必须在描述之前：描述里要取它的地址 ✓）
     * 只有"真被 `extc_eq` 递归到的 struct"才需要 —— 那是用户写的 `fn ==`，
     * 是任意代码，只能委托 ✓ 其余 kind 由 `extc_eq` 自己递归 ✓ */
    for (size_t i = 0; i < g->eqNeed.len; i++) {
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind != TY_STRUCT && t->kind != TY_GENERIC) continue;
        if (t->kind == TY_GENERIC && isView(t)) continue;   /* 视图由 extc_eq 自己递归 ✓ */
        FuncDef *m = findOpMethod(t, "==", NULL);
        if (!m) {
            /* 到不了这里（检查器已经保证"元素可比"才让比）——
             * 真到了就是**响亮地炸**，不许生成一个编不过的 C ✗ */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: structural equality needs a `fn ==` on this type");
            continue;
        }
        genEqAdapter(g, t, m);
    }
    if (g->eqNeed.len) cgLine(g, "");

    cgLine(g, "/* ---- 类型描述表（**按需**：只出真会被用到的那些）---- */");
    for (size_t i = 0; i < g->descs.len; i++)   /* 全部前向声明 ⇒ 定义顺序无关 ✓ */
        cgLine(g, "static const ExtcDesc %s_desc;", (*(Type **)vecAt(&g->descs, i))->name);
    cgLine(g, "");
    emitDescDefs(g);                       /* 阶段 B：真正出定义 */
    cgLine(g, "");
    g->out = saved;
}

/* 打印相关的**派生函数**现在全没了 ✓
 * （以前这里是 `_debug` / `_writeText` / `_name` 三个派生器：
 *   每个类型一份 printf 序列 —— 合成压测程序里堆出 2028 个、占生成 C 的 22.9% 行，
 *   而那个程序一次都没打印过 ✗。现在只剩描述数据 + 一个 `extc_print` ✓）
 */

/* C 原生就能比的类型：数值 / bool / 枚举 */
static bool nativeCmp(Type *t) {
    if (!t) return false;
    if (t->kind == TY_ENUM) return true;
    return t->kind == TY_BUILTIN;
}

/* `==` 找的是用户定义的 `fn ==(...)`（`!=` 没定义就退回用 `==` 取反） */
static FuncDef *findOpMethod(Type *t, const char *sym, const char *fallback) {
    Type *b = ttBase(t);
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return NULL;
    StructDef *sd = b->sdef;
    FuncDef *hit = NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, sym) == 0) return m;
        if (fallback && strcmp(m->name, fallback) == 0) hit = m;
    }
    return hit;
}

/* ⚠️ 旧的 `typeHasEq`（"元素能不能比，递归判断"）**没了** ——
 * 第④步之后 codegen 不再为数组派生 `_eq`，所以这个判据在 codegen 这一侧
 * 没有用户了；"能不能比"由**检查器**判（不通过就报
 * `` [2]p` cannot be compared: its element type `p` does not define `==` ``）✓
 * 两处判据以前必须**手动保持一致**（PLAN #20 那个真 bug 就是这么来的），
 * 现在只剩一处 ⇒ 那个不同步的坑从结构上没了 ✓ */

/* ---------------------------------------------------------------- 结构化 `==`
 *
 * ⭐ 第④步：以前给**每个数组类型**派生一份 `<T>_eq`（压测：200 个数组类型
 * ⇒ 2450 行代码，占生成 C 的 32% ✗）；现在一律走**通用** `extc_eq` + 描述表 ✓
 *
 * ⚠️ 要**地址**（`extc_eq(a, b, desc)` 是泛型实现）⇒ 实参得是地方；
 *    不是就先落进临时变量（语句前缀）—— 跟 `println` 那边同一个理由 ✓
 */
static const char *eqOperand(CG *g, Expr *x, Type *t) {
    const char *code = genExpr(g, x);
    if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
    const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);
    return arenaPrintf(g->arena, "&%s", tmp);
}

static const char *genEqCall(CG *g, Expr *e, Type *arr) {
    /* ⚠️ 顺序：左右各求值一次、**按源码顺序**（前缀行也是）✓ */
    const char *l = eqOperand(g, e->u.bin.left, arr);
    const char *r = eqOperand(g, e->u.bin.right, arr);
    return arenaPrintf(g->arena, "extc_eq(%s, %s, %s)", l, r, descRef(g, arr));
}

/* struct 的 `==` 适配器：把"用户写的 `fn ==`"接到 `extc_eq` 的
 * `bool (*)(const void *, const void *)` 上（只有 struct 需要 ——
 * 那是**任意代码**，只能委托 ✓）。
 *
 * ⚠️ `&` 与否要跟旧 `genEqTest` 一模一样：看那个方法**每一个形参**是不是 `ref`。 */
static void genEqAdapter(CG *g, Type *t, FuncDef *m) {
    const char *tn = cType(g, t);
    cgLine(g, "static bool %s_eqD(const void *a, const void *b) {", t->name);
    g->indent++;
    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    const char *a = p0->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)a", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)a", tn);
    const char *b = p1->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)b", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)b", tn);
    cgLine(g, "return %s(%s, %s);", cMethodName(g, t, m), a, b);
    g->indent--;
    cgLine(g, "}");
}

/* 元素比较的 C 表达式（递归：内建直接比，struct 走它的 `==`）
 * ⚠️ 第④步之后**没有调用者了** —— 数组的 `==` 改走 `genEqCall`（通用 `extc_eq`），
 * 而 struct/slice 的 `==` 走下面那一大段（用户方法的调用点，不经这里）✓ */

/* 二元运算。
 * `==` / `!=` 在 check 里被解析成 eq 方法调用（找不到 eq 就报错）；
 * 泛型里含类型参数的比较被推迟到这里，用实例上下文再解析一次。 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;

    /* 数组：编译器派生的 `==` / `!=`（数组没有 sdef，找不到方法）。
     *
     * ⭐ 第④步之后**不再为每个数组类型派生 `_eq` 函数** —— 改成通用的
     * `extc_eq(&a, &b, &arr_desc)`：一张描述表 + 一份递归实现 ✓
     * （语义一模一样：逐元素递归；元素是 struct 就调用户写的 `fn ==` ✓）
     *
     * 这里**不能**加 `!e->needEq` 的条件 —— 泛型里的比较推迟到实例化才解析，
     * 代入实参后元素类型可能正好是数组（`slice<[6]i32>` 里比较两行就是）。
     * 之前挡着，于是那种情况掉进下面的方法查找、报
     * 「`array_6_i32` needs to define `!=`」—— 一个假错误。 */
    if (!e->func) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ARRAY &&
            (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
            needEq(g, lt);                 /* 登记：这个类型要能用 extc_eq ✓ */
            const char *call = genEqCall(g, e, lt);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
    }

    if (e->func || e->needEq) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        FuncDef *m = e->func
                     ? e->func
                     : findOpMethod(lt, op, strcmp(op, "!=") == 0 ? "==" : NULL);

        if (!m) {
            /* 内建数值 / bool / 枚举：C 原生就能比，不需要 eq */
            if (nativeCmp(lt))
                return arenaPrintf(g->arena, "(%s %s %s)",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right));

            ctxError(g->ctx, e->line, 1,
                     "`==` inside a generic is checked at instantiation, not on the template -- the price of having no traits. "
                     "Add a `fn ==` to that type.",
                     "`%s` needs to define `%s`", cType(g, lt), op);
            return "0";
        }

        Param *p0 = *(Param **)vecAt(&m->params, 0);
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        const char *l = genExpr(g, e->u.bin.left);
        const char *r = genExpr(g, e->u.bin.right);
        if (p0->type->kind == TY_REF) l = arenaPrintf(g->arena, "&(%s)", l);
        if (p1->type->kind == TY_REF) r = arenaPrintf(g->arena, "&(%s)", r);

        const char *call = arenaPrintf(g->arena, "%s(%s, %s)",
                                       cMethodName(g, e->u.bin.left->type, m), l, r);
        return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
    }

    /* **算术不静默**（LANGUAGE.md §0.5）：除零 / 移位超宽在 C 里是 UB。
     * 我们让它们 **trap 带源码位置** —— 而且每个操作数**只求值一次**
     * （所以走 helper，不用 `(check(r), l op r)` 那种会求值两次的逗号表达式）✓ */
    {
        Type *lt   = ttBase(subst(g, e->u.bin.left->type));
        bool  sint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'i';
        bool  uint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'u';

        if ((sint || uint) && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)) {
            const char *fn = strcmp(op, "/") == 0 ? (sint ? "extc_divI" : "extc_divU")
                                                  : (sint ? "extc_modI" : "extc_modU");
            const char *ity = sint ? "int64_t" : "uint64_t";
            return arenaPrintf(g->arena, "(%s)%s((%s)(%s), (%s)(%s), \"%s\", %d)",
                               cType(g, lt), fn, ity, genExpr(g, e->u.bin.left),
                               ity, genExpr(g, e->u.bin.right), g->path, e->line);
        }
        if ((sint || uint) && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)) {
            int bits = 0;
            for (const char *q = lt->name + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
            if (bits > 0)
                return arenaPrintf(g->arena, "(%s %s extc_shiftCount((int64_t)(%s), %d, \"%s\", %d))",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right), bits, g->path, e->line);
        }
    }

    return arenaPrintf(g->arena, "(%s %s %s)",
                       genExpr(g, e->u.bin.left), op, genExpr(g, e->u.bin.right));
}

/* 零值表达式。
 *
 * ⚠️ 这里有个坑值得记：**零初始化不能一律用 `{0}`**。
 * `str` 是不可为空的，而 `{0}` 会把 `const char *` 变成 NULL ——
 * `printf("%s", NULL)` 在 C 里是 UB（glibc 恰好打 "(null)" 骗过你）。
 * 所以含 `str` 的 struct 必须逐字段写出零值。 */
static const char *zeroValue(CG *g, Type *t);

/* struct 里（递归地）有没有 `ref`？
 * 有的话就不能用 `{0}` —— 那会造出空引用，走防御分支让 C 编译器报错。 */
static bool needsExplicitZero(Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind != TY_STRUCT || !t->sdef) return false;
    for (size_t i = 0; i < t->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
        if (needsExplicitZero(fd->type)) return true;
    }
    return false;
}

/* 在有泛型实例上下文的保护下生成子表达式。
 * **坑**：泛型实例的字段必须用它**自己的**实参替换，不能用环境里碰巧留着的上下文
 * （否则 subst 原样返回，零值生成会自己递归自己 → 栈溢出）。 */
static void substEnterInst(CG *g, StructDef *sd, Vec *targs, Vec **saveP, Vec **saveA, const char **saveN) {
    *saveP = g->substParams;
    *saveA = g->substArgs;
    *saveN = g->ownerPrefix;
    g->substParams = &sd->typeParams;
    g->substArgs   = targs;
}

static void substLeaveInst(CG *g, Vec *saveP, Vec *saveA, const char *saveN) {
    g->substParams = saveP;
    g->substArgs   = saveA;
    g->ownerPrefix = saveN;
}

static const char *zeroValue(CG *g, Type *t) {
    if (!t) return "0";
    /* 带载荷枚举的零值 = **tag 0 + 载荷清零** ⇒ `(形状){0}` 正好是这个意思
     * （C 的 `{0}` 会把 tag 和整个 union 清零，而 union 的哪个成员有意义由 tag 决定）。
     * tag 0 的载荷里含 `ref` 时，类型检查阶段就会报「不能零初始化」✓ */
    if (t->kind == TY_ENUM)
        return enumHasPayload(t->edef) ? arenaPrintf(g->arena, "(%s){0}", t->name) : "0";
    if (t->kind == TY_ARRAY) return arenaPrintf(g->arena, "(%s){0}", t->name);

    if (t->kind == TY_PARAM) {
        Type *a = subst(g, t);
        if (a == t) return "0";     /* 没有上下文 —— 不该发生，但绝不死循环 */
        return zeroValue(g, a);
    }

    /* 泛型实例：用它**自己的**实参替换字段类型，再逐字段写零值 */
    if (t->kind == TY_GENERIC && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", t->name);

        Vec *sp, *sa;
        const char *sn;
        substEnterInst(g, sd, &t->targs, &sp, &sa, &sn);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", t->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");

        substLeaveInst(g, sp, sa, sn);
        return bufCstr(&b);
    }

    if (t->kind == TY_STRUCT && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0 || !needsExplicitZero(t))
            return arenaPrintf(g->arena, "(%s){0}", sd->name);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", sd->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");
        return bufCstr(&b);
    }

    if (ttIs(t, "bool")) return "false";

    /* 防御：`ref` 没有零值。check 保证走不到这里（含 ref 的 struct 不许零初始化、
     * 含 ref 的字段不许省略）—— 万一将来有路径漏过，这里生成一个**不存在的标识符**，
     * 让 C 编译器报错，而不是悄悄塞一个空引用。 */
    /* `?ref T` 的零值就是 `null`（可空引用有零值 ✓，见定案 ㊻）——
     * ⚠️ 这里以前一律当成"没有零值"，于是**含 `?ref` 字段的结构体**零初始化时
     * 会生成那个不存在的标识符 ✗（真 bug，2026-09-20 修：`var l: list` 编不过）*/
    if (t->kind == TY_REF) return t->nullable ? "((void *)0)"
                                              : "__extc_reference_has_no_zero_value__";

    return "0";
}

static const char *zeroInit(CG *g, Type *t) {
    return zeroValue(g, t);
}

/* `println(x)` 里 x 的**生成 C 表达式是不是 lvalue**（能取地址）？
 *
 * ⚠️ 这跟 `isPlaceExpr`（"extC 里的地方"）**不是一回事**，混了就出事：
 *   · 切片表达式 `s[0..5]` 在 extC 里是地方，但生成的 C 是
 *     `slice_u8_slice(s, 0, 5, "…", 54)` —— 一个**函数调用**，`&` 它不合法 ✗
 *     （真踩过：`examples/slices.extc` + `euler-sieve.extc` 当场编不过）
 *   · 枚举变体 `color.red` 在 extC 里长得像字段访问，但检查器早就换成了
 *     非地方的构造式 ⇒ 这里自然落到 false ✓
 *
 * **拿不准就返回 false**：代价只是多一次拷贝（跟旧 `_debug(expr)` 传值一样 ✓），
 * 猜错的代价是**生成的 C 编不过** ✗ */
static bool printArgIsPlace(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;                                  /* `x` / `p.f` 的根 */
    case EX_FIELD: return printArgIsPlace(e->u.field.obj);
    case EX_INDEX: return printArgIsPlace(e->u.index.obj);
    default:       return false;
    }
}

static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(subst(g, a->type));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt) { bufPuts(&b, "0"); continue; }

        /* ⭐ 结构化类型（枚举 / 字节视图 / struct / 泛型实例 / 数组）**一律走描述表**：
         *      extc_print(&x, &x_desc)
         * 于是"每种类型一份打印代码"彻底没了 —— 只剩每类型一份数据 ✓
         *
         * ⚠️ 描述表要**地址**（`extc_print` 是泛型打印器）⇒ 实参必须是个地方。
         * 不是地方（`println("字面量")` / `println(makePoint())`）⇒ 先落进
         * **临时变量**再取地址，那行由"语句前缀"机制吐在所在语句之前 ✓
         * （试过 C 的复合字面量 `(T){expr}`，**不行**：C 不允许用同类型的
         *   表达式去初始化聚合体 ⇒ gcc 报 incompatible types ✗）*/
        if (bt->kind == TY_ENUM || isByteView(bt) || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;              /* 打印运行时得跟着出来 ✓ */
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_print(&(%s), %s)", code, descRef(g, bt));
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
                bufPrintf(&b, "extc_print(&%s, %s)", tmp, descRef(g, bt));
            }
            continue;
        }
        if (bt->kind != TY_BUILTIN) { bufPuts(&b, "0"); continue; }

        if (strcmp(bt->name, "bool") == 0) {
            bufPrintf(&b, "printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
            continue;
        }
        const PrintFmt *pf = NULL;
        for (size_t k = 0; PRINT_FMT[k].extc; k++)
            if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
        if (!pf) { bufPuts(&b, "0"); continue; }
        bufPrintf(&b, "printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
    }

    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "printf(\"\\n\")");
    } else if (args->len == 0) {
        bufPuts(&b, "printf(\"\")");
    }

    bufPutc(&b, ')');
    return bufCstr(&b);
}

/* 这个表达式在 C 里是 lvalue 吗（能取地址）？跟 check 里那个 isPlace 同一个判断，
 * 只不过这里是**为了生成的 C 合法**：`&(f())` 在 C 里非法。 */
static bool isPlaceExpr(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlaceExpr(e->u.field.obj);
    case EX_INDEX: return isPlaceExpr(e->u.index.obj);
    case EX_SLICE: return isPlaceExpr(e->u.slice.obj);
    default:       return false;
    }
}

static const char *homeArg(CG *g, int marked);   /* 定义在后面 */

static const char *genMethodCall(CG *g, Expr *e) {
    FuncDef *f = e->func;
    if (!f) return "0";

    Type *recvT = e->u.method.recv->type;
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* 接收者按需取地址 / 解引用 —— 这就是 `a.f(x)` == `f(a, x)` 的全部机制 */
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = recvT && recvT->kind == TY_REF;
    if (wantRef && !haveRef) {
        /* 接收者是**临时值**时不能直接 `&`（C 里 `&(f())` 非法）。
         * 先落进一个临时存储再取地址。用**单元素数组**的复合字面量：
         *     `(T[]){ f() }`  →  类型是 `T *`（数组退化成指针）
         * 别写成 `&((T){ f() })` —— `(T){ x }` 在 C 里**不是拷贝**，
         * 它拿 x 去初始化**第一个成员**（`_Bool has = <option_i64>` 那种瞎报错）。
         * 数组初始化才是逐元素的，`{ f() }` 就是「用一个 T 初始化元素 0」。
         * 存储期到语句结束，正好够这次调用（Rust 的 `next().unwrap()` 同理）。
         * 注意：只有**不是地方**的才这么处理，否则会把「写进元素」变成「写进副本」。 */
        if (isPlaceExpr(e->u.method.recv)) {
            recvC = arenaPrintf(g->arena, "&(%s)", recvC);
        } else {
            recvC = arenaPrintf(g->arena, "(%s[]){ %s }",
                                cType(g, subst(g, recvT)), recvC);
        }
    } else if (!wantRef && haveRef) {
        recvC = arenaPrintf(g->arena, "*(%s)", recvC);
    }

    const char *fname = cMethodName(g, recvT, f);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
    /* A3：方法也要把家 arena 传过去（接收者就是那个"最浅的 mut ref 实参"✓）*/
    if (f->needsHome) bufPrintf(&b, ", %s", homeArg(g, e->homeDepth));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

static const char *genStructLit(CG *g, Expr *e) {
    /* ⚠️ 先整体替换一次：在泛型实例里，字面量记的类型还是**模板**（`result<T,E>`），
     * 直接拿它会看到 targs 是类型参数 —— 于是零值那条路拿到裸 `T`、
     * 退化成 `0`，生成的 C 里变成 `.value = 0`（真踩过：`result<unit, E>::failure`）。 */
    Type *t = subst(g, e->type);
    StructDef *sd = (t && (t->kind == TY_STRUCT || t->kind == TY_GENERIC)) ? t->sdef : NULL;
    if (!sd) return "(int){0}";

    const char *cname = cType(g, t);       /* 泛型实例拿到的是修饰名 */
    if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", cname);

    /* 泛型实例的字面量也要用它**自己的**实参替换字段类型 */
    Vec *sp = g->substParams, *sa = g->substArgs;
    const char *sn = g->ownerPrefix;
    if (t->kind == TY_GENERIC) {
        g->substParams = &sd->typeParams;
        g->substArgs   = &t->targs;
    }

    /* **所有**字段都写出来：省略的字段填它的零值。
     * 不能让 C 自己去零填充 —— 那会把省略的 `str` 字段变成 NULL。 */
    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){", cname);
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        Type *ft = subst(g, fd->type);
        Expr *v = litValueFor(e, fd->name);
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, ".%s = %s", fd->name,
                  v ? genExpr(g, v) : zeroValue(g, ft));
    }
    bufPuts(&b, "}");

    g->substParams = sp;
    g->substArgs   = sa;
    g->ownerPrefix = sn;
    return bufCstr(&b);
}

static const char *arenaRef(CG *g);
static const char *arenaRefAt(CG *g, int level);   /* ⭐ 定案 63：按层选 arena ✓ */

static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT: return arenaPrintf(g->arena, "%g", e->u.fval);
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:
            /* `"abc"` → 一个字节视图，指向只读内存里的字面量。
             * 长度用 `sizeof("...") - 1` —— 让 C 去处理转义，我们不用自己解析。 */
            return arenaPrintf(g->arena,
                "(%s){ .data = (uint8_t *)\"%s\", .len = sizeof(\"%s\") - 1 }",
                cType(g, e->type), e->u.str.text, e->u.str.text);
        case EX_IDENT: return e->u.ident.cname ? e->u.ident.cname : e->u.ident.name;

        /* `*p` —— 显式解引用就是一个 C 的解引用 ✓（只读/可写由类型检查管）*/
        case EX_DEREF:
            return arenaPrintf(g->arena, "(*(%s))", genExpr(g, e->u.deref.operand));

        case EX_BIN: return genBin(g, e);

        case EX_UN:
            return arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) return "0";
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
            if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);
            if (!e->func) return "0";

            /* 只用 `cSymName`（**不带** ownerPrefix）：调用点写的是被调用者自己的名字，
             * 而 ownerPrefix 是「当前正在生成谁的实例」。同一件事在 cFuncName 里
             * 得分清楚，见那个函数上的注释。这里顺手把 C 关键字改掉（`fn double`）。*/
            name = cSymName(g, name);
            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, name);
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
            }
            /* A3：被调用者需要一只"家"arena ⇒ 我传我的（或者传当前块的 ✓ 紧）*/
            if (e->func->needsHome) {
                if (e->u.call.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->homeDepth));
            }
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        case EX_INDEX: {
            Type *ot = e->u.index.obj->type;
            Type *ob = ttBase(subst(g, ot));
            const char *obj = genExpr(g, e->u.index.obj);
            const char *idx = genExpr(g, e->u.index.index);

            /* 数组：长度是**编译期常数** —— 所以 obj 只出现一次，不存在重复求值 */
            if (ob && ob->kind == TY_ARRAY) {
                /* ⚠️ 底是 **`ref [N]T`** 时，C 里 `obj` 是**指针** ⇒ 要先解一层 ✗
                 * （PLAN #25：`p[..]` / `p[i]` 切 `ref [8]u8` 会生成 `p.data[..]`，
                 *   gcc 报 "'p' is a pointer; did you mean to use '->'?"）*/
                if (ot && ot->kind == TY_REF)
                    obj = arenaPrintf(g->arena, "(*%s)", obj);
                return arenaPrintf(g->arena,
                    "%s.data[extc_checkedIndex((int64_t)(%s), %lld, \"%s\", %d)]",
                    obj, idx, (long long)ob->asize, g->path, e->line);
            }
            if (!isView(ob)) return "0";
            /* 原语按值收视图，所以引用要解一层 */
            if (ot && ot->kind == TY_REF) obj = arenaPrintf(g->arena, "*(%s)", obj);

            /* 原语返回指针 —— 解引用后是个 lvalue：能读、能取地址（`ref` 参数）、能赋值 */
            return arenaPrintf(g->arena, "(*%s_index(%s, (int64_t)(%s), \"%s\", %d))",
                               ob->name, obj, idx, g->path, e->line);
        }

        case EX_ARRAYLIT: {
            Type *t = e->type;
            if (!t || t->kind != TY_ARRAY) return "0";
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .data = {", cType(g, t));
            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
            }
            /* 末尾的 `...` 不用额外做什么 —— C 的初始化器本来就把剩下的补零 */
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }

        case EX_SLICE: return genSlice(g, e);

        case EX_TRY:
            /* `?` 不该走到这里 —— 它是语句级展开的，由 genStmt 那三处直接处理。
             * 走到这儿说明 check 的位置限制漏了，报出来别静默生成错的 C。 */
            ctxError(g->ctx, e->line, 1, NULL,
                     "internal: `?` reached expression codegen (position check missed it)");
            return "0";

        case EX_ASSOC: {
            /* 关联函数：C 名字用**实例名**修饰（`option_i64_some`）——
             * 跟方法同一套修饰规则，所以直接复用 cMethodName。 */
            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, cMethodName(g, e->assocOwner, e->func));
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.assoc.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.assoc.args, i)));
            }
            /* A3：被调用者需要家 arena ⇒ 补上（`Type::make()` 这类关联函数会分配 ✓）*/
            if (e->func->needsHome) {
                if (e->u.assoc.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->homeDepth));
            }
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        /* `new T` / `new [N]T` / `new T[n]`（PLAN A1）—— 分配进**当前块**的 arena、
         * **清零**（运行时那条 alloc 本身就清零 ⇒ extC 里只有一条规则 ✓）*/
        case EX_CONV: {
            Type *t = subst(g, e->u.conv.type);          /* 目标类型（检查器解析好的）*/
            const char *x = genExpr(g, e->u.conv.operand);
            if (!e->convCheck)
                return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

            /* 带检查：拿一张范围表，走上面那三个 `static inline` 助手 ✓ */
            const char *tn = t->name;
            bool isF = ttIsFloat(subst(g, e->u.conv.operand->type));
            if (isF) {
                int64_t lo = 0, hi = 0;
                if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
                else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
                else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
                else if (strcmp(tn,"i64")==0) { lo = 1; hi = 0; }   /* 用宏，见下 */
                else if (strcmp(tn,"u8")==0)  { lo = 0; hi = 255; }
                else if (strcmp(tn,"u16")==0) { lo = 0; hi = 65535; }
                else if (strcmp(tn,"u32")==0) { lo = 0; hi = 4294967295LL; }
                else { lo = 0; hi = INT64_MAX; }        /* u64（浮点源）：按 i64 上限查 */
                return arenaPrintf(g->arena,
                    "((%s)extc_convFloat((double)(%s), %lldLL, %lldLL, \"%s\", %d))",
                    cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
            }
            bool sign = (tn[0] == 'i');
            if (sign) {
                int64_t lo = 0, hi = 0;
                if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
                else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
                else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
                else
                    return arenaPrintf(g->arena,
                        "((%s)extc_narrowI((int64_t)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                        cType(g, t), x, g->path, e->line);
                return arenaPrintf(g->arena,
                    "((%s)extc_narrowI((int64_t)(%s), %lldLL, %lldLL, \"%s\", %d))",
                    cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
            }
            unsigned long long hi = 0;
            if (strcmp(tn,"u8")==0)  hi = 255ULL;
            else if (strcmp(tn,"u16")==0) hi = 65535ULL;
            else if (strcmp(tn,"u32")==0) hi = 4294967295ULL;
            else
                return arenaPrintf(g->arena,
                    "((%s)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))",
                    cType(g, t), x, g->path, e->line);
            return arenaPrintf(g->arena,
                "((%s)extc_narrowU((uint64_t)(%s), %lluULL, \"%s\", %d))",
                cType(g, t), x, hi, g->path, e->line);
        }

        case EX_NEW: {
            Type *w = subst(g, e->u.new_.type);
            /* ⭐ 定案 63：分配进检查器算好的那一层（可能因为"要存进更外层的地方"
             * 而**提升**过）—— 这是"自动清理的堆"那条路的具体落点 ✓ */
            const char *ar = arenaRefAt(g, e->arenaLevel);
            if (!e->u.new_.count) {
                /* 一个 T（或一个 [N]T）的**地方** ⇒ 就是它的地址 ✓ */
                return arenaPrintf(g->arena, "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s)))",
                                   cType(g, w), ar, cType(g, w));
            }
            /* `T[n]` ⇒ 视图 `{ data, len }`；个数只求值一次
             * （个数不纯时检查器打过 needTemp ⇒ 先吐一句把它装进临时变量 ✓）*/
            const char *n;
            if (e->needTemp) {
                const char *tmp = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
                pfLine(g, "int64_t %s = (int64_t)(%s);", tmp, genExpr(g, e->u.new_.count));
                n = tmp;
            } else {
                n = genExpr(g, e->u.new_.count);
            }
            Type *st = subst(g, e->type);      /* 检查器算好的 `slice<T>` ✓ */
            return arenaPrintf(g->arena,
                "(%s){ .data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s)),"
                " .len = (int64_t)(%s) }",
                cType(g, st), cType(g, w), ar, n, cType(g, w), n);
        }

        case EX_GENCALL: {
            /* 泛型调用 —— 目前只有内置原语 `alloc<T>(n)`：
             * 向**当前块**的 arena 要 n 个 T 的地方（按块细化之后就是这句话的意思 ✓）*/
            const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
            const char *n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
            return arenaPrintf(g->arena,
                "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s))",
                tn, arenaRef(g), n, tn);
        }

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);

        case EX_REF:
            return arenaPrintf(g->arena, "&(%s)", genExpr(g, e->u.ref.operand));

        /* `a ?? b` —— 可能没有就兜底。
         *
         *   · 主体**纯**（变量/字段/下标）⇒ 直接生成 C 三元（`x.tag == t_some ? x.u.some._0 : b`）
         *   · 主体**不纯**（`f() ?? -1`）⇒ 检查器已经打了 `needTemp` ⇒ 先算进临时变量，
         *     那几行由 `flushPrefix` 吐在**所在语句之前**（见下面 `e->needTemp` 分支）✓
         * 顺带的好处：C 的三元**只算一边** ⇒ 兜底那侧的副作用不会白跑 ✓ */
        case EX_COALESCE: {
            Type *mt = e->u.coalesce.main->type;
            const char *m;
            if (e->needTemp) {
                /* 主体**有副作用**（`f() ?? -1`）⇒ 先算一次装进临时变量，
                 * 后面三元里出现的是这个变量（重复读同一个变量没有副作用 ✓）
                 * 这几行由 flushPrefix 吐在**所在语句之前** ✓ */
                const char *tmp = arenaPrintf(g->arena, "__extc_c%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, mt), tmp, genExpr(g, e->u.coalesce.main));
                m = tmp;
            } else {
                m = genExpr(g, e->u.coalesce.main);
            }
            const char *fb = genExpr(g, e->u.coalesce.fallback);

            /* ⚠️ **兜底那侧显式转成结果类型**（2026-09-20 主人指出）：
             * C 的 `?:` 会对两支做"通常算术转换" —— `(int64_t)` 和 `int` 拼一起
             * 结果可能被悄悄**拓宽**（f32 载荷配 double 字面量就是典型）。
             * 类型检查那边已经保证了兜底能放进结果类型，所以这只是**保险 + 自证**，
             * 但它让生成的 C 一眼就能看出"我要的就是这个类型" ✓
             * ⚠️ 只给**内置标量**加：C 里**不能** cast 到数组类型，
             *    `option<[3]i32> ?? arr` 加了会直接编不过 ✗（实测踩过）*/
            /* ⚠️ 要转的是**结果类型**（载荷 T），不是 `option<T>` 本身 ——
             * 拿 option 的类型去转是错的（第一版就写错了，生成的 C 里根本没出现 cast ✗）*/
            Type *rt = mt;
            if (mt && mt->kind != TY_REF && mt->targs.len > 0)
                rt = *(Type **)vecAt(&mt->targs, 0);
            bool scalar = rt && (rt->kind == TY_BUILTIN || rt->kind == TY_REF);
            const char *rhs = scalar
                ? arenaPrintf(g->arena, "((%s)(%s))", cType(g, rt), fb) : fb;

            if (mt && mt->kind == TY_REF) {
                /* `?ref T`：C 里就是普通指针 ⇒ 判空即可 ✓ */
                return arenaPrintf(g->arena, "((%s) != ((void *)0) ? (%s) : %s)", m, m, rhs);
            }
            bool isOpt = isProtoType(mt, "option", 1);
            const char *tag = isOpt ? "some" : "success";
            return arenaPrintf(g->arena, "((%s).tag == %s_%s ? (%s).u.%s._0 : %s)",
                               m, cType(g, mt), tag, m, tag, rhs);
        }

        /* `e!` —— **我签字，没有运行时痕迹** ✓
         *   `opt!` / `r!` ⇒ 直接取载荷（union 成员），**不判 tag**
         *   `p!`（`?ref T`）⇒ C 里就是那个指针本身，一个字都不用生成 ✓ */
        case EX_SIGN: {
            Type *ot = e->u.sign.operand->type;
            if (ot && ot->kind == TY_REF) return genExpr(g, e->u.sign.operand);
            bool isOpt = isProtoType(ot, "option", 1);
            const char *var = isOpt ? "some" : "success";
            return arenaPrintf(g->arena, "(%s).u.%s._0",
                               genExpr(g, e->u.sign.operand), var);
        }

        /* `null` —— 可空引用的零值。C 里的表示就是一个空指针：
         * 类型检查已经保证了「用之前先查过 null」（narrowing），
         * 所以这里**不生成任何运行时检查** —— 编译期能证明的，运行时不留痕迹 ✓ */
        case EX_NULL:
            return "((void *)0)";

        case EX_ENUMVAL: {
            const char *tn = e->u.enumval.typeName;
            const char *vn = e->u.enumval.variant;
            /* 泛型枚举的实例名（`maybe_i64`）在类型表里查不到名字 —— 检查器
             * 解析好的类型记在 `assocOwner` 上，优先用它 ✓ */
            Type *et = e->assocOwner;
            if (!et && g->tt) et = ttFromName(g->tt, tn);
            /* ⚠️ **泛型实例里**：`tn` 是**模板**的名字（`option_T`），而实例的 C 名字是
             * `option_i32` ⇒ 有 `assocOwner` 时一律走 `subst` + `cType` 拿真名 ✓
             * （真 bug：`varArray<i32>::get()` 会生成 `option_T` 这种不存在的类型 ✗）*/
            if (et) {
                Type *rt = subst(g, et);
                if (rt && rt->name) tn = rt->name;
            }
            bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);

            /* 无载荷枚举（C 里就是 `enum`）⇒ 变体本身就是一个常量 */
            if (!payload)
                return arenaPrintf(g->arena, "%s_%s", tn, vn);

            /* 带载荷枚举（C 里是 `struct { tag; union }`）⇒ 连无载荷的变体
             * 也要"构造"一下：`(shape){ .tag = shape_dot }` */
            if (e->u.enumval.args.len == 0)
                return arenaPrintf(g->arena, "(%s){ .tag = %s_%s }", tn, tn, vn);

            /* **带载荷构造**：`(shape){ .tag = shape_circle, .u.circle = { ._0 = 2.0 } }` */
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .tag = %s_%s, .u.%s = {", tn, tn, vn, vn);
            for (size_t i = 0; i < e->u.enumval.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPrintf(&b, "._%zu = %s", i, genExpr(g, *(Expr **)vecAt(&e->u.enumval.args, i)));
            }
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }
    }
    return "0";
}

/* 值位置的**自动解引用**（形状 3）。
 *
 * 类型检查阶段把 `ref T` 在值位置当 `T` 用（并在那个节点上打 `deref` 标记），
 * 这里就补一次解引用 —— 于是 `let y = p` 拷的是值，`p + 1` 加的是值，
 * 而「能不能写」由 `ref` / `mut ref` 在类型层承担。
 *
 * 包一层是因为**所有**生成表达式的路径都要经过它（含递归调用）——
 * 放在唯一的出口上，就不会有哪条路忘了解引用。 */
static const char *genExpr(CG *g, Expr *e) {
    const char *s = genExprInner(g, e);
    if (e->deref) return arenaPrintf(g->arena, "*(%s)", s);
    return s;
}

/* ---------------------------------------------------------------- 自动调试打印
 *
 * 每个 struct 有 `<Type>_debug`（**递归**打印所有字段），这一件必须由**编译器**做 ——
 * 它等价于 Rust 的 `#[derive(Debug)]`：extC 没有反射，「遍历所有字段」在语言里写不出来。
 * 注意这跟「把标准库塞进编译器」是两回事：这是**编译器生成代码**。
 * 分界线见 DESIGN.md「能写在 extC 里的，就别写在编译器里」。
 *
 * ⚠️ 现在它**不再是"生成的代码"**，而是"生成的数据 + 一个通用打印器" ——
 * 见上面的 `genStructDesc` 与生成文件开头的 `extc_print` ✓
 */

/* ---------------------------------------------------------------- 语句 */

static void genStmt(CG *g, Stmt *s);

/* ---------------------------------------------------------------- `?` 展开
 *
 * `?` 是**语句级**的：C 没有语句表达式，所以要展开成
 *     <求值一次>  →  <失败就 return>  →  <接着用载荷>
 *
 * 编译器在这里认识的是**协议**（跟视图的 `data` + `len` 是同一种分工，
 * 见 MANUAL「语言认识协议，库提供结构」）：
 *     option：标签 `has`、载荷 `value`
 *     result：标签 `ok`、载荷 `value`、错误 `err`
 * 结构、方法、构造器**全在 prelude 里**，编译器只认这几个名字。
 * 这些名字在主程序里被 viewContractOk 那一套校验过（见 main.c）。
 */
/* `e?` 的 codegen（第三刀之后 option/result 是**普通枚举**了）。
 *
 * 以前它们是有 `has` / `ok` 字段的 struct ⇒ 这里是"读字段、拼字段"；
 * 现在是 `tag + union` ⇒ 全部走 **tag 比较 + 载荷路径** ✓ */
typedef struct {
    const char *tmp;      /* 临时变量名（求值就一次，装在这里） */
    const char *inst;     /* 实例的 C 名字：option_i64 / result_unit_gameError */
    const char *okVar;    /* 成功变体名：some / success */
    const char *failVar;  /* 失败变体名：none / failure */
    Type       *payload;  /* 载荷类型 T（成功那一侧装的） */
    bool        isOpt;    /* option 还是 result */
} TryInfo;

static bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* 泛型 struct：`slice<T>`、`varArray<T>` */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* 泛型**枚举**：`option<T>` / `result<T,E>`（第三刀之后它们就是枚举 ✓）*/
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/* 成功侧载荷的 C 路径：`x.u.some._0` / `x.u.success._0` */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/* 失败时该 return 什么：往**外层**的返回类型上构造失败值。
 * option 的零值就是 none（定案 8）；result 要带上内层的错误。 */
static const char *genTryFail(CG *g, TryInfo *ti) {
    Type *rt = subst(g, g->retType);
    if (!rt) return "0";
    if (ti->isOpt) return zeroValue(g, rt);
    /* result：把内层的错误**原样搬过去**（同一个 E ⇒ C 里直接抄那个成员）✓ */
    return arenaPrintf(g->arena,
                       "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s.u.%s._0 } }",
                       cType(g, rt), rt->name, ti->failVar, ti->failVar,
                       ti->tmp, ti->failVar);
}

/* ⭐ A（编译时长优化，2026-09-21）：**统一出口** —— Linux 内核那种 `goto out;` 套路 ✓
 *
 * 以前每个 `return` / `?` 失败点都把整串释放**内联展开**一遍
 * （`extc_arena_release(&__extc_a[3]); …(&__extc_a[1]); return x;`）⇒
 * 同一串在每个出口复制一次，gcc 要挨个看 ✗（压测量：这是 gcc 时间的大头之一）
 * 现在：每个出口只吐 `__extc_ret_v = <值>; goto __extc_ret;`，**释放串只吐一遍** ✓
 *
 * 语义没变：都是"先算好值、再放 arena、再返回" ✓
 * （值先落进 `__extc_ret_v`，比原来"先释放再求值"更保守一点 ✓）
 * `noArena` 的函数（不会分配、连数组都没有）⇒ 保持直接 return ✓ */
static void cgReturn(CG *g, const char *val) {
    if (g->noArena) {
        if (val) cgLine(g, "return %s;", val);
        else     cgLine(g, "return;");
        return;
    }
    if (val) cgLine(g, "__extc_ret_v = %s;", val);
    cgLine(g, "goto __extc_ret;");
}

/* 出「求值一次」和「失败就 return」两句，返回临时变量名 */
static TryInfo genTryHead(CG *g, Expr *e) {
    TryInfo ti;
    Type *ot = ttBase(subst(g, e->u.try_.operand->type));
    ti.isOpt = isProtoType(ot, "option", 1);
    ti.inst = cType(g, ot);
    ti.okVar = ti.isOpt ? "some" : "success";
    ti.failVar = ti.isOpt ? "none" : "failure";
    ti.payload = subst(g, *(Type **)vecAt(&ot->targs, 0));
    ti.tmp = arenaPrintf(g->arena, "__extc_try%d", g->tmpSeq++);

    const char *operand = genExpr(g, e->u.try_.operand);
    flushPrefix(g);
    cgLine(g, "%s %s = %s;", ti.inst, ti.tmp, operand);
    /* 失败要 return ⇒ 把**所有**层的 arena 都放掉（跟 `return` 一样）✓
     * ⚠️ 这件事必须在吐出这一行**之前**做：那行已经写着 `return` 了 */
    /* ⭐ A：失败出口也走**统一出口**（释放串不再内联展开 ✓）*/
    cgLine(g, "if (%s.tag != %s_%s) {", ti.tmp, ti.inst, ti.okVar);
    g->indent++;
    cgReturn(g, genTryFail(g, &ti));
    g->indent--;
    cgLine(g, "}");
    return ti;
}


static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

/* 当前块的 arena（分配走它、出块释放它）✓ */
/* 调用一个"需要家 arena"的函数时，我该传哪只？
 *   我自己有家 ⇒ 传我的家（那是最外层的、祖先那只 ✓）
 *   我没有家 ⇒ 传**当前块**那只（更紧：被调用者分配的东西活到本块结束 ✓）*/
static const char *homeArg(CG *g, int marked) {
    /* 这个是**当实参传**的（不是给 `&` 用的）⇒ 直接给指针 ✓
     * ⚠️ 别跟 `arenaRef` 搞混：那个的结果外面会套一层 `&`，所以它返回 `(*__extc_home)` ✓
     *
     * `marked`（A3 第二半，检查器标的）：
     *   -1 ⇒ 传我的家（祖先那只）；>=1 ⇒ 传 `&__extc_a[那个块]`（精确）*/
    if (marked >= 1) return arenaPrintf(g->arena, "&__extc_a[%d]", marked);
    if (marked == -1 || g->hasHome) {
        if (g->hasHome) return "__extc_home";
    }
    return arenaPrintf(g->arena, "&__extc_a[%d]", g->blkLevel);
}

static const char *arenaRef(CG *g) {
    /* 有家（A3）⇒ 分配到调用者选的那只；否则分配到**当前块** ✓ */
    if (g->hasHome) return "(*__extc_home)";   /* 外面还会套一层 `&` ✓ */
    return arenaPrintf(g->arena, "__extc_a[%d]", g->blkLevel);
}

/* ⭐ 定案 63（PLAN #38）：这个 `new` 该进**哪一层** arena？
 * 数由**检查器**算好（`Expr.arenaLevel`）—— 默认是语句所在的块，被"存进更外层
 * 的地方"时已经**提升**过了 ✓ 这里只负责翻译成 C 文本：
 *     k>0 ⇒ `__extc_a[k]`，出第 k 层块时 release ✓
 *
 * ⚠️⚠️ **有家的函数一律以 `hasHome` 为准**（不看那个数）—— 两个原因：
 *   ① 检查器算层数时**还不知道传递闭包**（"我调的人有家 ⇒ 我也有家"是查完
 *      所有函数体之后才算的）⇒ 那种函数里算出来的数是"以为自己没家"的 ✗
 *      （golden 当场抓出来：`out-param` 的分配从家 arena 掉回了块 arena）
 *   ② 家 arena 比块 arena **长寿 ⇒ 只会更安全**，方向永远对 ✓
 *      代价：这类函数里"其实没逃逸"的分配也活到调用者那块结束（A3 的老行为）✓
 *   ③ 提升只会把层数**变小**（往长寿方向），所以"被提升过"这件事在有家时不丢 ✓ */
static const char *arenaRefAt(CG *g, int level) {
    if (g->hasHome) return "(*__extc_home)";
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/* 释放第 lv 层（`lvl > 0`；第 0 层不用）*/
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    if (g->noArena) return;   /* ⭐ 这个函数不会分配 ⇒ 连 release 都不用吐 ✓ */
    cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/* **这个函数最多会用到几层块**（用来定 `__extc_a` 的大小 —— 栈上定长，零分配）*/
static int blkMaxLevel(Stmt *s);
static int blkMaxOfBlock(Stmt *block) {
    int m = 0;
    if (!block || block->kind != ST_BLOCK) return blkMaxLevel(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        if (blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i)) > m)
            m = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));
    return m;
}
static int blkMaxLevel(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_BLOCK: return 1 + blkMaxOfBlock(s);
    case ST_IF: {
        int a = 1 + blkMaxOfBlock(s->u.ifs.thenBody);
        int b = s->u.ifs.elseBody
                  ? 1 + (s->u.ifs.elseBody->kind == ST_BLOCK
                           ? blkMaxOfBlock(s->u.ifs.elseBody)
                           : blkMaxLevel(s->u.ifs.elseBody))
                  : 0;
        return a > b ? a : b;
    }
    case ST_WHILE: return 1 + blkMaxOfBlock(s->u.whiles.body);
    case ST_MATCH: {
        int m = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            int d = 1 + blkMaxOfBlock(a->body);
            if (d > m) m = d;
        }
        return m;
    }
    default: return 0;
    }
}

/* 进/出**一个块**：reset 进来、release 出去 ✓
 * ⚠️ 循环体也是一个块 ⇒ 每轮进来都 reset ⇒ **内存上界 = 一次迭代** ✓（这就是 A2 的目的）*/
static void genBlockBody(CG *g, Stmt *block) {
    g->blkLevel++;
    if (!g->noArena) cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);   /* 进来先清（防御性）*/
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
    cgReleaseLevel(g, g->blkLevel);
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/* 每条语句都在这一层里生成 ⇒ 语句内需要"提前求值"的东西把前缀吐在这里 ✓ */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_VAR: {
            /* `cname` = 检查器定下的名字（同层遮蔽过的会带 `__2` 后缀）*/
            const char *nm = s->u.var.cname ? s->u.var.cname : s->u.var.name;
            if (s->u.var.init && s->u.var.init->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.var.init);
                flushPrefix(g);
                cgLine(g, "%s %s = %s;",
                       cType(g, s->type), nm, tryPayloadPath(g, &ti));
                return;
            }
            const char *init = s->u.var.init ? genExpr(g, s->u.var.init)
                                             : zeroInit(g, s->type);
            flushPrefix(g);
            cgLine(g, "%s %s = %s;", cType(g, s->type), nm, init);
            return;
        }

        case ST_ASSIGN:
            if (s->u.assign.value && s->u.assign.value->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.assign.value);
                const char *at = genExpr(g, s->u.assign.target);
                flushPrefix(g);
                cgLine(g, "%s = %s;", at, tryPayloadPath(g, &ti));
                return;
            }
            const char *tgt = genExpr(g, s->u.assign.target);
            const char *val = genExpr(g, s->u.assign.value);
            flushPrefix(g);
            cgLine(g, "%s = %s;", tgt, val);
            return;

        case ST_IF: {
            const char *cnd = genExpr(g, s->u.ifs.cond);
            flushPrefix(g);
            cgLine(g, "if (%s) {", cnd);
            g->indent++;
            genBlockBody(g, s->u.ifs.thenBody);
            g->indent--;
            if (!s->u.ifs.elseBody) {
                cgLine(g, "}");
                return;
            }
            cgLine(g, "} else {");
            g->indent++;
            if (s->u.ifs.elseBody->kind == ST_BLOCK) genBlockBody(g, s->u.ifs.elseBody);
            else                                     genStmt(g, s->u.ifs.elseBody);
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_WHILE: {
            const char *cnd = genExpr(g, s->u.whiles.cond);
            flushPrefix(g);
            cgLine(g, "while (%s) {", cnd);
            g->indent++;
            g->loopLevel[g->loopLen++] = g->blkLevel + 1;   /* 循环体是下一层 */
            genBlockBody(g, s->u.whiles.body);
            g->loopLen--;
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_RETURN: {
            /* ⭐ A：**统一出口** —— 这里只吐 `__extc_ret_v = …; goto __extc_ret;`
             * （释放串在函数尾部吐**一遍**，不再每个 return 复制一份 ✓）*/
            if (!s->u.ret.value) {
                cgReturn(g, NULL);           /* ⭐ A：统一出口（释放串只吐一遍 ✓）*/
                return;
            }
            if (s->u.ret.value->kind == EX_TRY) {
                /* `return e?` —— 成功就把载荷装回**外层**返回类型 */
                TryInfo ti = genTryHead(g, s->u.ret.value);
                Type *rt = subst(g, g->retType);
                Buf rb;
                bufInit(&rb, g->arena);
                bufPrintf(&rb, "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s } }",
                          cType(g, rt), rt->name, ti.okVar, ti.okVar,
                          tryPayloadPath(g, &ti));
                cgReturn(g, bufCstr(&rb));
                return;
            }
            const char *v = genExpr(g, s->u.ret.value);
            flushPrefix(g);              /* ⚠️ 必须在出口 **之前**（见上）*/
            cgReturn(g, v);
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            /* `break` / `continue` 要跳出这几层块 ⇒ 先把它们放掉 ✓
             * （放到**循环体那一层**为止；循环体自己的 arena 由下一轮进来时 reset）*/
            int to = g->loopLen ? g->loopLevel[g->loopLen - 1] : 1;
            for (int lv = g->blkLevel; lv >= to; lv--) cgReleaseLevel(g, lv);
            cgLine(g, "%s", s->kind == ST_BREAK ? "break;" : "continue;");
            return;
        }

        case ST_EXPR:
            /* `f()?` 单独成句：只要「求值一次 + 失败就 return」两句，后面没有用法 */
            if (s->u.expr.expr->kind == EX_TRY) {
                (void)genTryHead(g, s->u.expr.expr);
                return;
            }
            const char *ex = genExpr(g, s->u.expr.expr);
            flushPrefix(g);
            cgLine(g, "%s;", ex);
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_MATCH: {
            /* `match` → 一个 `switch`（枚举的变体在 C 里就是常量 `枚举名_变体名`）。
             * **穷尽性由类型检查担保**（check.c），所以这里不需要 `default` ——
             * 真漏了根本编不过，轮不到生成 C ✓
             *
             * 被 match 的表达式**只求值一次**：先装进一个临时变量。
             * （带载荷时要读 `.u.xxx`，重新求值就错了 —— 比如 `match f() { ... }`。）*/
            Type *et = ttBase(s->u.match.scrutinee->type);
            bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);
            const char *subj = genExpr(g, s->u.match.scrutinee);
            if (payload) {
                const char *tmp = arenaPrintf(g->arena, "__extc_m%d", g->tmpSeq++);
                cgLine(g, "%s %s = %s;", cType(g, et), tmp, subj);
                subj = tmp;
            }

            /* ⚠️ **故意不用 `switch`**（2026-09-20 真踩过，读行循环当场卡死）：
             * 分支体里的 `break` / `continue` 是要跳出**外面那个循环**的，
             * 而在 `switch` 里它只会跳出 switch ⇒ `while true` + `break` 永远不结束 ✗
             * if/else 链里没有这层"别人的 break" ✓
             * 穷尽性由类型检查担保 ⇒ 最后不需要 `else` ✓ */
            flushPrefix(g);
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                cgLine(g, "%s (%s%s == %s_%s) {", i == 0 ? "if" : "} else if",
                       subj, payload ? ".tag" : "", et ? et->name : "?", arm->variant);
                g->indent++;
                /* 绑定载荷：`circle(r) => ...` ⇒ `double r = tmp.u.circle._0;` */
                for (size_t k = 0; k < arm->binds.len; k++) {
                    Variant *v = et && et->edef ? NULL : NULL;
                    (void)v;
                    Type *bt = NULL;
                    if (et && et->edef) {
                        for (size_t j = 0; j < et->edef->variants.len; j++) {
                            Variant *vv = *(Variant **)vecAt(&et->edef->variants, j);
                            if (strcmp(vv->name, arm->variant) != 0) continue;
                            bt = *(Type **)vecAt(&vv->types, k);
                            /* 泛型枚举实例：载荷类型按实参替换（`just(T)` ⇒ `slice_u8`）*/
                            if (et->edef->typeParams.len > 0 &&
                                et->targs.len == et->edef->typeParams.len)
                                bt = ttSubstitute(g->tt, bt, &et->edef->typeParams, &et->targs);
                            break;
                        }
                    }
                    cgLine(g, "%s %s = %s.u.%s._%zu;", cType(g, bt), *(const char **)vecAt(&arm->binds, k),
                           subj, arm->variant, k);
                }
                genBlockBody(g, arm->body);
                g->indent--;
            }
            cgLine(g, "}");
            return;
        }
    }
}

/* ---------------------------------------------------------------- 顶层 */

/* 函数参数表的 C 文本。**`needsHome` 的函数在最后多收一只隐藏的 arena 指针**
 * （A3 逃逸提升）：它里面的 `new` 分配到**调用者选的那只** arena ✓ */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* ⚠️ `main` 的签名是 C 定死的（不能加隐藏参数）—— 它的"家"是自己函数体里
     * 那句 `extc_arena *__extc_home = &__extc_a[1];` ✓ */
    if (!f->owner && strcmp(f->name, "main") == 0) { bufPuts(&sig, "void"); return bufCstr(&sig); }
    if (f->params.len == 0 && !f->needsHome) { bufPuts(&sig, "void"); return bufCstr(&sig); }
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (i) bufPuts(&sig, ", ");
        bufPrintf(&sig, "%s %s", cType(g, p->type), p->cname ? p->cname : p->name);
    }
    if (f->needsHome) {
        if (f->params.len) bufPuts(&sig, ", ");
        bufPuts(&sig, "extc_arena *__extc_home");
    }
    return bufCstr(&sig);
}

static void genFunc(CG *g, FuncDef *f) {
    bool isMain = cgIsMain(f);
    if (isMain) {
        /* `main` 不能有隐藏参数（C 的签名定死了）⇒ 它的"家"就是自己函数体的 arena ✓ */
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "static %s %s(%s) {", cType(g, f->ret), cFuncName(g, f),
                  cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    /* `?` 要靠它构造「失败时往上一层返回什么」。临时变量编号在每个函数里重置，
     * 所以 `__extc_try0` 各函数各一份，不会撞。 */
    Type *savedRet = g->retType;
    int   savedSeq = g->tmpSeq;
    g->retType = subst(g, f->ret);
    g->tmpSeq = 0;
    /* ---- arena 按**块**细化（PLAN A2）----
     * 一只数组，**层数在编译期就算好了**（就是块嵌套的最大深度）⇒ 栈上定长、零分配 ✓
     * `{0}` 就够了（`extc_arena` 里只有个 `top` 指针，NULL = 空）✓
     * 函数体本身是**第 1 层**（`genBlockBody` 进来就 +1）✓ */
    int maxLv = 1 + blkMaxOfBlock(f->body);
    /* ⭐ 编译时长优化（2026-09-21）：**不会往自己的块 arena 里放东西的函数，连数组都不吐** ✓
     * 判据在检查器里算好（`f->mayUseArena`：体里有 `new`，或调用了"有家"的函数）✓
     * 省掉的是"纯计算"函数的样板 —— 真实例子里约一半函数属于这种 ✓
     * （`cgReleaseLevel` 里也同步跳过，见那边 ✓）*/
    bool savedNoArena = g->noArena;
    g->noArena = !f->mayUseArena;
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    /* ⭐ A：统一出口要用的**返回值落地变量**（只在本函数真的有出口释放时用）✓ */
    bool retVoid = !g->retType || g->retType->kind == TY_VOID;
    if (!g->noArena && !retVoid)
        cgLine(g, "%s __extc_ret_v;", cType(g, g->retType));
    g->blkLevel = 0;
    g->loopLen  = 0;
    bool savedHome = g->hasHome;
    g->hasHome = f->needsHome;
    if (isMain && f->needsHome)
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* main 的家 = 自己函数体 */");
    genBlockBody(g, f->body);
    /* ⭐ A：**统一出口** —— 释放串只吐一遍；自然结束也走这里 ✓
     * （`goto` 保证 label 一定有人用 ⇒ 不会有 `-Wunused-label` 警告 ✓）
     * ⚠️ 释放**所有**层：return 可能从更深的块里跳出来 ⇒ 各层都得放 ✓
     *    （放一只空 arena 是 no-op，代价可忽略 ✓）*/
    if (!g->noArena) {
        cgLine(g, "goto __extc_ret;");
        g->indent--;
        cgLine(g, "__extc_ret:");
        g->indent++;
        for (int lv = 1; lv <= maxLv; lv++)
            cgLine(g, "extc_arena_release(&__extc_a[%d]);", lv);
        if (isMain)       cgLine(g, "return 0;");   /* 生成的 C 里 main 是 int ✓ */
        else if (retVoid) cgLine(g, "return;");
        else              cgLine(g, "return __extc_ret_v;");
    }
    g->retType = savedRet;
    g->tmpSeq = savedSeq;
    g->hasHome = savedHome;
    g->noArena = savedNoArena;
    g->indent--;
    cgLine(g, "}");
}

/* ---------------------------------------------------------------- 泛型实例（单态化）
 *
 * check 只对模板检查一遍；这里**按实例生成 N 份 C**。
 * 容器的源码是 extC 写的（预lude），编译器只做「按实参把 T 代进去」这一件事。
 */

/* ⭐ **生成的东西一律 `static`**（除了 C 定位死的 `main`）——
 * 这不是风格，是**性能**：extC 只吐**一个 .c**（单 TU），外链毫无用处，
 * 却让 gcc 必须假设"别处还会调它、指针会别名" ⇒ **外层循环向量化被直接拒掉** ✗
 *
 * 实测（OI 数量级矩阵乘 n=1000，10⁹ 次乘加，同一份代码只差链接性）：
 *     static 208 ms   vs   extern 660 ms   ⇒ **3.2×** ✓
 * （`-fopt-info-vec-missed` 的原文：extern 那版报 "unsupported outerloop form"，
 *   static 那版报 "outer-loop already vectorized"）
 * 内链还白送：跨函数内联 / 常量传播更狠，`-Wl,--gc-sections` 也不再是唯一兜底 ✓
 *
 * ⚠️ 将来若真要**多 TU**（extC 现在不支持），这条得改成"只导出被别的 TU 用到的" ✓ */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

static void genFuncProto(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    bufPrintf(&sig, "%s%s %s(%s);", cgIsMain(f) ? "" : "static ", cType(g, f->ret),
              cFuncName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
}

/* ---------------------------------------------------------------- struct 定义顺序
 *
 * 普通 struct 和泛型实例会**互相包含**（`player` 里有 `slice<u8>`，
 * 而 `slice<u8>` 的字段又是普通的 `i64`），所以不能简单地「先普通再实例」。
 * 这里按**依赖顺序**出定义 —— 一劳永逸，将来 `array<point>` 之类也不会有问题。
 * 环只可能通过 `ref`（指针），而指针只需要前面的 typedef，所以先出全部 typedef。
 */

typedef struct {
    StructDef *sd;      /* 普通 struct；泛型实例和数组为 NULL */
    Type      *inst;    /* 泛型实例或数组；普通 struct 为 NULL */
    TypeDef   *td;      /* **带载荷的枚举**；其他为 NULL */
    Vec        deps;    /* int* —— 依赖的 unit 下标 */
    bool       done;
} SUnit;

static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* 泛型实例/数组（含泛型枚举实例）*/
    if (u->td) return u->td->name;          /* 非泛型带载荷枚举 */
    return u->sd->name;
}

static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_ENUM && u->td && t->edef == u->td) return (int)i;
        /* ⚠️ 泛型实例按 **C 名字** 找，不按指针比：
         *   ① `mut slice<T>` 是**影子**（跟只读版共用名字、共用结构体）——
         *      拿指针比会找不到 ⇒ 依赖排序漏掉它 ⇒
         *      `struct reader { chunk: mut slice<u8> }` 报 **incomplete type** ✗
         *   ② 同一个 C 名字可能有两个实例（可写/只读视图各一个）✓ */
        if ((t->kind == TY_GENERIC || t->kind == TY_ARRAY) && u->inst &&
            strcmp(u->inst->name, t->name) == 0) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

static void unitBody(CG *g, SUnit *u) {
    /* **带载荷的枚举**：`struct { tag; union }` —— 跟 struct 一样参与依赖排序，
     * 因为载荷里可能有别的 struct（`| holding(slice<u8>)` 就是一个真踩过的坑：
     * 排在 `slice_u8` 前面会报 unknown type name）✓ */
    if (u->td) {
        /* 泛型枚举实例（`option<i64>`）：进替换上下文，载荷类型交给 `cType` 替换 ✓ */
        if (u->inst) substEnter(g, u->inst);
        cgLine(g, "struct %s {", unitName(u));
        g->indent++;
        cgLine(g, "int tag;");
        cgLine(g, "union {");
        g->indent++;
        for (size_t j = 0; j < u->td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&u->td->variants, j);
            if (v->types.len == 0) continue;
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "struct { ");
            for (size_t k = 0; k < v->types.len; k++) {
                if (k) bufPuts(&b, " ");
                bufPrintf(&b, "%s _%zu;", cType(g, *(Type **)vecAt(&v->types, k)), k);
            }
            bufPrintf(&b, " } %s;", v->name);
            cgLine(g, "%s", bufCstr(&b));
        }
        g->indent--;
        cgLine(g, "} u;");
        g->indent--;
        cgLine(g, "};");
        cgLine(g, "");
        if (u->inst) substLeave(g);
        return;
    }

    bool generic = u->inst && u->inst->kind == TY_GENERIC;
    if (generic) substEnter(g, u->inst);

    cgLine(g, "struct %s {", unitName(u));
    g->indent++;
    if (u->inst && u->inst->kind == TY_ARRAY) {
        /* 数组就是一个「装着裸数组的结构体」—— 这就是值语义的来源 */
        cgLine(g, "%s data[%lld];", cType(g, u->inst->inner),
               (long long)u->inst->asize);
    } else {
        for (size_t i = 0; i < u->sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "%s %s;", cType(g, fd->type), fd->name);
        }
        /* C 不允许**空 struct**（`struct unit { };` 是 GNU 扩展，而且
         * `(unit){0}` 会触发 excess elements 警告）。所以零字段的 struct
         * 补一个占位字节 —— 用户看不见它，`(T){0}` 也就合法了。
         * `unit`（`result<unit, E>` 用）就是靠这条活下来的。 */
        if (u->sd->fields.len == 0) cgLine(g, "char __extc_empty;");
    }
    g->indent--;
    cgLine(g, "};");
    cgLine(g, "");
    if (generic) substLeave(g);
}

/* ⚠️ 数组的派生 `_eq` **没了**（第④步）：数组的 `==` 现在一律走
 * `extc_eq(&a, &b, &arr_desc)` —— 一张描述表 + 一份递归实现 ✓
 * （旧形状：每个数组类型一份循环体 —— 压测里 200 个类型 = 2450 行、占 32% ✗）*/

/* 登记一个切片 helper（去重）。base 是底的类型（数组或视图），
 * st 是结果切片类型，tail = 「切到尾」（`s[lo..]`，省略的界是底的长度）。
 *
 * 名字 = `extc_slice`（或 `extc_sliceTo`）+ 底的 C 名字 —— 一个底类型只会切成
 * 一种切片，所以不会撞。
 * 这里**故意不写出拼接后的例子**：ARRAYS.md 有一条机械验收（源码里不许出现
 * 切片类型的 C 名字模式），注释也不破例。 */
static const char *sliceHelper(CG *g, Type *ob, Type *st, bool tail) {
    const char *name = arenaPrintf(g->arena, "extc_slice%s_%s",
                                   tail ? "To" : "", ob->name);
    for (size_t i = 0; i < g->helpers.len; i++)
        if (strcmp(((SliceHelper *)vecAt(&g->helpers, i))->name, name) == 0)
            return name;

    const char *ret = cType(g, st);
    Buf b;
    bufInit(&b, g->arena);
    if (ob->kind == TY_ARRAY) {
        /* 固定数组：长度是编译期常数，直接写死 */
        bufPrintf(&b, "static %s %s(%s *a, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, %lld, f, ln);\n",
                  (long long)ob->asize);
        bufPrintf(&b, "    return (%s){ .data = &a->data[s], .len = hi - lo };\n}\n", ret);
    } else if (tail) {
        /* 切到尾：hi 就是 s.len —— 收参数而不是就地展开，所以 s 只求值一次 */
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, const char *f, int ln) {\n",
                  ret, name, ob->name);
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, v.len, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = v.len - lo };\n}\n", ret);
    } else {
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = hi - lo };\n}\n", ret);
    }

    SliceHelper *h = (SliceHelper *)vecPush(&g->helpers);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/* `a[lo..hi]` —— 切一个只读视图出来。
 *
 * **P（凡是编译期能证明的，运行时不留痕迹）在这里分两条路**：
 *   ① 底是固定数组，且两个界都是字面量（省略的界由 check 补成字面量）
 *      → 范围在 check 阶段就算清楚了，越界早就报过错了，
 *        生成的 C 就是 `&a.data[2]` / `.len = 3`，**一个检查都没有**。
 *   ② 其余情况 → 生成一个 helper 做运行时检查（trap 带 extC 位置）。 */
static const char *genSlice(CG *g, Expr *e) {
    Type *ob = ttBase(subst(g, e->u.slice.obj->type));
    Type *st = subst(g, e->type);
    if (!ob || !st || !ob->name) {
        ctxError(g->ctx, e->line, 1, NULL, "cannot generate a slice of this type");
        return "0";
    }

    const char *obj = genExpr(g, e->u.slice.obj);
    Expr *lo = e->u.slice.lo, *hi = e->u.slice.hi;
    /* ⚠️ 底是 **`ref [N]T`**（C 里是指针）时：
     *   · 取 `.data` 要**解一层**：`(*p).data[..]`
     *   · 但传给切片原语的**参数本身就是那个指针**（原语收 `array_*` ✓）
     *   （PLAN #25：不处理就生成 `p.data[..]` ⇒ gcc 报 'p' is a pointer ✗）*/
    bool objIsRef = e->u.slice.obj->type &&
                    subst(g, e->u.slice.obj->type)->kind == TY_REF;

    if (ob->kind == TY_ARRAY && lo && hi &&
        lo->kind == EX_INT && hi->kind == EX_INT) {
        const char *arr = objIsRef ? arenaPrintf(g->arena, "(*%s)", obj) : obj;
        return arenaPrintf(g->arena, "(%s){ .data = &(%s.data[%lld]), .len = %lld }",
                           cType(g, st), arr, lo->u.ival, hi->u.ival - lo->u.ival);
    }

    const char *loS = lo ? genExpr(g, lo) : "0";
    const char *arg = ob->kind != TY_ARRAY ? obj
                      : (objIsRef ? obj : arenaPrintf(g->arena, "&(%s)", obj));
    if (!hi) {
        /* 只可能是视图底 —— 数组底在 check 里已经把界补成字面量了 */
        const char *fn = sliceHelper(g, ob, st, true);
        return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), \"%s\", %d)",
                           fn, arg, loS, g->path, e->line);
    }
    const char *fn = sliceHelper(g, ob, st, false);
    const char *hiS = genExpr(g, hi);
    return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), (int64_t)(%s), \"%s\", %d)",
                       fn, arg, loS, hiS, g->path, e->line);
}


/* ⭐ PLAN #27：**实例集合的传递闭包**（2026-09-21）
 *
 * 问题：方法签名里提到的实例如果没在程序里被"直接用到过"，就不会进 `units`
 *   `varArray<i64>::get() -> option<i64>` ⇒ 生成的 C 报 `unknown type name 'option_i64'` ✗
 *   （用户什么都没写错 —— 是编译器自己的账没算全）
 *
 * 做法：扫每个 unit 的**字段 + 方法签名 + 枚举载荷**（泛型实例要**代入** T），
 * 把里面提到的"结构体类"补进 `units`，反复跑到不动点 ✓
 * 带**轮数上限**，超了**响亮报错** —— 不许静默少生成（ARENA-FORMAL §8.5 那条规矩）✗
 */
static void addInstanceUnit(Arena *arena, Vec *units, Type *t) {
    if (!t) return;
    bool isStructInst = (t->kind == TY_GENERIC && t->sdef);          /* varArray<i32> 这种 */
    bool isEnumInst   = (t->kind == TY_ENUM && t->edef && t->edef->typeParams.len > 0);
    if (!isStructInst && !isEnumInst) return;
    /* ⚠️ 去重必须按 **C 名字**，不能直接用 `unitFind`：
     *   `unitFind` 对**泛型枚举实例**是按 `edef`（模板）比的 ⇒ `option_i64` 会匹配到
     *   已经存在的 `option_i32`（两者共用 `edef`）⇒ 误判"已经有了" ⇒ 不生成 ⇒
     *   生成的 C 报 `unknown type name 'option_i64'` ✗（压测程序里抓出来的 ✓）
     *   （原始收集走的是 `tt->enumInstances` 列表 ⇒ 那时没暴露这个问题 ✓）*/
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (u->inst && t->name && strcmp(u->inst->name, t->name) == 0) return;
        if (u->td && t->kind == TY_ENUM && u->inst == NULL && u->td == t->edef) return;
    }
    SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
    if (isStructInst) u->sd = t->sdef; else u->td = t->edef;
    u->inst = t;
    vecInit(&u->deps, arena, sizeof(int));
    *(SUnit **)vecPush(units) = u;
}

/* 扫一个类型：它自己 + 里面提到的（引用/切片的内层、数组元素、泛型实参）✓ */
static void scanTypeForUnits(Arena *arena, Vec *units, Type *t, int depth) {
    if (!t || depth > 12) return;                                    /* 自我引用靠 depth 兜底 ✓ */
    addInstanceUnit(arena, units, t);
    if (t->inner) scanTypeForUnits(arena, units, t->inner, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        scanTypeForUnits(arena, units, *(Type **)vecAt(&t->targs, i), depth + 1);
}

static void scanUnitForUnits(Arena *arena, TypeTable *tt, Vec *units, SUnit *u) {
    Vec *tps = NULL; Vec *tas = NULL;
    StructDef *sd = u->sd ? u->sd : (u->inst ? u->inst->sdef : NULL);
    if (u->inst && sd) { tps = &sd->typeParams; tas = &u->inst->targs; }
    /* ① 字段 */
    if (sd) for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (tps && ft) ft = ttSubstitute(tt, ft, tps, tas);
        scanTypeForUnits(arena, units, ft, 0);
    }
    /* ② 方法签名（参数 + 返回值）—— **#27 漏的就是这里** */
    if (sd) for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&sd->methods, i);
        for (size_t j = 0; j < f->params.len; j++) {
            Type *pt = (*(Param **)vecAt(&f->params, j))->type;
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
        Type *rt = f->ret;
        if (tps && rt) rt = ttSubstitute(tt, rt, tps, tas);
        scanTypeForUnits(arena, units, rt, 0);
    }
    /* ③ 枚举载荷 */
    if (u->td) for (size_t i = 0; i < u->td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&u->td->variants, i);
        for (size_t k = 0; k < v->types.len; k++) {
            Type *pt = *(Type **)vecAt(&v->types, k);
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
    }
}

bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out) {
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.tt = tt;

    vecInit(&g.structs, arena, sizeof(void *));
    bufInit(&g.prefix, arena);          /* 语句前缀（PLAN #19）—— 忘初始化就是段错误 ✗ */
    /* **按 C 名字去重**：可写视图和只读视图是**同一个 C 结构体**
     * （`slice<mut slice<T>>` 和 `slice<slice<T>>` 都叫 `slice_slice_T`），
     * 而 `ttEquals` 把 `mut` 算进身份 ⇒ 类型表里会有**两个实例、一个名字**。
     * 按名字去重，后面的 struct / `_debug` / `_eq` / `writeText` 才不会各生成两份 ✓ */
    vecInit(&g.insts, arena, sizeof(void *));
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->instances, i);
        bool dup = false;
        for (size_t k = 0; k < g.insts.len && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&g.insts, k))->name, it->name) == 0;
        if (!dup) *(Type **)vecPush(&g.insts) = it;
    }
    vecInit(&g.funcs, arena, sizeof(void *));
    vecInit(&g.helpers, arena, sizeof(SliceHelper));
    vecInit(&g.descs, arena, sizeof(void *));
    vecInit(&g.eqNeed, arena, sizeof(void *));
    bufInit(&g.desc, arena);
    bufInit(&g.rt, arena);              /* 打印/比较运行时：**按需**才拼进输出 ✓ */
    bufInit(&g.rtPrint, arena);
    bufInit(&g.rtEq, arena);
    bufInit(&g.body, arena);
    g.tmpSeq = 0;
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (sd->typeParams.len > 0) continue;   /* 泛型：按实例生成，不走这里 */
        *(StructDef **)vecPush(&g.structs) = sd;
        /* 方法也是函数 —— 一起进原型/定义的表 */
        for (size_t j = 0; j < sd->methods.len; j++)
            *(FuncDef **)vecPush(&g.funcs) = *(FuncDef **)vecAt(&sd->methods, j);
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        *(FuncDef **)vecPush(&g.funcs) = *(FuncDef **)vecAt(&m->funcs, i);

    bufPuts(out,
        "/* Generated by the extC compiler -- do not edit by hand.\n"
        " * The generated C is deliberately boring: no macro tricks, no clever optimizations.\n"
        " * `#line` directives map the C compiler's errors back to the .extc source lines.\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"      /* offsetof —— 描述表要用 */
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <stdlib.h>\n\n"
        "/* ⚠️ 下面这些原语全部 `static inline` —— **这不是风格问题**：\n"
        " * 不内联的话，gcc 在 -O1 下**看不见检查体**，于是既不能消掉检查、\n"
        " * 也不能把 `i % 7` 变成乘法+移位。实测（2026-09-20）：取模慢 4.6 倍、\n"
        " * 矩阵乘慢 1.5 倍；加了 inline 之后**全部追平 C** ✓ */\n"
        "/* 越界 trap：带 extC 的位置（由 `#line` 与调用点传进来的 file/line 保证）*/\n"
        "static inline void extc_trap(const char *file, int line, int64_t i, int64_t n) {\n"
        "    fprintf(stderr, \"%s:%d: trap: index %lld out of range (length %lld)\\n\",\n"
        "            file, line, (long long)i, (long long)n);\n"
        "    exit(1);\n"
        "}\n"
        "/* ---- 算术的失败必须**响亮**（LANGUAGE.md 0.5）：\n"
        " * 除零、除法的溢出、移位超宽，在 C 里都是 UB —— 我们让它 trap 带源码位置。\n"
        " * 不静默算错，也不留 UB ✓ */\n"
        "static inline void extc_trapMsg(const char *file, int line, const char *msg) {\n"
        "    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, msg);\n"
        "    exit(1);\n"
        "}\n"
        "static inline int64_t extc_divI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a % b;\n"
        "}\n"
        "static inline uint64_t extc_divU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline uint64_t extc_modU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a % b;\n"
        "}\n"
        "/* 移位：C 里移 >= 位宽 或 负数 都是 UB ⇒ 检查移位数，返回它（求值一次）*/\n"
        "static inline int64_t extc_shiftCount(int64_t b, int64_t w, const char *f, int l) {\n"
        "    if (b < 0 || b >= w) extc_trapMsg(f, l, \"shift count out of range\");\n"
        "    return b;\n"
        "}\n"
        "/* 带越界检查的下标：**返回下标**，所以调用点只求值一次。*/\n"
        "static inline int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line) {\n"
        "    if (i < 0 || i >= n) extc_trap(file, line, i, n);\n"
        "    return i;\n"
        "}\n"
        "/* 带范围检查的切片：要求 0 <= lo <= hi <= n，返回 lo。*/\n"
        "static inline int64_t extc_checkedRange(int64_t lo, int64_t hi, int64_t n,\n"
        "                          const char *file, int line) {\n"
        "    if (lo < 0 || hi < lo || hi > n) {\n"
        "        fprintf(stderr, \"%s:%d: trap: slice %lld..%lld is out of range (length %lld)\\n\",\n"
        "                file, line, (long long)lo, (long long)hi, (long long)n);\n"
        "        exit(1);\n"
        "    }\n"
        "    return lo;\n"
        "}\n\n");
    bufPuts(out,
        /* ------------------------------------------------------------------
         * arena：**每帧一只 + 按句柄分配**
         *
         * 一只 arena 就是一个词法作用域。它是个**块链表**：
         * 分配 = 往当前块里推一个指针；释放 = 把整条链还给系统。
         *
         * 为什么「按句柄」（`extc_arena *`）而不是「当前的」：
         * 容器（varArray）要记住**自己出生在哪只 arena**，增长时从那只要 ——
         * 于是**方法分配的内存活得过方法返回** ✓
         * 这正是 DESIGN 表里「VarArray<T> → arena = 所在 region」的落地。
         *
         * ⚠️ 进程内没有共享状态了（每帧一个对象），线程化时不用改结构。
         * ------------------------------------------------------------------ */
        "typedef struct extc_ablock { struct extc_ablock *prev; int64_t cap, used; char data[1]; } extc_ablock;\n"
        "typedef struct extc_arena { extc_ablock *top; } extc_arena;\n"
        "static inline void extc_arena_init(extc_arena *a) { a->top = NULL; }\n"
        "static inline void extc_arena_release(extc_arena *a) {\n"
        "    while (a->top) { extc_ablock *p = a->top->prev; free(a->top); a->top = p; }\n"
        "}\n"
        "/* 显式转换用（PLAN #23）：整数收窄 / 换符号 / 浮点转整数 ⇒ 装不下就 trap（带位置）*/\n"
        "static inline int64_t extc_narrowI(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {\n"
        "    if (v < lo || v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline uint64_t extc_narrowU(uint64_t v, uint64_t hi, const char *f, int l) {\n"
        "    if (v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline int64_t extc_convFloat(double v, int64_t lo, int64_t hi,"
        " const char *f, int l) {\n"
        "    if (!(v >= (double)lo && v <= (double)hi))"
        " extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n"
        "    return (int64_t)v;   /* 向零截断 = C 的规则 ✓ */\n"
        "}\n"
        "void *extc_arena_alloc(extc_arena *a, int64_t n) {\n"        "    if (n <= 0) n = 1;\n"
        "    n = (n + 7) & ~(int64_t)7;\n"
        "    if (!a->top || a->top->cap - a->top->used < n) {\n"
        "        int64_t cap = n > 4096 ? n : 4096;\n"
        "        extc_ablock *b = (extc_ablock *)malloc(sizeof(extc_ablock) + (size_t)cap);\n"
        "        if (!b) { fprintf(stderr, \"extc: out of arena memory\\n\"); exit(1); }\n"
        "        b->prev = a->top; b->cap = cap; b->used = 0;\n"
        "        a->top = b;\n"
        "    }\n"
        "    {\n"
        "        void *p = a->top->data + a->top->used;\n"
        "        a->top->used += n;\n"
        "        memset(p, 0, (size_t)n);   /* ⭐ 分配**永远清零** */\n"
        "        return p;\n"
        "    }\n"
        "}\n\n");

    /* ==================================================================
     * 类型描述表（descriptor table）+ **一份**通用打印器
     *
     * 以前：「每种用到的类型」派生一份 `_debug` 打印**代码** ——
     *       合成压测程序（N=1000）里有 2028 个、占生成 C 的 **22.9% 行**，
     *       而那个程序**一次都没打印过结构体**（全是废的 ✗）。
     * 现在：每类型只剩一份编译期**常量**（进 .rodata），打印逻辑全程序**一份** ✓
     *       而且"要不要印"能按需决定（见 generate() 里描述表的可达性闭包）。
     *
     * ⚠️ 这**不是**运行时反射：描述表是静态数据，类型、字段偏移、变体名
     *    全是编译期常量，gcc 全程看得见 ⇒ P′（"不可证必须响亮"）一点没松 ✓
     *
     * 同一张表以后还能喂给 `extc_eq`（结构化 ==）· 序列化 · hash ✓
     * ================================================================== */
    bufPuts(&g.rt,
        "/* ---- 类型描述表 ----\n"
        " * 每类型一份静态数据；`extc_print` 全程序只有一份。\n"
        " * size 的含义：标量/结构体 = sizeof(T)；数组/切片 = **元素步长**。\n"
        " * 视图的 C 布局固定是 `{ T *data; int64_t len; }`（见 genViewUnit）。\n"
        " */\n"
        "enum {\n"
        "    EXTC_D_I8, EXTC_D_I16, EXTC_D_I32, EXTC_D_I64,\n"
        "    EXTC_D_U8, EXTC_D_U16, EXTC_D_U32, EXTC_D_U64,\n"
        "    EXTC_D_F32, EXTC_D_F64, EXTC_D_BOOL,\n"
        "    EXTC_D_ENUM,      /* table = const char *const[]，tag 在偏移 0 */\n"
        "    EXTC_D_STRUCT,    /* table = ExtcField[] */\n"
        "    EXTC_D_ARRAY,     /* 定长数组：count 个 elem */\n"
        "    EXTC_D_SLICE,     /* 视图：{ T *data; int64_t len; } */\n"
        "    EXTC_D_TEXT,      /* slice<u8>：按**文本**印（跟别的切片不一样）*/\n"
        "    EXTC_D_REF        /* ref / ?ref：一律印 <ref> */\n"
        "};\n"
        "\n"
        "typedef struct ExtcDesc ExtcDesc;\n"
        "typedef struct { const char *name; size_t off; const ExtcDesc *desc; } ExtcField;\n"
        "\n"
        "struct ExtcDesc {\n"
        "    int             kind;\n"
        "    const char     *name;    /* 结构体/枚举的显示名（打印用）*/\n"
        "    size_t          size;    /* 标量/结构体 = sizeof(T)；数组/切片 = 元素步长 */\n"
        "    size_t          count;   /* 字段数 / 变体数 / 数组长度 */\n"
        "    const void     *table;   /* ExtcField[] 或 const char *const[] */\n"
        "    const ExtcDesc *elem;    /* 数组/切片的元素 */\n"
        "    /* ⭐ 结构化 `==` 用：**只有 struct 才填** —— 那是用户（或库）写的 `fn ==`，\n"
        "     * 是**任意代码**，只能委托不能重造 ✓ codegen 为它生成一行适配器。\n"
        "     * 其余 kind 由 `extc_eq` 自己递归 ⇒ 这一格留空（位置在最后 ⇒ 老初始化式\n"
        "     * 少写一个也自动补 0 ✓）*/\n"
        "    bool          (*eq)(const void *a, const void *b);\n"
        "};\n"
        "\n"
        "/* 不依赖具体类型的四只：引用 / 字节视图 / 标量 —— 全程序共享 ✓ */\n"
        "static const ExtcDesc extc_desc_ref  = { EXTC_D_REF,  \"ref\",  sizeof(void *), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_text = { EXTC_D_TEXT, \"slice<u8>\", 1, 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_bool = { EXTC_D_BOOL, \"bool\", sizeof(bool), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i8  = { EXTC_D_I8,  \"i8\",  sizeof(int8_t),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i16 = { EXTC_D_I16, \"i16\", sizeof(int16_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i32 = { EXTC_D_I32, \"i32\", sizeof(int32_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i64 = { EXTC_D_I64, \"i64\", sizeof(int64_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u8  = { EXTC_D_U8,  \"u8\",  sizeof(uint8_t),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u16 = { EXTC_D_U16, \"u16\", sizeof(uint16_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u32 = { EXTC_D_U32, \"u32\", sizeof(uint32_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u64 = { EXTC_D_U64, \"u64\", sizeof(uint64_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_f32 = { EXTC_D_F32, \"f32\", sizeof(float),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_f64 = { EXTC_D_F64, \"f64\", sizeof(double), 0, NULL, NULL };\n"
        "\n");
    /* ⚠️ 分成两次 `bufPuts`：C99 只保证支持 4095 字符的字符串字面量，
     * 整块拼一起会触发 -Woverlength-strings（不是错，但没必要留着噪声）*/
    bufPuts(&g.rtPrint,
        "/* 通用递归打印器 —— 输出格式必须跟以前派生的 `_debug` **逐字节一致** ✓\n"
        " * （真值表见 tools/print-formats.txt：浮点 %g、[N]u8 按数字、slice<u8> 按文本 ……）*/\n"
        "static void extc_print(const void *p, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:   printf(\"%d\", (int)*(const int8_t *)p); return;\n"
        "    case EXTC_D_I16:  printf(\"%d\", (int)*(const int16_t *)p); return;\n"
        "    case EXTC_D_I32:  printf(\"%d\", (int)*(const int32_t *)p); return;\n"
        "    case EXTC_D_I64:  printf(\"%lld\", (long long)*(const int64_t *)p); return;\n"
        "    case EXTC_D_U8:   printf(\"%u\", (unsigned)*(const uint8_t *)p); return;\n"
        "    case EXTC_D_U16:  printf(\"%u\", (unsigned)*(const uint16_t *)p); return;\n"
        "    case EXTC_D_U32:  printf(\"%u\", (unsigned)*(const uint32_t *)p); return;\n"
        "    case EXTC_D_U64:  printf(\"%llu\", (unsigned long long)*(const uint64_t *)p); return;\n"
        "    case EXTC_D_F32:  printf(\"%g\", (double)*(const float *)p); return;\n"
        "    case EXTC_D_F64:  printf(\"%g\", (double)*(const double *)p); return;\n"
        "    case EXTC_D_BOOL: printf(\"%s\", *(const bool *)p ? \"true\" : \"false\"); return;\n"
        "    case EXTC_D_REF:  printf(\"<ref>\"); return;\n"
        "    case EXTC_D_TEXT: {\n"
        "        int64_t n = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "        printf(\"%.*s\", (int)n, (const char *)*(const void *const *)p);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ENUM: {\n"
        "        const char *const *names = (const char *const *)d->table;\n"
        "        int tag = *(const int *)p;\n"
        "        if (tag < 0 || (size_t)tag >= d->count) printf(\"<%s>\", d->name);\n"
        "        else                                    printf(\"%s\", names[tag]);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *f = (const ExtcField *)d->table;\n"
        "        printf(\"%s { \", d->name);\n"
        "        for (size_t i = 0; i < d->count; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            printf(\"%s: \", f[i].name);\n"
        "            extc_print((const char *)p + f[i].off, f[i].desc);\n"
        "        }\n"
        "        printf(\" }\");\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *data;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t len = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "            data = (const char *)*(const void *const *)p;\n"
        "            n = len > 0 ? (size_t)len : 0;\n"
        "        } else {\n"
        "            data = (const char *)p;\n"
        "            n = d->count;\n"
        "        }\n"
        "        printf(\"[\");\n"
        "        for (size_t i = 0; i < n; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            extc_print(data + i * d->size, d->elem);\n"
        "        }\n"
        "        printf(\"]\");\n"
        "        return;\n"
        "    }\n"
        "    }\n"
        "}\n\n");

    /* ⭐ 结构化 `==`：跟 `extc_print` **共用同一张描述表**（goal 第④步）✓
     *
     * 语义必须跟旧的"给每个数组类型派生一份 `_eq`"**逐位一致**：
     *   · 标量 / 枚举 ⇒ 直接 `==`（枚举只比 **tag** —— 载荷枚举本来就不可比，
     *     `typeHasEq` 挡着，走不到这里 ✓）
     *   · 数组 ⇒ 逐元素递归；**切片 ⇒ 长度相等 + 逐元素递归**
     *     （跟 prelude 里 `slice<T>::==` 一字不差：先比 len 再逐个比 ✓）
     *   · **struct ⇒ 调用户/库写的 `fn ==`** —— 描述表里那格 `eq` 就是它，
     *     由 codegen 生成一行适配器填上 ✓
     * ⇒ 于是"每个数组类型一份 `_eq` 函数"变成"每类型一份数据" ✓
     *   （压测：200 个不同的数组类型 ⇒ 2450 行派生代码 → 数据 + 适配器）
     */
    bufPuts(&g.rtEq,
        "static bool extc_eq(const void *a, const void *b, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:  return *(const int8_t *)a  == *(const int8_t *)b;\n"
        "    case EXTC_D_I16: return *(const int16_t *)a == *(const int16_t *)b;\n"
        "    case EXTC_D_I32: return *(const int32_t *)a == *(const int32_t *)b;\n"
        "    case EXTC_D_I64: return *(const int64_t *)a == *(const int64_t *)b;\n"
        "    case EXTC_D_U8:  return *(const uint8_t *)a  == *(const uint8_t *)b;\n"
        "    case EXTC_D_U16: return *(const uint16_t *)a == *(const uint16_t *)b;\n"
        "    case EXTC_D_U32: return *(const uint32_t *)a == *(const uint32_t *)b;\n"
        "    case EXTC_D_U64: return *(const uint64_t *)a == *(const uint64_t *)b;\n"
        "    case EXTC_D_F32: return *(const float *)a  == *(const float *)b;\n"
        "    case EXTC_D_F64: return *(const double *)a == *(const double *)b;\n"
        "    case EXTC_D_BOOL: return *(const bool *)a == *(const bool *)b;\n"
        "    case EXTC_D_REF:  return *(const void *const *)a == *(const void *const *)b;\n"
        "    case EXTC_D_ENUM: return *(const int *)a == *(const int *)b;\n"
        "    case EXTC_D_TEXT: {\n"
        "        const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "        const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "        if (la != lb) return false;\n"
        "        if (la <= 0) return true;      /* 空视图：data 可能是 null ⇒ 不比 ✓ */\n"
        "        return memcmp(*(const void *const *)a, *(const void *const *)b, (size_t)la) == 0;\n"
        "    }\n"
        "    case EXTC_D_STRUCT:\n"
        "        /* 用户写的 `fn ==`（适配器）；没有就说明根本不该被比 ✓ */\n"
        "        return d->eq ? d->eq(a, b) : false;\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *pa, *pb;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "            const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "            if (la != lb) return false;\n"
        "            pa = (const char *)*(const void *const *)a;\n"
        "            pb = (const char *)*(const void *const *)b;\n"
        "            n = la > 0 ? (size_t)la : 0;\n"
        "        } else {\n"
        "            pa = (const char *)a;\n"
        "            pb = (const char *)b;\n"
        "            n = d->count;\n"
        "        }\n"
        "        for (size_t i = 0; i < n; i++)\n"
        "            if (!extc_eq(pa + i * d->size, pb + i * d->size, d->elem)) return false;\n"
        "        return true;\n"
        "    }\n"
        "    }\n"
        "    return false;\n"
        "}\n\n");

    /* 枚举最靠前 —— C11 不能前置声明 enum tag，
     * 所以 struct 字段里用到枚举时必须先有定义 */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        if (!enumHasPayload(td)) {
            /* 无载荷：还是 C 的枚举（跟以前一模一样，一个字节都没变）*/
            bufPuts(&b, "typedef enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&b, ", ");
                bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&b, " } %s;", td->name);
            cgLine(&g, "%s", bufCstr(&b));
            cgLine(&g, "");
        } else if (td->typeParams.len > 0) {
            continue;      /* 泛型枚举：tag 常量和定义都由**实例**负责（见下）*/
        } else {
            /* **带载荷**：tag 常量现在就能出（它们不依赖任何东西），
             * 而 `struct { tag; union }` 的定义交给下面的**依赖排序**那一区 ——
             * 因为载荷里可能含别的 struct（`| holding(slice<u8>)`）✓ */
            Buf e2;
            bufInit(&e2, arena);
            bufPrintf(&e2, "enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&e2, ", ");
                bufPrintf(&e2, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&e2, " };");
            cgLine(&g, "%s", bufCstr(&e2));
            cgLine(&g, "");
            continue;               /* `_name` 也推迟到定义之后（它要读 v.tag）*/
        }

        /* 定案 11：枚举自动有名字文本 —— 现在由**描述表里的变体名数组**提供
         * （`<Type>_desc` 的 `table`），不再派生 `<Type>_name` 函数 ✓ */
    }

    /* 收集所有「struct 类」的东西：普通 struct + 泛型实例 */
    Vec units;
    vecInit(&units, arena, sizeof(void *));
    /* ⚠️ **按 C 名字去重**：可写视图和只读视图是同一个 C 结构体
     * （`slice<mut slice<T>>` 和 `slice<slice<T>>` 都叫 `slice_slice_T`），
     * 而 `ttEquals` 把 `mut` 算进身份 ⇒ 类型表里会有**两个实例、一个名字**。
     * 不去重的话 C 里会出现两份一模一样的 `struct` 定义 ⇒ redefinition 错误 ✓ */
    for (size_t i = 0; i < g.structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g.structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *it = *(Type **)vecAt(&g.insts, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = it->sdef;              /* 数组为 NULL */
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* **带载荷的枚举**也是「struct 类」的东西：它的 union 里按值装着载荷类型 */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!enumHasPayload(td)) continue;
        if (td->typeParams.len > 0) continue;    /* 泛型枚举：实例见下面 */
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = td;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* 泛型枚举的**实例**（`option<i64>`）各自是一份结构体定义 */
    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = it->edef;
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    /* ⭐ PLAN #27：**跑到不动点**把"方法签名里提到的实例"补进来 ✓
     * 带轮数上限；超了说明有意外情况 ⇒ **响亮报错**（不许静默少生成 ✗）*/
    {
        bool grew = true;
        int round = 0;
        const char *srcName = NULL, *newName = NULL;   /* 超限时报"谁造出了谁" ✓ */
        while (grew && round < 64) {
            grew = false;
            round++;
            size_t cur = units.len;                 /* 只看这一轮开始时的长度 ✓ */
            for (size_t i = 0; i < cur; i++) {
                SUnit *u = *(SUnit **)vecAt(&units, i);
                size_t before = units.len;
                scanUnitForUnits(arena, tt, &units, u);
                if (units.len != before) {
                    grew = true;
                    srcName = unitName(u);
                    for (size_t k = before; k < units.len; k++)
                        newName = unitName(*(SUnit **)vecAt(&units, k));
                }
            }
        }
        if (grew) {
            /* ⭐ 上限不是给"写挂的循环"兜底的，是防**实例套娃不收敛**
             * （单态化语言的共同做法：C++ 的 `-ftemplate-depth` / rustc 的 `recursion_limit`）；
             * 人家只报"太深了"，我们顺便报**是哪个实例在套娃** ⇒ 好定位 ✓ */
            fprintf(stderr,
                    "extc: error: generic instance closure did not settle in 64 rounds.\n"
                    "      last expansion: `%s` mentions `%s`, which needs more instances.\n"
                    "      This usually means instances nest without bound (such as\n"
                    "      `option<option<option<...>>>`). Use a concrete type, or report\n"
                    "      this program if you think it should compile.\n",
                    srcName ? srcName : "?", newName ? newName : "?");
            exit(1);
        }
    }

    /* 先出**全部** typedef —— 指针字段（`ref T`）只需要它 */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        cgLine(&g, "typedef struct %s %s;", unitName(u), unitName(u));
    }
    if (units.len) cgLine(&g, "");

    /* 算依赖：字段类型**按值**包含的另一个 struct 类 */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (u->td) {                    /* 枚举：载荷类型按值装在 union 里 */
            for (size_t j = 0; j < u->td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&u->td->variants, j);
                for (size_t k = 0; k < v->types.len; k++) {
                    Type *pt = *(Type **)vecAt(&v->types, k);
                    if (u->inst)                 /* 泛型实例：按实参替换 */
                        pt = ttSubstitute(tt, pt, &u->td->typeParams, &u->inst->targs);
                    if (pt->kind == TY_REF) continue;
                    int d = unitFind(&units, pt);
                    if (d >= 0 && d != (int)i) *(int *)vecPush(&u->deps) = d;
                }
            }
            continue;
        }
        if (u->inst && u->inst->kind == TY_ARRAY) {
            int j = unitFind(&units, u->inst->inner);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
            continue;
        }
        for (size_t k = 0; k < u->sd->fields.len; k++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, k);
            Type *ft = fd->type;
            if (u->inst) ft = ttSubstitute(tt, ft, &u->sd->typeParams, &u->inst->targs);
            if (ft->kind == TY_REF) continue;
            int j = unitFind(&units, ft);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
        }
    }

    /* 按依赖顺序出定义；有环（只可能通过值，C 本来就不允许）时硬出，让 C 去报 */
    for (;;) {
        bool progressed = false, allDone = true;
        for (size_t i = 0; i < units.len; i++) {
            SUnit *u = *(SUnit **)vecAt(&units, i);
            if (u->done) continue;
            allDone = false;
            bool ready = true;
            for (size_t k = 0; k < u->deps.len && ready; k++)
                ready = (*(SUnit **)vecAt(&units, *(int *)vecAt(&u->deps, k)))->done;
            if (!ready) continue;
            unitBody(&g, u);
            u->done = true;
            progressed = true;
        }
        if (allDone || !progressed) break;
    }
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (!u->done) { unitBody(&g, u); u->done = true; }
    }

    /* ⚠️ 以前这里给**带载荷枚举**出 `_name`（读 `v.tag`），所以必须排在定义之后。
     * 现在变体名在描述表的 `table` 里 ⇒ 整段没了 ✓
     * （枚举的 tag 常量在上面各自的位置就出完了，不受影响）*/

    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        TypeDef *td = it->edef;
        /* 实例的 tag 常量（`maybe_i64_nothing = 0`）—— 值跟基类型一致
         * （都按变体顺序，所以**零值 = tag 0** 这条对实例同样成立 ✓）*/
        Buf tb;
        bufInit(&tb, arena);
        bufPuts(&tb, "enum { ");
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&tb, ", ");
            bufPrintf(&tb, "%s_%s = %zu", it->name, v->name, j);
        }
        bufPuts(&tb, " };");
        cgLine(&g, "%s", bufCstr(&tb));
    }

    /* ------------------------------------------------------------------
     * ⚠️ **类型描述表不在这里出** —— 它在「原型区之后、函数体之前」，
     * 因为**要哪些描述得先看函数体**（`genPrint` 打谁）⇒ 见 `emitDescRegion` ✓
     * ------------------------------------------------------------------ */

    /* 全局变量 / 常量 —— **直接就是 C 的静态对象**（定长的全局不需要 arena）。
     * C 自动把静态对象清零，所以零初始化的全局不用 generate 任何初始化式。
     * ⚠️ 加 `static`（内链）是**性能**决定，理由见 `cgIsMain` 上面的注释：
     *    外链会让 gcc 拒掉外层循环向量化，OI 数量级的矩阵乘因此慢 **3.2×** ✗ */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(&g, gd->ann);
        if (gd->init) {
            cgLine(&g, "static %s %s = %s;", ct, gd->name, genExpr(&g, gd->init));
        } else {
            /* 没有初始化式 ⇒ C 的静态存储期自动清零（跟定案 8 一致）*/
            cgLine(&g, "static %s %s;", ct, gd->name);
        }
    }
    if (m->globals.len) cgLine(&g, "");


    /* ------------------------------------------------------------------
     * 原型区：**所有**函数的原型都得在**任何**函数体之前出来。
     *
     * 这一区里**只许放原型**，函数体一律不许出现。原因是真实踩过的坑：
     * 数组的 `==` 是编译器生成的，它的循环体要调用元素类型的 `fn ==`
     * （比如 `point_eq`）。曾经数组 `==` 的定义排在 `point_eq` 的原型前面，
     * C 就把 `point_eq` 当成隐式声明（`int()`），紧接着真原型一到就
     * "conflicting types for 'point_eq'" —— **示例代码抓出来的真 bug**。
     *
     * 教训跟 T4 那次一样：**顺序问题不要靠「碰巧对了」，要结构上排掉。**
     * ---------------------------------------------------------------- */
    /* ⚠️ `_debug` / `_writeText` / `_name` / 数组 `_eq` 的原型**都不再有了** ——
     * 打印和结构化 `==` 都走描述表（数据），没有派生函数要提前声明 ✓ */
    /* 实例的方法原型（数组没有方法，也没有 sdef）*/
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++)
            genFuncProto(&g, *(FuncDef **)vecAt(&inst->sdef->methods, j));
        substLeave(&g);
    }
    /* 普通 struct 的方法 + 自由函数：顺序无关，顺带支持互相调用 */
    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        const char *ret = cgIsMain(f) ? "int" : cType(&g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        /* ⚠️ 参数表**必须**跟定义用同一套（`cgParamList`）—— 有家 arena 的函数
         * 多一只隐藏参数，原型漏了就是"C 的类型对不上" ✗（真踩过）*/
        bufPrintf(&sig, "%s%s %s(%s);", cgIsMain(f) ? "" : "static ", ret,
                  cFuncName(&g, f), cgParamList(&g, f));
        cgLine(&g, "%s", bufCstr(&sig));
    }
    if (g.structs.len || g.insts.len || g.funcs.len) cgLine(&g, "");

    /* ================= 函数体区（下面全是定义，不再是原型） ============== */

    /* 函数体先写进临时 buf：切片 helper 是**生成过程中才发现**需要的，
     * 而 C 要求「先定义后使用」。所以最后按 原型 → helper → 函数体 拼回去。 */
    g.out = &g.body;

    /* 视图的下标原语（打印/比较**都不再派生任何函数**，见描述表 ✓）*/
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        if (isView(inst)) genViewIndexer(&g, inst);
        substLeave(&g);
    }

    /* 实例的方法定义 */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            genFunc(&g, *(FuncDef **)vecAt(&inst->sdef->methods, j));
            cgLine(&g, "");
        }
        substLeave(&g);
    }

    for (size_t i = 0; i < g.funcs.len; i++) {
        genFunc(&g, *(FuncDef **)vecAt(&g.funcs, i));
        cgLine(&g, "");
    }

    /* 收尾：把"生成过程中才知道要哪些"的东西接到函数体前面 ——
     *   ⭐ 打印运行时 + **类型描述表**（第三步：按需，看 genPrint 打过谁）
     *   ⭐ 切片 helper
     * 顺序：原型 → 描述 → 函数体 ✓（C 要求"先定义后使用"）*/
    g.out = out;
    emitDescRegion(&g);
    /* 描述表类型 + 共享标量描述：打印或比较**任一**需要就得有 ✓ */
    if (g.needRuntime || g.eqNeed.len) bufPuts(out, bufCstr(&g.rt));
    if (g.needRuntime)                 bufPuts(out, bufCstr(&g.rtPrint));
    if (g.eqNeed.len)                  bufPuts(out, bufCstr(&g.rtEq));
    bufPuts(out, bufCstr(&g.desc));
    for (size_t i = 0; i < g.helpers.len; i++)
        bufPuts(out, ((SliceHelper *)vecAt(&g.helpers, i))->text);
    bufPuts(out, bufCstr(&g.body));

    return !ctx->hasError;
}
