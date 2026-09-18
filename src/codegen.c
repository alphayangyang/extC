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
} CG;

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

/* 单态化：把当前实例上下文里的 TY_PARAM 换成实参 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
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

static const char *genExpr(CG *g, Expr *e);

    
/* 方法在 C 里要加 struct 前缀 —— `Point_eq` 和 `Board_eq` 不能撞。
 * （在 extC 里它们本来就是两个不同的名字，见 DECISIONS 决策 9）*/
/* extC 的符号名 → C 标识符片段。
 * 运算符在 extC 里就叫 `==`，但 C 里不能这么拼，所以映射一下。 */
static const char *cSymName(const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq" }, { "!=", "ne" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    return name;
}

static const char *cFuncName(CG *g, FuncDef *f) {
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s", g->ownerPrefix, cSymName(f->name));
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(f->name));
    return f->name;
}

/* 方法名修饰：接收者是泛型实例时用实例名（`Pair_i32_u8_getFirst`） */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s", rb->name, cSymName(f->name));
    /* ⚠️ 这里**不能**用 cFuncName —— 它带的是「当前正在生成的实例」前缀。
     * 被调用的方法可能属于另一个类型（在 Wrapper<Point> 里调 Point.==）。*/
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(f->name));
    return f->name;
}

/* 一个类型是不是「字节视图」？是的话 `println` 按文本打印。
 *
 * 这是编译器知道的**唯一**一件跟 slice 有关的事，而且它是**输出约定**：
 * 「指向 byte 的视图」该按文本打印 —— 这是输出的基本操作，不是容器的实现。
 * 容器的定义、字段、方法全在 stdlib/prelude.extc 里（见 MIGRATION.md 的验收标准）。
 */
static bool isByteView(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return false;
    if (strcmp(t->sdef->name, "slice") != 0) return false;
    return t->targs.len == 1 && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
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

/* 二元运算。
 * `==` / `!=` 在 check 里被解析成 eq 方法调用（找不到 eq 就报错）；
 * 泛型里含类型参数的比较被推迟到这里，用实例上下文再解析一次。 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;

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
                     "`==` inside a generic is checked at instantiation, not on the template -- the price of having no traits."
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
    if (t->kind == TY_ENUM) return "0";

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
    if (t->kind == TY_REF) return "__extc_reference_has_no_zero_value__";

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
        /* struct / 泛型实例自动递归打印 */
        if (bt->kind == TY_STRUCT || bt->kind == TY_GENERIC) {
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

static const char *genMethodCall(CG *g, Expr *e) {
    FuncDef *f = e->func;
    if (!f) return "0";

    Type *recvT = e->u.method.recv->type;
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* 接收者按需取地址 / 解引用 —— 这就是 `a.f(x)` == `f(a, x)` 的全部机制 */
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = recvT && recvT->kind == TY_REF;
    if (wantRef && !haveRef)      recvC = arenaPrintf(g->arena, "&(%s)", recvC);
    else if (!wantRef && haveRef) recvC = arenaPrintf(g->arena, "*(%s)", recvC);

    const char *fname = cMethodName(g, recvT, f);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
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
    Type *t = e->type;
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

static const char *genExpr(CG *g, Expr *e) {
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
        case EX_IDENT: return e->u.ident.name;

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

            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, name);
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
            }
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);

        case EX_REF:
            return arenaPrintf(g->arena, "&(%s)", genExpr(g, e->u.ref.operand));

        case EX_ENUMVAL:
            return arenaPrintf(g->arena, "%s_%s", e->u.enumval.typeName, e->u.enumval.variant);
    }
    return "0";
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
    if (t->kind == TY_ENUM)    { cgLine(g, "printf(\"%%s\", %s_name(%s));", t->name, expr); return; }
    if (t->kind == TY_STRUCT)  { cgLine(g, "%s_debug(%s);", t->name, expr); return; }
    if (t->kind == TY_REF)     { cgLine(g, "printf(\"<ref>\");"); return; }
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

static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

static void genBlockBody(CG *g, Stmt *block) {
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
}

static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);

    switch (s->kind) {
        case ST_VAR: {
            const char *init = s->u.var.init ? genExpr(g, s->u.var.init)
                                             : zeroInit(g, s->type);
            cgLine(g, "%s %s = %s;", cType(g, s->type), s->u.var.name, init);
            return;
        }

        case ST_ASSIGN:
            cgLine(g, "%s = %s;", genExpr(g, s->u.assign.target),
                   genExpr(g, s->u.assign.value));
            return;

        case ST_IF:
            cgLine(g, "if (%s) {", genExpr(g, s->u.ifs.cond));
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

        case ST_WHILE:
            cgLine(g, "while (%s) {", genExpr(g, s->u.whiles.cond));
            g->indent++;
            genBlockBody(g, s->u.whiles.body);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_RETURN:
            if (!s->u.ret.value) cgLine(g, "return;");
            else                 cgLine(g, "return %s;", genExpr(g, s->u.ret.value));
            return;

        case ST_BREAK:    cgLine(g, "break;");    return;
        case ST_CONTINUE: cgLine(g, "continue;"); return;

        case ST_EXPR:
            cgLine(g, "%s;", genExpr(g, s->u.expr.expr));
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;
    }
}

/* ---------------------------------------------------------------- 顶层 */

static void genFunc(CG *g, FuncDef *f) {
    if (!f->owner && strcmp(f->name, "main") == 0) {
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "%s %s(", cType(g, f->ret), cFuncName(g, f));
        if (f->params.len == 0) {
            bufPuts(&sig, "void");
        } else {
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                if (i) bufPuts(&sig, ", ");
                bufPrintf(&sig, "%s %s", cType(g, p->type), p->name);
            }
        }
        bufPuts(&sig, ") {");
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    genBlockBody(g, f->body);
    g->indent--;
    cgLine(g, "}");
}

/* ---------------------------------------------------------------- 泛型实例（单态化）
 *
 * check 只对模板检查一遍；这里**按实例生成 N 份 C**。
 * 容器的源码是 extC 写的（预lude），编译器只做「按实参把 T 代进去」这一件事。
 */

static void substEnter(CG *g, Type *inst) {
    g->substParams = &inst->sdef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
}

static void substLeave(CG *g) {
    g->substParams = NULL;
    g->substArgs   = NULL;
    g->ownerPrefix = NULL;
}

static void genFuncProto(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    bufPrintf(&sig, "%s %s(", cType(g, f->ret), cFuncName(g, f));
    if (f->params.len == 0) {
        bufPuts(&sig, "void");
    } else {
        for (size_t j = 0; j < f->params.len; j++) {
            Param *p = *(Param **)vecAt(&f->params, j);
            if (j) bufPuts(&sig, ", ");
            bufPrintf(&sig, "%s %s", cType(g, p->type), p->name);
        }
    }
    bufPuts(&sig, ");");
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
    StructDef *sd;
    Type      *inst;    /* NULL = 普通 struct */
    Vec        deps;    /* int* —— 依赖的 unit 下标 */
    bool       done;
} SUnit;

static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_GENERIC && u->inst == t) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

static void unitBody(CG *g, SUnit *u) {
    if (u->inst) substEnter(g, u->inst);
    cgLine(g, "struct %s {", u->inst ? u->inst->name : u->sd->name);
    g->indent++;
    for (size_t i = 0; i < u->sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, i);
        cgLine(g, "%s %s;", cType(g, fd->type), fd->name);
    }
    g->indent--;
    cgLine(g, "};");
    cgLine(g, "");
    if (u->inst) substLeave(g);
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
    vecInit(&g.funcs, arena, sizeof(void *));
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
        "#include <stdio.h>\n\n");

    /* 枚举最靠前 —— C11 不能前置声明 enum tag，
     * 所以 struct 字段里用到枚举时必须先有定义 */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        bufPuts(&b, "typedef enum { ");
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&b, ", ");
            bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
        }
        bufPrintf(&b, " } %s;", td->name);
        cgLine(&g, "%s", bufCstr(&b));

        /* 定案 11：无载荷枚举自动有名字文本。
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
    for (size_t i = 0; i < g.structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g.structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < tt->instances.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = (*(Type **)vecAt(&tt->instances, i))->sdef;
        u->inst = *(Type **)vecAt(&tt->instances, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    /* 先出**全部** typedef —— 指针字段（`ref T`）只需要它 */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        const char *n = u->inst ? u->inst->name : u->sd->name;
        cgLine(&g, "typedef struct %s %s;", n, n);
    }
    if (units.len) cgLine(&g, "");

    /* 算依赖：字段类型**按值**包含的另一个 struct 类 */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
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

    /* _debug 的原型先全出来：实例和普通 struct 会互相递归打印 */
    for (size_t i = 0; i < g.structs.len; i++)
        cgLine(&g, "void %s_debug(%s v);",
               (*(StructDef **)vecAt(&g.structs, i))->name,
               (*(StructDef **)vecAt(&g.structs, i))->name);
    for (size_t i = 0; i < tt->instances.len; i++) {
        const char *n = (*(Type **)vecAt(&tt->instances, i))->name;
        cgLine(&g, "void %s_debug(%s v);", n, n);
    }
    if (g.structs.len || tt->instances.len) cgLine(&g, "");

    /* 实例的自动调试打印（字段类型要先替换）+ 字节视图的文本输出 */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        substEnter(&g, inst);
        genStructDebug(&g, inst->name, inst->sdef);
        if (isByteView(inst)) genByteViewWriter(&g, inst->name);
        substLeave(&g);
    }
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (isByteView(inst)) cgLine(&g, "void %s_writeText(%s v);", inst->name, inst->name);
    }

    /* struct 的自动调试打印 */
    for (size_t i = 0; i < g.structs.len; i++)
        genStructDebug(&g, (*(StructDef **)vecAt(&g.structs, i))->name,
                       *(StructDef **)vecAt(&g.structs, i));

    /* 实例的方法原型 */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++)
            genFuncProto(&g, *(FuncDef **)vecAt(&inst->sdef->methods, j));
        substLeave(&g);
    }
    if (tt->instances.len) cgLine(&g, "");

    /* 原型：顺序无关，顺带支持互相调用 */
    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        const char *ret = (!f->owner && strcmp(f->name, "main") == 0)
                              ? "int" : cType(&g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        bufPrintf(&sig, "%s %s(", ret, cFuncName(&g, f));
        if (f->params.len == 0) {
            bufPuts(&sig, "void");
        } else {
            for (size_t j = 0; j < f->params.len; j++) {
                Param *p = *(Param **)vecAt(&f->params, j);
                if (j) bufPuts(&sig, ", ");
                bufPuts(&sig, cType(&g, p->type));
            }
        }
        bufPuts(&sig, ");");
        cgLine(&g, "%s", bufCstr(&sig));
    }
    if (g.funcs.len) cgLine(&g, "");

    /* 实例的方法定义 */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
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

    return !ctx->hasError;
}
