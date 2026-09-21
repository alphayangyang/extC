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
    int         loopLevel[64];
    int         loopLen;
    Vec         insts;          /* Type* —— **按 C 名字去重**后的泛型实例（见下）*/
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
static void genPrintValue(CG *g, Type *t, const char *expr);

    
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

/* 非字节视图的调试打印：跟数组一样打成一列元素。
 *
 * 不这么做的话 `println(v)` 会掉进 struct 的调试打印、输出
 * `slice { data: <ref>, len: 3 }` —— 一个**给不出信息**的地址占位。
 * 视图是「一片元素」，那就按元素打。字节视图不走这里（它按文本打）。 */
static void genViewDebug(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    cgLine(g, "void %s_debug(%s v) {", v->name, v->name);
    g->indent++;
    cgLine(g, "printf(\"[\");");
    cgLine(g, "for (int64_t i = 0; i < v.len; i++) {");
    g->indent++;
    cgLine(g, "if (i) printf(\", \");");
    genPrintValue(g, elem, "v.data[i]");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "printf(\"]\");");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
}

/* 按值收一个字节视图再打印 —— 保证实参只求值一次 */
static void genByteViewWriter(CG *g, const char *cname) {
    cgLine(g, "void %s_writeText(%s v) {", cname, cname);
    g->indent++;
    cgLine(g, "printf(\"%%.*s\", (int)v.len, (const char *)v.data);");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
}

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

/* 元素能不能比？数组的 `==` 是**编译器派生**的（跟 `_debug` 一样）——
 * 数组类型用户写不出来，也就没法给它们写 `fn ==`。条件是元素能比，递归定义。 */
static bool typeHasEq(Type *t) {
    if (!t) return false;
    if (t->kind == TY_ARRAY) return typeHasEq(t->inner);
    if (t->kind == TY_BUILTIN) return true;
    /* ⚠️ 枚举**要分开看**（2026-09-20 第三刀之后撞出来的真 bug）：
     * 带载荷的枚举在 C 里是 `struct { tag; union }` ⇒ **没有 `==`** ✗
     * 类型检查那边本来就禁止比它，这里必须跟它一致 —— 否则 `[4]?i64` 这种
     * 数组类型会生成一个编不过的 `_eq` helper（gcc: invalid operands to binary ==）✗ */
    if (t->kind == TY_ENUM) return !(t->edef && enumHasPayload(t->edef));
    return findOpMethod(t, "==", NULL) != NULL;
}

/* 元素比较的 C 表达式（递归：内建直接比，struct 走它的 `==`）*/
static const char *genEqTest(CG *g, Type *t, const char *x, const char *y) {
    if (!t) return "0";
    if (t->kind == TY_ARRAY)
        return arenaPrintf(g->arena, "%s_eq(%s, %s)", t->name, x, y);
    if (t->kind == TY_BUILTIN || t->kind == TY_ENUM)
        return arenaPrintf(g->arena, "((%s) == (%s))", x, y);

    FuncDef *m = findOpMethod(t, "==", NULL);
    if (!m) return "0";
    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    const char *a = p0->type->kind == TY_REF ? arenaPrintf(g->arena, "&(%s)", x) : x;
    const char *b = p1->type->kind == TY_REF ? arenaPrintf(g->arena, "&(%s)", y) : y;
    return arenaPrintf(g->arena, "%s(%s, %s)", cMethodName(g, t, m), a, b);
}

/* 二元运算。
 * `==` / `!=` 在 check 里被解析成 eq 方法调用（找不到 eq 就报错）；
 * 泛型里含类型参数的比较被推迟到这里，用实例上下文再解析一次。 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;

    /* 数组：编译器派生的 `==` / `!=`（数组没有 sdef，找不到方法）。
     *
     * 这里**不能**加 `!e->needEq` 的条件 —— 泛型里的比较推迟到实例化才解析，
     * 代入实参后元素类型可能正好是数组（`slice<[6]i32>` 里比较两行就是）。
     * 之前挡着，于是那种情况掉进下面的方法查找、报
     * 「`array_6_i32` needs to define `!=`」—— 一个假错误。 */
    if (!e->func) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ARRAY &&
            (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
            const char *call = genEqTest(g, lt, genExpr(g, e->u.bin.left),
                                                 genExpr(g, e->u.bin.right));
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

        /* 定案 11：无载荷枚举自动有名字文本 */
        if (bt->kind == TY_ENUM) {
            bufPrintf(&b, "printf(\"%%s\", %s_name(%s))", bt->name, code);
            continue;
        }
        /* 字节视图按文本打印 */
        if (isByteView(bt)) {
            bufPrintf(&b, "%s_writeText(%s)", bt->name, code);
            continue;
        }
        /* struct / 泛型实例 / 数组：自动递归打印 */
        if (bt->kind == TY_STRUCT || bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            bufPrintf(&b, "%s_debug(%s)", bt->name, code);
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
            if (!e->u.new_.count) {
                /* 一个 T（或一个 [N]T）的**地方** ⇒ 就是它的地址 ✓ */
                return arenaPrintf(g->arena, "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s)))",
                                   cType(g, w), arenaRef(g), cType(g, w));
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
                cType(g, st), cType(g, w), arenaRef(g), n, cType(g, w), n);
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
 * 每个 struct 生成一个 `<Type>_debug`，**递归**打印所有字段。
 *
 * 这一件必须由**编译器**做 —— 它等价于 Rust 的 `#[derive(Debug)]`：
 * extC 没有反射，「遍历所有字段」这件事在语言里写不出来。
 * 注意这跟「把标准库塞进编译器」是两回事：这是**编译器生成代码**。
 * 分界线见 DESIGN.md「能写在 extC 里的，就别写在编译器里」。
 */

static void genPrintValue(CG *g, Type *t, const char *expr) {
    if (!t) { cgLine(g, "printf(\"?\");"); return; }

    if (isByteView(t))         { cgLine(g, "%s_writeText(%s);", t->name, expr); return; }
    if (t->kind == TY_ARRAY)   { cgLine(g, "%s_debug(%s);", t->name, expr); return; }
    if (t->kind == TY_ENUM)    { cgLine(g, "printf(\"%%s\", %s_name(%s));", t->name, expr); return; }
    if (t->kind == TY_STRUCT)  { cgLine(g, "%s_debug(%s);", t->name, expr); return; }
    if (t->kind == TY_REF)     { cgLine(g, "printf(\"<ref>\");"); return; }
    /* 泛型实例（含视图）：走它自己的 `_debug` —— 之前这里落到 "?"，
     * 于是「struct 里放一个 slice<i32> 字段」打印出来是个问号 */
    if (t->kind == TY_GENERIC) { cgLine(g, "%s_debug(%s);", t->name, expr); return; }
    if (t->kind != TY_BUILTIN) { cgLine(g, "printf(\"?\");"); return; }

    if (strcmp(t->name, "bool") == 0) {
        cgLine(g, "printf(\"%%s\", (%s) ? \"true\" : \"false\");", expr);
        return;
    }
    for (size_t i = 0; PRINT_FMT[i].extc; i++) {
        if (strcmp(PRINT_FMT[i].extc, t->name) == 0) {
            cgLine(g, "printf(\"%s\", %s(%s));", PRINT_FMT[i].fmt, PRINT_FMT[i].cast, expr);
            return;
        }
    }
    cgLine(g, "printf(\"?\");");
}

static void genStructDebug(CG *g, const char *cname, StructDef *sd) {
    cgLine(g, "void %s_debug(%s v) {", cname, cname);
    g->indent++;
    cgLine(g, "printf(\"%s { \");", sd->name);      /* 显示名用 extC 原名 */
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (i) cgLine(g, "printf(\", \");");
        cgLine(g, "printf(\"%s: \");", fd->name);
        genPrintValue(g, subst(g, fd->type), arenaPrintf(g->arena, "v.%s", fd->name));
    }
    cgLine(g, "printf(\" }\");");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
}

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
    {
        Buf rel;
        bufInit(&rel, g->arena);
        for (int lv = g->blkLevel; lv >= 1; lv--)
            bufPrintf(&rel, "extc_arena_release(&__extc_a[%d]); ", lv);
        cgLine(g, "if (%s.tag != %s_%s) { %sreturn %s; }",
               ti.tmp, ti.inst, ti.okVar, bufCstr(&rel), genTryFail(g, &ti));
    }
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

/* 释放第 lv 层（`lvl > 0`；第 0 层不用）*/
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
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
    cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);   /* 进来先清（防御性）*/
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
            /* 返回之前**一定**要把所有层的 arena 都放掉 —— 释放时机是词法的，
             * 所以每个 return 都是一个释放点（包括 `?` 生成的那些）✓ */
            Buf relBuf;
            bufInit(&relBuf, g->arena);
            for (int lv = g->blkLevel; lv >= 1; lv--)
                bufPrintf(&relBuf, "extc_arena_release(&__extc_a[%d]); ", lv);
            const char *rel = bufCstr(&relBuf);
            if (!s->u.ret.value) {
                cgLine(g, "%s", rel);
                cgLine(g, "return;");
                return;
            }
            if (s->u.ret.value->kind == EX_TRY) {
                /* `return e?` —— 成功就把载荷装回**外层**返回类型 */
                TryInfo ti = genTryHead(g, s->u.ret.value);
                Type *rt = subst(g, g->retType);
                cgLine(g, "%s", rel);
                cgLine(g, "return (%s){ .tag = %s_%s, .u.%s = { ._0 = %s } };",
                       cType(g, rt), rt->name, ti.okVar, ti.okVar,
                       tryPayloadPath(g, &ti));
                return;
            }
            const char *v = genExpr(g, s->u.ret.value);
            flushPrefix(g);              /* ⚠️ 必须在 release **之前**（见上）*/
            cgLine(g, "%s", rel);
            cgLine(g, "return %s;", v);
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
    bool isMain = !f->owner && strcmp(f->name, "main") == 0;
    if (isMain) {
        /* `main` 不能有隐藏参数（C 的签名定死了）⇒ 它的"家"就是自己函数体的 arena ✓ */
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "%s %s(%s) {", cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
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
    cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    g->blkLevel = 0;
    g->loopLen  = 0;
    bool savedHome = g->hasHome;
    g->hasHome = f->needsHome;
    if (isMain && f->needsHome)
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* main 的家 = 自己函数体 */");
    genBlockBody(g, f->body);
    g->retType = savedRet;
    g->tmpSeq = savedSeq;
    g->hasHome = savedHome;
    g->indent--;
    cgLine(g, "}");
}

/* ---------------------------------------------------------------- 泛型实例（单态化）
 *
 * check 只对模板检查一遍；这里**按实例生成 N 份 C**。
 * 容器的源码是 extC 写的（预lude），编译器只做「按实参把 T 代进去」这一件事。
 */

static void genFuncProto(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    bufPrintf(&sig, "%s %s(%s);", cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
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

/* 数组的自动调试打印：`[1, 2, 3]` */
static void genArrayDebug(CG *g, Type *arr) {
    cgLine(g, "void %s_debug(%s v) {", arr->name, arr->name);
    g->indent++;
    cgLine(g, "printf(\"[\");");
    cgLine(g, "for (int64_t i = 0; i < %lld; i++) {", (long long)arr->asize);
    g->indent++;
    cgLine(g, "if (i) printf(\", \");");
    genPrintValue(g, arr->inner, "v.data[i]");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "printf(\"]\");");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
}

/* 数组的 `==`：编译器派生（数组类型用户写不出来，所以没法用 fn == 实现）*/
static void genArrayEq(CG *g, Type *arr) {
    if (!typeHasEq(arr->inner)) return;
    cgLine(g, "bool %s_eq(%s a, %s b) {", arr->name, arr->name, arr->name);
    g->indent++;
    cgLine(g, "for (int64_t i = 0; i < %lld; i++) {", (long long)arr->asize);
    g->indent++;
    cgLine(g, "if (!%s) return false;",
           genEqTest(g, arr->inner, "a.data[i]", "b.data[i]"));
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "return true;");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
}

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

        /* 定案 11：枚举自动有名字文本（带载荷的打印的是**变体名**，载荷不打印）。
         * 不写 static —— 免得没用到的枚举触发 -Wunused-function。 */
        cgLine(&g, "const char *%s_name(%s v) {", td->name, td->name);
        g.indent++;
        cgLine(&g, "switch (v) {");
        g.indent++;
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            cgLine(&g, "case %s_%s: return \"%s\";", td->name, v->name, v->name);
        }
        cgLine(&g, "default: return \"<%s>\";", td->name);
        g.indent--;
        cgLine(&g, "}");
        g.indent--;
        cgLine(&g, "}");
        cgLine(&g, "");
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

    /* **带载荷枚举**的 `_name`（定案 11）：它要读 `v.tag`，所以必须排在
     * 上面的定义之后。无载荷的那些在文件开头就出掉了 ✓ */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!enumHasPayload(td)) continue;
        if (td->typeParams.len > 0) continue;      /* 泛型：实例见下 */
        cgLine(&g, "const char *%s_name(%s v) {", td->name, td->name);
        g.indent++;
        cgLine(&g, "switch (v.tag) {");
        g.indent++;
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            cgLine(&g, "case %s_%s: return \"%s\";", td->name, v->name, v->name);
        }
        cgLine(&g, "default: return \"<%s>\";", td->name);
        g.indent--;
        cgLine(&g, "}");
        g.indent--;
        cgLine(&g, "}");
        cgLine(&g, "");
    }
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

        cgLine(&g, "const char *%s_name(%s v) {", it->name, it->name);
        g.indent++;
        cgLine(&g, "switch (v.tag) {");
        g.indent++;
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            cgLine(&g, "case %s_%s: return \"%s\";", it->name, v->name, v->name);
        }
        cgLine(&g, "default: return \"<%s>\";", it->name);
        g.indent--;
        cgLine(&g, "}");
        g.indent--;
        cgLine(&g, "}");
        cgLine(&g, "");
    }
    /* 全局变量 / 常量 —— **直接就是 C 的静态对象**（定长的全局不需要 arena）。
     * C 自动把静态对象清零，所以零初始化的全局不用 generate 任何初始化式。 */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(&g, gd->ann);
        if (gd->init) {
            cgLine(&g, "%s %s = %s;", ct, gd->name, genExpr(&g, gd->init));
        } else {
            /* 没有初始化式 ⇒ C 的静态存储期自动清零（跟定案 8 一致）*/
            cgLine(&g, "%s %s;", ct, gd->name);
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
    for (size_t i = 0; i < g.structs.len; i++)
        cgLine(&g, "void %s_debug(%s v);",
               (*(StructDef **)vecAt(&g.structs, i))->name,
               (*(StructDef **)vecAt(&g.structs, i))->name);
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *it = *(Type **)vecAt(&g.insts, i);
        cgLine(&g, "void %s_debug(%s v);", it->name, it->name);
        /* 数组的 `==` 也要原型 —— 嵌套数组之间是递归调用的 */
        if (it->kind == TY_ARRAY && typeHasEq(it->inner))
            cgLine(&g, "bool %s_eq(%s a, %s b);", it->name, it->name, it->name);
        if (it->kind == TY_GENERIC && isByteView(it))
            cgLine(&g, "void %s_writeText(%s v);", it->name, it->name);
    }
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
        const char *ret = (!f->owner && strcmp(f->name, "main") == 0)
                              ? "int" : cType(&g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        /* ⚠️ 参数表**必须**跟定义用同一套（`cgParamList`）—— 有家 arena 的函数
         * 多一只隐藏参数，原型漏了就是"C 的类型对不上" ✗（真踩过）*/
        bufPrintf(&sig, "%s %s(%s);", ret, cFuncName(&g, f), cgParamList(&g, f));
        cgLine(&g, "%s", bufCstr(&sig));
    }
    if (g.structs.len || g.insts.len || g.funcs.len) cgLine(&g, "");

    /* ================= 函数体区（下面全是定义，不再是原型） ============== */

    /* 函数体先写进临时 buf：切片 helper 是**生成过程中才发现**需要的，
     * 而 C 要求「先定义后使用」。所以最后按 原型 → helper → 函数体 拼回去。 */
    g.out = &g.body;

    /* 实例的自动调试打印（字段类型要先替换）+ 字节视图的文本输出 */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind == TY_ARRAY) {
            genArrayDebug(&g, inst);
            genArrayEq(&g, inst);
            continue;
        }
        substEnter(&g, inst);
        if (isView(inst) && !isByteView(inst))
            genViewDebug(&g, inst);
        else
            genStructDebug(&g, inst->name, inst->sdef);
        if (isView(inst))      genViewIndexer(&g, inst);
        if (isByteView(inst))  genByteViewWriter(&g, inst->name);
        substLeave(&g);
    }

    /* struct 的自动调试打印 */
    for (size_t i = 0; i < g.structs.len; i++)
        genStructDebug(&g, (*(StructDef **)vecAt(&g.structs, i))->name,
                       *(StructDef **)vecAt(&g.structs, i));

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

    /* 收尾：先把函数体区攒下来的切片 helper 放出来，再接上函数体 */
    g.out = out;
    for (size_t i = 0; i < g.helpers.len; i++)
        bufPuts(out, ((SliceHelper *)vecAt(&g.helpers, i))->text);
    bufPuts(out, bufCstr(&g.body));

    return !ctx->hasError;
}
