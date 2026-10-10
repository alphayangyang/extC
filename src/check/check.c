/* Type checking: infer the type of every expression and write it back into the AST.
 *
 * Three conversion rules drive every assignment, argument, and return check:
 *   - Widening (lossless) is implicit.
 *   - Narrowing (lossy) is always rejected; a lossy conversion has to be written out.
 *   - A literal takes the type its context expects, by value (`let x: f32 = 3.14`,
 *     `let x: u8 = 200`).
 *
 * After this pass codegen only translates; it never reasons about types again.
 */

#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort: sorting the table of module-level names */
#include <string.h>


#include "check_internal.h"
#include "plan.h"          /* 计划：只经访问器读取 */
/* ---------------------------------------------------------------- errors */

/**
 * @brief 在一个源码行上报告一个致命编译错误。
 *
 * 消息先渲染进固定大小的缓冲区，过长时截断而不是溢出。报告遵循"首个错误为准"：一旦错误标志
 * 置起，后续错误被丢掉，避免连锁诊断涌向用户。
 *
 * @param c     收集诊断的检查器。
 * @param line  诊断指向的源码行；没有已知位置时为 0。
 * @param note  消息下额外打印的一句说明，或 NULL。
 * @param fmt   消息的 printf 格式。
 * @param ...   `fmt` 的格式参数。
 */
void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

/**
 * @brief 在一个源码行上报告一个警告。
 *
 * 与报错唯一的区别是警告不置错误标志，编译继续，退出状态也不变。
 *
 * @param c     发出警告的检查器。
 * @param line  警告指向的源码行。
 * @param note  消息下额外打印的一句说明，或 NULL。
 * @param fmt   消息的 printf 格式。
 * @param ...   `fmt` 的格式参数。
 */
void ckWarn(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxWarn(c->ctx, line, 1, note, "%s", tmp);
}

/**
 * @brief 把类型渲染成用户书写的源码文本，用于诊断。
 *
 * lambda 的类型是生成的闭包结构体，用户写的是签名，所以诊断里显示签名。
 *
 * @param c  检查器，文本分配在它的 arena 里。
 * @param t  要渲染的类型；NULL 渲染成 `void`。
 * @return   以 NUL 结尾的字符串，在本次运行的余下时间里一直有效。
 */
const char *typeStr(Checker *c, Type *t) {
    /* lambda 的类型是生成的闭包结构体；用户写的是签名，诊断里就显示签名。 */
    if (t && t->sdef && anLamSig(t->sdef)) return anLamSig(t->sdef);
    Buf b;
    bufInit(&b, c->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/* ---------------------------------------------------------------- generated C names (renaming) */

/**
 * @brief 比较两个 `const char *` 名字，供模块名表排序使用。
 *
 * @param a  指向第一个名字的指针，按 `const char **` 解释。
 * @param b  指向第二个名字的指针，按 `const char **` 解释。
 * @return   `strcmp` 的结果：负、0、正分别表示前者小、相等、前者大。
 */
static int cmpNamePtr(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
/**
 * @brief 判断一个模块级名字是否已被占用。
 *
 * 这是编译期优化：逐个查模块级的各个向量会让命名在它们总长上变成二次的，一张排好序的表加二分
 * 查找避免这一点。
 *
 * 表只在四个向量的长度之和变化时重建：检查期间模块级名字只增不改，泛型实例记在类型表与代码
 * 生成那边，不进 `m->*`。
 *
 * @param c     持有缓存表的检查器。
 * @param name  要查的候选名字。
 * @return      函数、结构体、类型或全局已有这个名字时为 true。
 */
static bool moduleNameTaken(Checker *c, const char *name) {
    Module *m = c->m;
    size_t want = m->funcs.len + m->structs.len + m->types.len + m->globals.len;
    if (!c->moduleNames.arena) vecInit(&c->moduleNames, c->arena, sizeof(const char *));
    if (c->moduleNamesSize != want) {
        c->moduleNames.len = 0;
        for (size_t i = 0; i < m->funcs.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(FuncDef **)vecAt(&m->funcs, i))->name;
        for (size_t i = 0; i < m->structs.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(StructDef **)vecAt(&m->structs, i))->name;
        for (size_t i = 0; i < m->types.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(TypeDef **)vecAt(&m->types, i))->name;
        for (size_t i = 0; i < m->globals.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(GlobalDef **)vecAt(&m->globals, i))->name;
        qsort(c->moduleNames.data, c->moduleNames.len, sizeof(const char *), cmpNamePtr);
        c->moduleNamesSize = want;
    }
    size_t lo = 0, hi = c->moduleNames.len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, *(const char **)vecAt(&c->moduleNames, mid));
        if (r == 0) return true;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return false;
}

/**
 * @brief 给源名字分配它在生成 C 里的名字。
 *
 * `let` 只给值起名而不分配存储，所以 `let a = 1` 之后在同一作用域再写 `let a = a + 1` 合法：
 * 第二个 `a` 给这个名字新的含义，而第一个绑定连同它的存储留在原处。C 不允许同一块里两次声明
 * 同名，所以从第二次使用起，生成的 C 叫它 `a__2`。extC 源码里的名字始终不变 —— 改名是编译器
 * 内部的事，不能漏进用户视野。
 *
 * 由此有两条约束：
 *   - 同一个函数内后缀单调递增且不复用，生成的 C 因此读起来确定，不依赖 C 自己的块作用域规则。
 *   - 模块级名字（函数、结构体、全局）也要避开，否则局部会在生成的 C 里遮住函数名，`foo()` 就
 *     调不到了。
 *
 * @param c     检查器，持有每函数的名字使用计数。
 * @param name  源码里写的名字。
 * @return      要发出的名字；第一次使用返回原名字。字符串活在检查器的 arena 里。
 */
const char *cNameFor(Checker *c, const char *name) {
    NameUse *u = NULL;
    for (size_t i = 0; i < c->nameUses.len; i++) {
        NameUse *it = *(NameUse **)vecAt(&c->nameUses, i);
        if (strcmp(it->name, name) == 0) { u = it; break; }
    }
    int n = u ? u->count : 0;

    const char *cand;
    do {
        n++;
        cand = (n == 1) ? name : arenaPrintf(c->arena, "%s__%d", name, n);
        /* C 关键字也要避开：`let double = 3` 是合法 extC，但生成的 C 不能声明名为 `double`
         * 的变量。 */
    } while (moduleNameTaken(c, cand) || cIdentIsKeyword(cand));

    if (u) u->count = n;
    else {
        NameUse *fresh = (NameUse *)arenaAllocZero(c->arena, sizeof(NameUse));
        fresh->name = name;
        fresh->count = n;
        *(NameUse **)vecPush(&c->nameUses) = fresh;
    }
    return cand;
}

/* ---------------------------------------------------------------- scopes */

/**
 * @brief 进入一个新的词法作用域。
 *
 * 同时记下当前收窄表的长度作为水位，块结束时把收窄表恢复到它：块内证明的可空引用事实随块
 * 一起消失，与非空收窄的词法作用域一致。
 *
 * @param c  检查器。
 */
void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
    *(size_t *)vecPush(&c->narrowMarks) = c->narrow.len;
}

/**
 * @brief 退出最内层词法作用域，丢掉它的绑定并撤销它建立的非空事实。
 *
 * 收窄表按进入作用域时记下的水位截回。
 *
 * @param c  检查器。
 */
void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
    if (c->narrowMarks.len) {
        c->narrow.len = *(size_t *)vecAt(&c->narrowMarks, c->narrowMarks.len - 1);
        c->narrowMarks.len--;
    }
}

/**
 * @brief 把当前实例的类型实参替换进一个类型。
 *
 * @param c  检查器；替换上下文是 checker's `substParams` / `substArgs`。
 * @param t  要替换的类型。
 * @return   替换后的类型；没有正在进行的实例化或 `t` 为空时原样返回 `t`。
 */
Type *tsub(Checker *c, Type *t) {
    if (!c->substParams || !c->substArgs || !t) return t;
    return ttSubstitute(c->tt, t, c->substParams, c->substArgs);
}



/* ---------------------------------------------------------------- helpers */











/**
 * @brief 判断表达式是不是整数或浮点字面量。
 *
 * @param e  表达式；必须非 NULL。
 * @return   EX_INT 或 EX_FLOAT 时返回 true，这两类字面量的类型由上下文决定。
 */
bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

/**
 * @brief 判断该类型的值能否交给 `print` 或 `println`。
 *
 * 结构体、泛型实例与数组由编译器生成的 `<Type>_debug` 帮助函数递归打印；无载荷枚举总有名字
 * 可印。
 *
 * @param t  实参类型。
 * @return   存在打印方式时返回 true；t 为空或类型不受支持时返回 false。
 */
bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    if (b->kind == TY_ENUM) return true;
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC || b->kind == TY_ARRAY;
}

/**
 * @brief 判断字面量的值能否放进目标类型。
 *
 * 字面量在上下文给它类型之前没有自己的类型，所以 `3.14` 在 `f32` 上下文里成为 `f32`，
 * `200` 在 `u8` 上下文里成为 `u8`。无符号目标拒绝负的整数字面量；错误类型接受任何字面量，
 * 以免一条诊断级联出更多。
 *
 * 64 位的情形不靠移位判定：64 位字面量能持有的每个值都放得进有符号 64 位目标，无符号目标
 * 接受其中的非负值。
 *
 * @param e     字面量：EX_INT、EX_FLOAT 或 EX_BOOL。
 * @param want  上下文期望的类型。
 * @return      字面量的值在 want 里可表示时返回 true。
 */
bool literalFits(Expr *e, Type *want) {
    Type *w = ttBase(want);
    if (!w) return false;
    if (ttIsError(w)) return true;

    if (e->kind == EX_INT) {
        int bits = ttIntBits(w);
        if (bits > 0) {
            long long v = e->u.ival;
            if (bits == 64) return ttIntSigned(w) || v >= 0;
            if (ttIntSigned(w)) {
                long long lo = -(1LL << (bits - 1));
                long long hi = (1LL << (bits - 1)) - 1;
                return v >= lo && v <= hi;
            }
            if (v < 0) return false;
            return (unsigned long long)v <= ((1ULL << bits) - 1);
        }
        return ttIsFloat(w);
    }

    if (e->kind == EX_FLOAT) return ttIsFloat(w);
    if (e->kind == EX_BOOL)  return ttIs(w, "bool");
    return false;
}

/**
 * @brief 给说不出自己类型的表达式套上上下文期望的类型。
 *
 * 只处理裸 `null`、数组字面量和没有限定名的结构体字面量，并且只在上下文唯一确定类型时生效；
 * `null` 只接受可空引用的上下文，因为它无从知道目标类型。
 *
 * @param e     要补类型的表达式。
 * @param want  上下文期望的类型。
 */
void adoptContextType(Expr *e, Type *want) {
    if (!e || !want) return;
    if (e->kind == EX_NULL) {
        if (want->kind == TY_REF && want->nullable) e->type = want;
        return;
    }
    Type *w = ttBase(want);
    if (!w) return;
    /* 把上下文类型暂存在 `e->type`，随后的类型检查会用同一个类型覆盖它。泛型实例没有名字可查、
     * 数组字面量不知道自己的长度，这是它们唯一能得到类型的途径。 */
    if (e->kind == EX_ARRAYLIT) { e->type = w; return; }
    if (w->kind != TY_STRUCT && w->kind != TY_GENERIC) return;
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) e->type = w;
}

/**
 * @brief 检查 `got` 类型的值能否赋到期望 `want` 的位置。
 *
 * 只有无损转换是隐式的：加宽、非空引用转可空引用、丢掉 `mut` 等；其余转换要用户写出来。
 * 这是赋值、实参、字段初始化与载荷值的统一闸门。类型里含类型参数的赋值会被记录下来，留到
 * 实例化时按真实类型复核。
 *
 * `mut` 只有无损方向是隐式的：可写引用可以当只读用，反之要写成 `mut`。`?ref T` 转 `ref T`
 * 被拒绝：它声称引用非空，而那必须先被证明。
 *
 * @param c     检查器，用于渲染类型与报错。
 * @param want  期望类型。
 * @param got   被赋值表达式的类型。
 * @param node  该表达式，用于取行号与字面量适配。
 * @param what  目标描述（如 "argument"、"field `x`"），出现在诊断里。
 * @return      赋值合法时返回 true；不合法时已报错并返回 false。
 */
bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
    /* 两类值推迟：解析被推迟到实例化的方法调用（那里的 `got` 是错误类型），以及**任何**类型含类型
     * 参数的表达式 —— 例如 `fn run<T>` 里的 `s = s + c.value()`，和式的类型是 `T` 而赋值期望 `i64`。
     * 在模板上放行是对的，前提是把这一对期望类型与实际类型记下来，由每个实例复核，否则
     * `fn f<T>(x: T) -> i8 { return x.hash() }` 会在实例的 `hash` 返回 `i64` 时悄悄截断。 */
    if ((ttIsError(got) && node && node->kind == EX_METHOD && !planCallee(node) &&
         mentionsParam(node->u.method.recv->type)) ||
        (node && got && (got->kind == TY_PARAM || mentionsParam(got)))) {
        DeferredUse *du = (DeferredUse *)arenaAllocZero(c->arena, sizeof(DeferredUse));
        du->call  = node;
        du->want  = want;
        du->func  = c->curFunc;
        du->owner = c->curFunc ? c->curFunc->owner : NULL;
        *(DeferredUse **)vecPush(&c->deferredUses) = du;
    }
    if (ttIsError(want) || ttIsError(got)) return true;
    /* 类型参数同样还不可知；实例化时由记下的这一对落实真实类型，不符就在那里报出，并点出实例名。 */
    if (got && (got->kind == TY_PARAM || mentionsParam(got))) return true;

    /* ---- 装箱：把协程的帧值放进期望句柄类型的位置 ----
     * 表面类型只有 `coroutine<T>`，表示法是编译器的事。这里值是**帧**（检查器为那个协程合成的
     * 类型），期望类型是句柄，于是帧移进任务自己的位置，由代码生成发出句柄。产出的类型必须一致。 */
    if (want && got && isProtoType(want, "coroutine", 1) && got->sdef && planCoroOf(got->sdef) &&
        node && node->kind == EX_CALL) {
        Type *yt = want->targs.len ? *(Type **)vecAt(&want->targs, 0) : NULL;
        Type *ft = planYieldType(planCoroOf(got->sdef));
        if (yt && ft && strcmp(typeStr(c, yt), typeStr(c, ft)) == 0) {
            planSetBoxedCoro(node, true);
            node->type = want;
            planSetCoroBoxed(planCoroOf(got->sdef), true);
            return true;
        }
    }

    /* `ref T` 两侧都必须是引用。
     *
     * 陷阱很隐蔽：字面量适配那条路会调 `ttBase(want)`，而它把 `ref` 剥掉，于是 `p = 5` 在
     * `p: ref i64` 上看起来像"5 放得进 i64"而被放行。生成的 C 会是 `int64_t * p; p = 5;`，
     * 只有 gcc 会抱怨（`makes pointer from integer without a cast`）。类型检查必须在这里拦下。 */
    if (want->kind == TY_REF && got->kind != TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` can only be assigned another reference; "
                "to write through it, assign to what it points to (`p.field = ...`, `p[i] = ...`)",
                "%s expects `%s`, found `%s` -- a value is not a reference",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }
    if (want->kind != TY_REF && got->kind == TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` is not a value; extC has no implicit dereference",
                "%s expects `%s`, found `%s` -- dereference it first",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (want->kind == TY_REF && got->kind == TY_REF && got->nullable && !want->nullable &&
        ttEquals(want->inner, got->inner)) {
        ckError(c, node ? node->line : 0,
                "check it first: `if p != null { ... }` -- inside that branch the compiler"
                " knows it is not null and generates no runtime check; there is no other"
                " way to turn a `?ref T` into a `ref T`",
                "%s expects `%s`, found `%s` -- a nullable reference", 
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (ttEquals(want, got)) return true;

    /* 非空转可空是安全方向，因此隐式，和丢掉 `mut` 一样。 */
    if (want->kind == TY_REF && got->kind == TY_REF && want->nullable && !got->nullable &&
        want->mut == got->mut && ttEquals(want->inner, got->inner)) return true;

    /* **擦除指针**：`ref T` 可以交给 `ref void`（`mut ref T` 交给 `mut ref void`），相当于 C 的
     * `T *` -> `void *`。边界 API 用 `void *` 说"指向什么不关我的事"时就靠它。
     *
     * 它写成擦除而不是强制转换，靠下面三个条件守住：
     *   - `want` 的指向类型必须是 `void`，所以反向（`ref void` -> `ref T`）走不到这里，只有知道
     *     真实类型的声明（以及 `fn(...)` 符号这一情形）是回去的路；
     *   - `!got->nullable || want->nullable` 挡下 `?ref T` -> `ref void`：那等于声称它非空；
     *   - `!want->mut || got->mut` 挡下 `ref T` -> `mut ref void`：那等于索要写权限。
     *
     * `ref void` 能做的事严格更少：不能 `*`、不能 `[i]`、不能取字段，检查器三者都拒。 */
    if (want->kind == TY_REF && got->kind == TY_REF &&
        want->inner && ttBase(want->inner) && ttBase(want->inner)->kind == TY_VOID &&
        (!got->nullable || want->nullable) && (!want->mut || got->mut))
        return true;

    /* 丢掉 `mut` 是隐式的：可写的东西当然可以读。它单向且永远安全；反方向是索要写权限，必须写成
     * `mut`。引用（`mut ref T` -> `ref T`）与视图（`mut slice<T>` -> `slice<T>`）都是这个道理。 */
    if (want->kind == got->kind) {
        if (want->kind == TY_REF && got->mut && !want->mut &&
            ttEquals(want->inner, got->inner)) return true;
        /* 视图：`mut` 可以递归丢掉（`mut slice<mut slice<T>>` 可以当 `slice<slice<T>>` 用，
         * 反向不行）；判据见 `ttViewDowngradable`。 */
        if (want->kind == TY_GENERIC && ttViewDowngradable(want, got) &&
            !ttEquals(want, got)) return true;
    }
    if (ttCanWiden(got, want)) return true;
    if (node && literalFits(node, want)) return true;

    /* 放不下的整数字面量与类型不匹配是两种错误，所以给它自己的消息。（浮点字面量赋给整数变量
     * 是有损转换，落到下面那条通用消息。） */
    if (node && node->kind == EX_INT && ttIsInteger(want)) {
        ckError(c, node->line, NULL,
                "%s: literal `%lld` does not fit in `%s`",
                what, node->u.ival, typeStr(c, want));
        return false;
    }

    ckError(c, node ? node->line : 0,
            "extC only widens implicitly (lossless); a lossy conversion must be written out",
            "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
    return false;
}

/**
 * @brief 要求条件表达式的类型是 `bool`，否则报错。
 *
 * extC 没有隐式真值：整数或引用当条件用会被报出，而不是被转换。
 *
 * @param c     检查器。
 * @param t     条件的类型。
 * @param node  条件表达式，用于取源码位置。
 */
void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "conditions must be `bool` -- extC has no implicit truthiness",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

