/* The plan side table: accessors, setters, and the per-call-site alternative answers.
 *
 * Storage lives in `results.c`: one result slot per node, addressed by a **dense node
 * id**, so a result is never keyed by a node pointer and survives input immutability.
 * This file keeps what is specific to the plan: the meaning of each field, the setters
 * that record them, and the tables that answer "what does this call site resolve to".
 *
 * A setter with a NULL node is a no-op, and a lookup for a NULL node misses: some plan
 * questions are asked about nodes that do not exist.
 *
 * Two plan fields live on an AST node: the resolved callee (`Expr.func`) and the `for`
 * step (`Stmt.forStep`); their accessors below read the node. Do not add storage for them
 * without also making the instance set explicit (`forStep` is half syntax -- the parser
 * owns it). */
#include "plan.h"
#include "results.h"
#include <stdlib.h>

/* One bit per stored field. Macros, not an enum: a pedantic C11 compiler rejects
 * enumerator values above 31, and the analysis side has already grown past 32. */
#define PLAN_ARENA_LEVEL         0
#define PLAN_ZONE_LEVEL          1
#define PLAN_ARENA_ARG           2
#define PLAN_NEED_TEMP           3
#define PLAN_USES_HOME           4
#define PLAN_MAY_USE_ARENA       5
#define PLAN_MAKES_POOL          6
#define PLAN_COND_ALLOCS         7
#define PLAN_IS_CORO             8
#define PLAN_YIELD_TYPE          9
#define PLAN_CORO_FRAME_TYPE     10
#define PLAN_CORO_NEEDS_ZONE     11
#define PLAN_CORO_PROTO          12
#define PLAN_INST_NAME           13
#define PLAN_TEMPLATE            14
#define PLAN_CORO_BOXED          15
#define PLAN_FOR_STEP_DROPPED    16
#define PLAN_CALLEE              17
#define PLAN_USED                18
#define PLAN_CNAME               19
#define PLAN_BUILTIN_HOLDER      20
#define PLAN_CORO_OF             21
#define AN_LAM_SIG               22
#define AN_MAKES_POOL_ANY        23
#define PLAN_IMPL_TRAIT          24
#define PLAN_IMPL_TARGET         25
#define PLAN_USED_DYN            26
#define PLAN_USES_DYN            27
#define PLAN_PAR_WORKER          28
#define PLAN_DOM_NEW             29
#define PLAN_VIEW_OF             30
#define AN_REF_DEPTH             31
#define AN_HOME_DEPTH            32
#define AN_LEXICAL_LEVEL         33
#define AN_STORED_AT             34
#define PLAN_EXT_DOM             35
#define PLAN_FIELD               36
#define PLAN_ASSOC_OWNER         37
#define PLAN_DYN_RECV_VIA_REF    39
#define PLAN_CALL_VIA_FN         40
#define AN_MIN_AT                41
#define AN_REUSE                 42
#define AN_BORROWED              43
#define AN_QUALIFIED             44
#define AN_CONV_CHECK            45
#define AN_NEED_OP               46
#define AN_IS_ASSOC              47
#define AN_LAM_CHECKED           48
#define AN_LAM_INFER_RET         49
#define AN_NEEDS_HOME            50
#define AN_ALLOC_STATE           51
#define AN_MAY_PRINT_STATE       52
#define AN_FRESH_COUNT           53
#define AN_ARENA_SITES           54
#define AN_N_PARAM_SYMS          55
#define AN_PARAM_SYMS            56
#define AN_EFF_STATE             66
#define AN_EFF_COMPLETE          67
#define AN_EFF_UNKNOWN           68
#define PLAN_IS_EXT_TARGET       57
#define PLAN_IS_PAR_WORKER       58
#define AN_ADDR_MASK             59
#define AN_CONT_MASK             60
#define AN_OTHER_MASK            61
#define AN_HOME_ADDR_MASK        62
#define AN_HOME_CONT_MASK        63
#define PLAN_PAR_TLS_ARENA       62
#define PLAN_DEREF               63
#define PLAN_BOXED_CORO          64
#define PLAN_CORO_KIND           65
#define PLAN_DYN_TABLE           72
#define PLAN_CORO_RET_PROTO      73
#define PLAN_OW_SITES            75
#define PLAN_OW_LOCAL            76
#define PLAN_VAR_INIT_OO         77
#define AN_DUMMY_PAST_64         65
#define AN_ADDR_FROM_LOCAL       64
/* The analysis half of the slot, by name: the checker's own working state, which no later
 * phase reads. Named here so the two audiences cannot quietly merge -- a field that code
 * generation starts reading must move out of this list (and get a `planXxx` accessor). */
const char *const planAnalysisFields[] = { "lamSig", "makesPoolAny",
                                           "refDepth", "homeDepth", "lexicalLevel", "storedAt",
                                           "minAt", "reuse", "borrowed", "qualified" };
const size_t planAnalysisFieldCount = sizeof planAnalysisFields / sizeof planAnalysisFields[0];

/* ---- setters (called by the checker) ----------------------------------------------- */

/**
 * @brief 记录一个分配站点要用的 arena 层级。
 *
 * @param e  分配站点表达式（`new` 或可能分配的调用）；为 NULL 时忽略本次写入。
 * @param v  arena 层级：`ARENA_HOME` 表示家 arena，k >= 1 表示第 k 层块的 arena。
 */
void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaLevel = v; resultsSetBit(s, PLAN_ARENA_LEVEL);
}
/**
 * @brief 记录一个池站点要用的 zone 层级。
 *
 * 只往下调：同一个节点可能被检查不止一次，而层级越小越长寿，所以取更小的那个。
 *
 * @param e  池站点表达式；为 NULL 时忽略本次写入。
 * @param v  zone 层级：`ZONE_HOME` 表示用调用者收到的家 zone，k >= 1 表示第 k 层块的 zone。
 */
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->zoneLevel = v; resultsSetBit(s, PLAN_ZONE_LEVEL);
}
/**
 * @brief 记录一个调用往下传的 arena 参数。
 *
 * @param e  调用点表达式；为 NULL 时忽略本次写入。
 * @param v  arena 参数：`ARENA_HOME` 表示调用者自己的家 arena，k >= 1 表示第 k 层块的 arena。
 */
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaArg = v; resultsSetBit(s, PLAN_ARENA_ARG);
}
/**
 * @brief 记录一个表达式需要临时变量。
 *
 * @param e  表达式；为 NULL 时忽略本次写入。
 * @param v  需要临时变量则为 true。
 */
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->needTemp = v; resultsSetBit(s, PLAN_NEED_TEMP);
}

/**
 * @brief 记录一个函数是否接收调用者的家 arena 作隐藏参数。
 *
 * @param f  函数；为 NULL 时忽略本次写入。
 * @param v  需要隐藏的家 arena 参数则为 true。
 */
void planSetUsesHome(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->usesHome = v; resultsSetBit(s, PLAN_USES_HOME);
}
/**
 * @brief 记录一个函数体自己会不会分配存储。
 *
 * @param f  函数；为 NULL 时忽略本次写入。
 * @param v  可能分配则为 true。
 */
void planSetMayUseArena(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->mayUseArena = v; resultsSetBit(s, PLAN_MAY_USE_ARENA);
}
/**
 * @brief 记录一个函数会不会建池，直接建或经由被调者到达都算。
 *
 * @param f  函数；为 NULL 时忽略本次写入。
 * @param v  会建池则为 true。
 */
void planSetMakesPool(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->makesPool = v; resultsSetBit(s, PLAN_MAKES_POOL);
}
/**
 * @brief 记录一个循环的条件里会不会分配存储。
 *
 * @param st  循环语句；为 NULL 时忽略本次写入。
 * @param v   条件里分配则为 true。
 */
void planSetCondAllocs(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->condAllocs = v; resultsSetBit(s, PLAN_COND_ALLOCS);
}

/**
 * @brief 记录一个函数是不是协程。
 *
 * @param f  函数；为 NULL 时忽略本次写入。
 * @param v  是协程则为 true。
 */
void planSetIsCoro(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->isCoro = v; resultsSetBit(s, PLAN_IS_CORO);
}
/**
 * @brief 记录一个协程 `yield` 交出的值类型。
 *
 * @param f  协程函数；为 NULL 时忽略本次写入。
 * @param v  yield 值的类型。
 */
void planSetYieldType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->yieldType = v; resultsSetBit(s, PLAN_YIELD_TYPE);
}
/**
 * @brief 记录一个协程帧的 C 结构体类型。
 *
 * @param f  协程函数；为 NULL 时忽略本次写入。
 * @param v  帧结构体类型。
 */
void planSetCoroFrameType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroFrameType = v; resultsSetBit(s, PLAN_CORO_FRAME_TYPE);
}
/**
 * @brief 记录一个协程是否需要属于自己的地方（zone）。
 *
 * @param f  协程函数；为 NULL 时忽略本次写入。
 * @param v  需要自己的 zone 则为 true。
 */
void planSetCoroNeedsZone(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroNeedsZone = v; resultsSetBit(s, PLAN_CORO_NEEDS_ZONE);
}
/**
 * @brief 记录一个实例生成时使用的 C 名。
 *
 * 名字由模板名接上各类型实参的改写串拼成，实例化时写一次，代码生成发定义和调用点都用它。
 * 只保存指针，字符串要在后续读取之前一直有效。
 *
 * @param f     实例函数定义；为 NULL 时忽略本次写入。
 * @param name  C 名。
 */
void planSetInstName(FuncDef *f, const char *name) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->instName = name; resultsSetBit(s, PLAN_INST_NAME);
}

/**
 * @brief 记录一个实例由哪个模板函数实例化而来。
 *
 * 模板指针存在 plan 侧表、不放在节点字段上：同一个指针决定实例的名字、类型实参与共享哪具
 * 函数体。实例化在给实例做完浅拷贝之后写下它，因为浅拷贝带不上这个指针。
 *
 * @param f     实例函数定义；为 NULL 时忽略本次写入。
 * @param tmpl  实例化所用的模板函数定义。
 */
void planSetTemplate(FuncDef *f, FuncDef *tmpl) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->tmpl = tmpl; resultsSetBit(s, PLAN_TEMPLATE);
}

/**
 * @brief 记录一个合成协议方法的协议编号。
 *
 * @param f  合成的协议方法；为 NULL 时忽略本次写入。
 * @param v  协议编号：帧上的 next/value/send 为 1/2/5，句柄上的为 3/4/6。
 */
void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroProto = v; resultsSetBit(s, PLAN_CORO_PROTO);
}

/**
 * @brief 记录一个协程有没有被装箱成句柄。
 *
 * 帧值被强制转换成 `coroutine<T>` 句柄的地方写下这一位；记下之后这个协程总会得到任务，
 * 即使没有被显式 spawn，因而算作需要 zone。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param v  曾被装箱成句柄则为 true。
 */
void planSetCoroBoxed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroBoxed = v; resultsSetBit(s, PLAN_CORO_BOXED);
}

/* ---- accessors (signatures are the public surface) --------------------------------- */

/**
 * @brief 取一个实例是由哪个模板生成的。
 *
 * @param f  目标函数。
 * @return   模板函数；`f` 为 NULL 或不是实例时为 NULL。
 */
FuncDef *planTemplate(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_TEMPLATE) ? s->tmpl : NULL;
}
/**
 * @brief 取一个实例的 C 名字。
 *
 * @param f  目标函数。
 * @return   C 名字；没有记录时为 NULL。
 */
const char *planInstName(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_INST_NAME) ? s->instName : NULL;
}

/* ---- the resolved callee, per enclosing instance -----------------------------------
 *
 * A call site inside a generic body is **one node shared by every instance** of that
 * body: `wrap<T>` with two type arguments has one `pick(x)` node, and the instance that
 * callee resolves to depends on which `wrap` is being emitted. Keeping a single pointer
 * on the node means the last resolution wins: with `wrap<i32>` and
 * `wrap<meter>`, both emitted functions call the same `pick_*`, the other instance is
 * never emitted at all, and the generated C does not compile.
 *
 * So the deferred-call fixpoint records one entry per (call site, enclosing instance),
 * and code generation asks for the entry of the body it is emitting: `planEnterFunc` for
 * a function instance, `planEnterInst` for the type instance whose methods are being
 * emitted. A call site that belongs to no instance keeps the single pointer on the node,
 * which is the answer everywhere else. */
typedef struct {
    const Expr    *node;      /* the call site */
    const FuncDef *enclosing; /* the function instance whose body it was resolved in */
    const Type    *inst;      /* or the type instance, for a method body of a generic struct */
    FuncDef       *callee;    /* what the call resolves to inside that body */
} AltCallee;

static AltCallee *g_alt;
static size_t     g_altLen, g_altCap;
static const FuncDef *g_emitFunc;   /* the function instance currently being emitted */
static const Type    *g_emitInst;   /* the type instance whose methods are being emitted */

/**
 * @brief 判断一条按包含体记录的候选是否属于当前正在生成的那具函数体或类型实例。
 *
 * @param a  一条候选记录。
 * @return   包含体与当前发射上下文相符则返回 true；没有发射上下文时为 false。
 */
static bool altMatches(const AltCallee *a) {
    if (g_emitFunc && a->enclosing == g_emitFunc) return true;
    if (g_emitInst && a->inst == g_emitInst) return true;
    return false;
}

/**
 * @brief 取一个调用点当前解析到的被调函数。
 *
 * 泛型体里的一个调用点会被该体的每个实例共享；代码生成用 `planEnterFunc` / `planEnterInst`
 * 框出正在生成的那具函数体或类型实例，本函数优先返回为该体记录的那一份解析结果。
 * 不属于任何实例的调用点返回记录在节点上的那一份。
 *
 * @param e  调用点（或函数值）表达式。
 * @return   解析到的函数定义；内建、运算符、尚未解析以及没有记录时返回 NULL。
 */
FuncDef *planCallee(const Expr *e) {
    if (!e) return NULL;
    if (g_emitFunc || g_emitInst) {
        for (size_t i = g_altLen; i > 0; i--) {
            const AltCallee *a = &g_alt[i - 1];
            if (a->node == e && altMatches(a)) return a->callee;
        }
    }
    NodeResults *r = resultsOf(e, false);
    return (r && resultsBit(r, PLAN_CALLEE)) ? r->func : NULL;
}

/**
 * @brief 记录一个调用点解析出的被调函数，作为默认答案。
 *
 * 泛型体里的调用点被该体的所有实例共享，各实例的答案由 `planSetAltCallee` 按体记录；本函数
 * 写的是站点不属于任何实例时读到的那一份，没有发射上下文时 `planCallee` 返回它。
 *
 * @param e       调用点（或函数值）表达式；为 NULL 时忽略本次写入。
 * @param callee  解析到的函数定义；没有解析结果时可以为 NULL。
 */
void planSetCallee(Expr *e, FuncDef *callee) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (r) {
        r->func = callee;
        resultsSetBit(r, PLAN_CALLEE);
    }
}

/**
 * @brief 取一个绑定或变量语句生成时使用的 C 名。
 *
 * 参数不在这里：参数按值存储（协程帧会拷贝它们），按节点指针索引的槽跟不上拷贝，所以参数
 * 自带字段。本函数不接受参数节点。
 *
 * @param node  绑定表达式或变量声明语句节点。
 * @return      记录下来的 C 名；没有记录或记录为空串时返回 NULL，调用方应回落到声明自己的名字。
 */
const char *planCName(const void *node) {
    NodeResults *r = resultsOf(node, false);
    if (!r || !resultsBit(r, PLAN_CNAME)) return NULL;
    /* An empty name means "no name of its own": callers fall back to the declaration's name
     * (`planCName(x) ? planCName(x) : x->name`), and the checker does write "" for a binding
     * it cannot name. Returning "" would win that fallback and emit C with no identifier. */
    return r->cname && r->cname[0] ? r->cname : NULL;
}

/**
 * @brief 记录一个绑定或变量语句生成时使用的 C 名。
 *
 * @param node  绑定表达式或变量声明语句节点；为 NULL 时忽略本次写入。
 * @param name  C 名。
 * @param kind  该节点的种类（`RKIND_EXPR` 或 `RKIND_STMT`），用于结果槽的类型检查。
 */
void planSetCName(void *node, const char *name, ResultKind kind) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, kind, __LINE__);
    if (!r) return;
    r->cname = name;
    resultsSetBit(r, PLAN_CNAME);
}

/* ---- struct-definition facts ---- */

/**
 * @brief 判断这个结构体定义是不是某个内建类型的合成占位结构体。
 *
 * 这种占位结构体没有字段也没有布局，生成 C 时不为它声明结构体；它的方法照常按普通函数发出。
 *
 * @param sd  结构体定义。
 * @return    是内建占位结构体则返回 true；否则返回 false。
 */
bool planBuiltinHolder(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, PLAN_BUILTIN_HOLDER) ? r->builtinHolder : false;
}
/**
 * @brief 记录一个结构体定义是不是内建类型的合成占位结构体。
 *
 * @param sd  结构体定义；为 NULL 时忽略本次写入。
 * @param v   是否为内建占位结构体。
 */
void planSetBuiltinHolder(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->builtinHolder = v; resultsSetBit(r, PLAN_BUILTIN_HOLDER);
}

/**
 * @brief 取一个协程帧结构体所属的协程函数。
 *
 * 帧结构体由检查器为每个协程合成；生成 C 时用这里记下的函数名把帧类型与 `$step` / `$next`
 * 函数拼在一起。
 *
 * @param sd  结构体定义（合成的帧结构体）。
 * @return    所属的协程函数；该结构体不是协程帧时返回 NULL。
 */
FuncDef *planCoroOf(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, PLAN_CORO_OF)) ? r->coroOf : NULL;
}
/**
 * @brief 记录一个协程帧结构体所属的协程函数。
 *
 * @param sd  合成的帧结构体；为 NULL 时忽略本次写入。
 * @param f   所属的协程函数。
 */
void planSetCoroOf(StructDef *sd, FuncDef *f) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->coroOf = f; resultsSetBit(r, PLAN_CORO_OF);
}

/**
 * @brief 取 lambda 环境结构体上记下的用户所写签名。
 *
 * 环境结构体是编译器合成的；诊断里显示这个字符串，于是用户看到的是自己写的
 * `fn(i64) -> i64`，而不是生成的类型名。
 *
 * @param sd  结构体定义（lambda 的环境结构体）。
 * @return    记下的签名字符串；不是 lambda 环境结构体或没有记录时返回 NULL。
 */
const char *anLamSig(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, AN_LAM_SIG)) ? r->an.lamSig : NULL;
}
/**
 * @brief 记下 lambda 环境结构体上用户所写的签名。
 *
 * @param sd    lambda 的环境结构体；为 NULL 时忽略本次写入。
 * @param sig   用户写的签名字符串。
 */
void anSetLamSig(StructDef *sd, const char *sig) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.lamSig = sig; resultsSetBit(r, AN_LAM_SIG);
}

/**
 * @brief 判断这个结构体是否有方法会建池。
 *
 * 这是结构体一级的汇总：某个方法建池，则它的关联函数（构造器一族）也可能建池，
 * 池层级与隐藏 zone 参数的判断要算上这一层。
 *
 * @param sd  结构体定义。
 * @return    有方法建池则返回 true；没有记录时返回 false。
 */
bool anMakesPoolAny(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, AN_MAKES_POOL_ANY) ? r->an.makesPoolAny : false;
}
/**
 * @brief 记录这个结构体是否有方法会建池。
 *
 * @param sd  结构体定义；为 NULL 时忽略本次写入。
 * @param v   是否有方法建池。
 */
void anSetMakesPoolAny(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.makesPoolAny = v; resultsSetBit(r, AN_MAKES_POOL_ANY);
}

/* ---- the effect closure's state -----------------------------------------------------
 *
 * `effState` is work state; `effComplete`/`effUnknown` are results. None of the three is
 * read by code generation, so all three sit on the analysis side. */
/**
 * @brief 判断一个函数的效应摘要是否可信。
 *
 * 闭包合并被调者摘要时，只要有一个被调者算不完（在环上、或调用了没解析出来的东西），
 * 就把这里置为假；调用点据此走最保守的一档检查。
 *
 * @param f  函数定义。
 * @return   摘要可信则返回 true；没有记录时返回 false。
 */
bool anEffComplete(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_COMPLETE) ? r->an.effComplete : false;
}
/**
 * @brief 记录一个函数的效应摘要是否可信。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  摘要是否可信。
 */
void anSetEffComplete(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effComplete = v; resultsSetBit(r, AN_EFF_COMPLETE);
}
/**
 * @brief 判断一个函数的效应摘要里是否有没能解析的调用。
 *
 * 有的话摘要永远不算完成，调用点按最保守的一档处理。
 *
 * @param f  函数定义。
 * @return   有未解析的调用则返回 true；没有记录时返回 false。
 */
bool anEffUnknown(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_UNKNOWN) ? r->an.effUnknown : false;
}
/**
 * @brief 记录一个函数的效应摘要里是否有没能解析的调用。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否有未解析的调用。
 */
void anSetEffUnknown(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effUnknown = v; resultsSetBit(r, AN_EFF_UNKNOWN);
}
/**
 * @brief 取效应闭包的计算状态。
 *
 * 取值是 `EFF_NONE`（还没算）、`EFF_DONE`（算完了，可不可信看 `anEffComplete`）或
 * `EFF_IN_PROGRESS`（正在算——递归时再遇到就是环，摘要按不完整处理）。
 * 只有效应闭包自己读写这个值。
 *
 * @param f  函数定义。
 * @return   状态值；没有记录时返回 `EFF_NONE`。
 */
int anEffState(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_STATE) ? r->an.effState : 0;
}
/**
 * @brief 记录效应闭包的计算状态。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  状态值：`EFF_NONE`、`EFF_DONE` 或 `EFF_IN_PROGRESS`。
 */
void anSetEffState(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effState = v; resultsSetBit(r, AN_EFF_STATE);
}

/* ---- effect summary ----------------------------------------------------------------
 *
 * Pure results (the checker writes them, only the checker reads them): the transitively
 * closed masks and `addrFromLocal`. The **state** that drives the closure
 * (`effState`/`effComplete`/`effUnknown`) and the call graph it walks (`callees`) are
 * pass state, not a per-node result. */
#define AEF_GET(fn, type, field, bit, dflt)                                \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AEF_SET(fn, type, field, bit)                                      \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }

AEF_GET(anAddrMask, uint64_t, addrMask, AN_ADDR_MASK, 0)
AEF_SET(anSetAddrMask, uint64_t, addrMask, AN_ADDR_MASK)
AEF_GET(anContMask, uint64_t, contMask, AN_CONT_MASK, 0)
AEF_SET(anSetContMask, uint64_t, contMask, AN_CONT_MASK)
AEF_GET(anOtherMask, uint64_t, otherMask, AN_OTHER_MASK, 0)
AEF_SET(anSetOtherMask, uint64_t, otherMask, AN_OTHER_MASK)
AEF_GET(anHomeAddrMask, uint64_t, homeAddrMask, AN_HOME_ADDR_MASK, 0)
AEF_SET(anSetHomeAddrMask, uint64_t, homeAddrMask, AN_HOME_ADDR_MASK)
AEF_GET(anHomeContMask, uint64_t, homeContMask, AN_HOME_CONT_MASK, 0)
AEF_SET(anSetHomeContMask, uint64_t, homeContMask, AN_HOME_CONT_MASK)
AEF_GET(anAddrFromLocal, bool, addrFromLocal, AN_ADDR_FROM_LOCAL, false)
AEF_SET(anSetAddrFromLocal, bool, addrFromLocal, AN_ADDR_FROM_LOCAL)

/* ---- per-function analysis facts ---------------------------------------------------
 *
 * **Instances**: `funcInstance` builds an instance with `*in = *tmpl` -- a shallow copy.
 * Plain fields travel with that copy; a slot keyed by node pointer does not. So every fact
 * here is inherited explicitly in one place (`planInheritFuncFacts`, right after the copy). */
#define ANF_GET(fn, type, side, field, bit, dflt)                          \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->side . field : (dflt);     \
    }
#define ANF_SET(fn, type, side, field, bit)                                \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->side . field = v; resultsSetBit(r, bit);                          \
    }

ANF_GET(anIsAssoc, bool, an, isAssoc, AN_IS_ASSOC, false)
ANF_SET(anSetIsAssoc, bool, an, isAssoc, AN_IS_ASSOC)
ANF_GET(anLamChecked, bool, an, lamChecked, AN_LAM_CHECKED, false)
ANF_SET(anSetLamChecked, bool, an, lamChecked, AN_LAM_CHECKED)
ANF_GET(anLamInferRet, bool, an, lamInferRet, AN_LAM_INFER_RET, false)
ANF_SET(anSetLamInferRet, bool, an, lamInferRet, AN_LAM_INFER_RET)
ANF_GET(anNeedsHome, bool, an, needsHome, AN_NEEDS_HOME, false)
ANF_SET(anSetNeedsHome, bool, an, needsHome, AN_NEEDS_HOME)
ANF_GET(anAllocState, int, an, allocState, AN_ALLOC_STATE, 0)
ANF_SET(anSetAllocState, int, an, allocState, AN_ALLOC_STATE)
ANF_GET(anMayPrintState, int, an, mayPrintState, AN_MAY_PRINT_STATE, 0)
ANF_SET(anSetMayPrintState, int, an, mayPrintState, AN_MAY_PRINT_STATE)
ANF_GET(anFreshCount, int, an, freshCount, AN_FRESH_COUNT, 0)
ANF_SET(anSetFreshCount, int, an, freshCount, AN_FRESH_COUNT)
ANF_GET(anParamSymCount, int, an, nParamSyms, AN_N_PARAM_SYMS, 0)
ANF_SET(anSetParamSymCount, int, an, nParamSyms, AN_N_PARAM_SYMS)
/**
 * @brief 取一个函数记录下来的分配站点列表。
 *
 * 列表里是函数体里每个可能分配的表达式：`new`，以及可能分配存储的调用。分配放置阶段按它逐
 * 站点决定 arena 层级；这是检查器自己的工作状态，后续阶段不读。泛型实例不单独收集，直接指向
 * 其模板的那一份列表。
 *
 * @param f  函数定义。
 * @return   分配站点向量；没有记录或 `f` 为 NULL 时返回空向量。
 */
Vec anArenaSites(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return (r && resultsBit(r, AN_ARENA_SITES)) ? r->an.arenaSites : (Vec){0};
}
/**
 * @brief 记下一个函数的 arena 待决站点清单。
 *
 * 清单里既有分配点表达式，也有 arena 实参还没定的调用点，交给分析收尾之后的 pass 逐站决定
 * 层级。实例化出来的函数从它的模板抄走同一份清单。
 *
 * @param f  目标函数。
 * @param v  站点向量，元素为 `Expr *`。
 */
void anSetArenaSites(FuncDef *f, Vec v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.arenaSites = v; resultsSetBit(r, AN_ARENA_SITES);
}
/**
 * @brief 取一个函数存放形参符号的可写数组。
 *
 * 数组容量固定，前 `anParamSymCount` 个元素有效；调用会顺带建好结果槽。
 *
 * @param f  目标函数。
 * @return   数组首地址；`f` 为 NULL 或建槽失败时为 NULL。
 */
void **anParamSyms(FuncDef *f) {
    if (!f) return NULL;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return NULL;
    resultsSetBit(r, AN_PARAM_SYMS);
    return r->an.paramSyms;
}

/**
 * @brief 把模板函数的每函数分析事实搬到新物化的实例上。
 *
 * 实例是模板的浅拷贝（`*in = *tmpl`），而结果槽按节点指针索引，不会被这次拷贝带过来；
 * 所以拷贝之后必须立刻调用本函数，把模板的分析块与已写位掩码并到实例的槽里。
 *
 * @param in    通过浅拷贝新建的实例。
 * @param tmpl  模板函数。
 */
void planInheritFuncFacts(FuncDef *in, const FuncDef *tmpl) {
    if (!in || !tmpl) return;
    NodeResults *t = resultsOf(tmpl, false);
    if (!t) return;
    NodeResults *r = resultsAs(in, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    /* The `an` block is copied wholesale (it is one struct), so its bits come along too --
     * which is exactly what "a slot keyed by node pointer does not travel with the shallow
     * copy, so inherit it here" means in practice. */
    uint64_t lo = t->loMask, hi = t->hiMask;
    r->an = t->an;
    r->loMask |= lo;
    r->hiMask |= hi;
}

/* The three that are read outside the checker (the ABI and the parallel runtime) live in
 * the plan half of the slot, so they need their own accessors -- the `an.` macros cannot
 * reach them. */
#define PLF_GET(fn, type, field, bit, dflt)                                \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->field : (dflt);            \
    }
#define PLF_SET(fn, type, field, bit)                                      \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->field = v; resultsSetBit(r, bit);                                 \
    }

/* The write-once pass's two facts. (The call graph itself stays a mutable vector on the
 * AST: `vecPush`'s contract forbids holding a `Vec *` across a push, and an accessor
 * handing one back invites exactly that.) */
/**
 * @brief 记录这个函数体里 `@overwrite` 复用站点的个数。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  站点个数。
 */
void planSetOwSites(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.owSites = v; resultsSetBit(r, PLAN_OW_SITES);
}
/**
 * @brief 记录 `@overwrite` 复用站点的存储是否放在本函数自己的帧里。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否放在本函数帧里。
 */
void planSetOwLocal(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.owLocal = v; resultsSetBit(r, PLAN_OW_LOCAL);
}
/**
 * @brief 取这个函数体里 `@overwrite` 复用站点的个数。
 *
 * 生成 C 时用这个计数决定要不要为复用站点准备隐藏的存储并传递它。只有检查器写它。
 *
 * @param f  函数定义。
 * @return   站点个数；没有记录时返回 0。
 */
int planOwSites(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_OW_SITES) ? r->an.owSites : 0;
}
/**
 * @brief 判断 `@overwrite` 复用站点的存储是否放在本函数自己的帧里。
 *
 * 为真表示每次调用各有自己的一块存储（`main`，或者会递归到自己的函数）；
 * 为假表示存储由调用点的帧持有，并作为不透明的隐藏参数传进来。
 *
 * @param f  函数定义。
 * @return   存储在本函数帧里则返回 true；没有记录时返回 false。
 */
bool planOwLocal(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_OW_LOCAL) ? r->an.owLocal : false;
}
/**
 * @brief 判断这个函数的函数体是否被 `dyn` 虚表 thunk 从外部点名。
 *
 * 这种引用在「没有任何名字提到就不发出」的裁剪里看不见，因此要把它记下来，函数体必须保留。
 *
 * @param f  函数定义。
 * @return   被 `dyn` 虚表点名则返回 true；没有记录时返回 false。
 */
bool planDynTable(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_DYN_TABLE) ? r->dynTable : false;
}
/**
 * @brief 记录这个函数的函数体是否被 `dyn` 虚表 thunk 从外部点名。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否被 `dyn` 虚表点名。
 */
void planSetDynTable(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->dynTable = v; resultsSetBit(r, PLAN_DYN_TABLE);
}
/**
 * @brief 取协程声明的返回类型（尚未代入类型实参的那一份）。
 *
 * 物化泛型实例时用它按类型实参替换出实例真正的返回类型。
 *
 * @param f  协程函数定义（模板）。
 * @return   声明的返回类型；不是协程或没有记录时返回 NULL。
 */
Type *planCoroRetProto(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_CORO_RET_PROTO) ? r->coroRetProto : NULL;
}
/**
 * @brief 记录协程声明的返回类型（尚未代入类型实参的那一份）。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param t  声明的返回类型；不是协程时传 NULL。
 */
void planSetCoroRetProto(FuncDef *f, Type *t) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->coroRetProto = t; resultsSetBit(r, PLAN_CORO_RET_PROTO);
}
/**
 * @brief 取代码生成前导阶段分配给这个协程的编号。
 *
 * 句柄里的 kind 字段与调度域的任务表都用这个编号来分派到对应的 `$step` / `$next`。
 *
 * @param f  协程函数定义。
 * @return   编号；没有记录时返回 0。
 */
int planCoroKind(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_CORO_KIND) ? r->coroKind : 0;
}
/**
 * @brief 记录代码生成前导阶段分配给这个协程的编号。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param v  编号。
 */
void planSetCoroKind(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->coroKind = v; resultsSetBit(r, PLAN_CORO_KIND);
}
/**
 * @brief 判断这个表达式在生成时是否要套一层解引用。
 *
 * 值的类型是 `ref T` 而它所在的位置需要一个 `T`，生成 C 时就写成 `*(...)`。
 *
 * @param e  表达式。
 * @return   需要解引用则返回 true；没有记录时返回 false。
 */
bool planDeref(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DEREF) ? r->deref : false;
}
/**
 * @brief 记录这个表达式在生成时是否需要套一层解引用。
 *
 * @param e  表达式；为 NULL 时忽略本次写入。
 * @param v  是否需要解引用。
 */
void planSetDeref(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->deref = v;
    resultsSetBit(r, PLAN_DEREF);
}
/**
 * @brief 判断这个协程调用是否要装箱成句柄来存储。
 *
 * 为真表示协程的值要存到活过当前 C 作用域的地方：生成 C 时把帧放进任务自己的 place，
 * 并把值做成 `{frame, kind, task}` 句柄。
 *
 * @param e  协程调用表达式。
 * @return   需要装箱则返回 true；没有记录时返回 false。
 */
bool planBoxedCoro(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_BOXED_CORO) ? r->boxedCoro : false;
}
/**
 * @brief 记录这个协程调用是否要装箱成句柄来存储。
 *
 * @param e  协程调用表达式；为 NULL 时忽略本次写入。
 * @param v  是否需要装箱。
 */
void planSetBoxedCoro(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->boxedCoro = v;
    resultsSetBit(r, PLAN_BOXED_CORO);
}
PLF_GET(planIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET, false)
PLF_SET(planSetIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET)
PLF_GET(planIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER, false)
PLF_SET(planSetIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER)
PLF_GET(planParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA, false)
PLF_SET(planSetParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA)

/* ---- operator / comparison facts -----------------------------------------------------
 *
 * `needOp` and `convCheck` are read by code generation (not to make a decision, but to know
 * that a deferred operator still has to be re-resolved, and that a narrowing conversion gets
 * a runtime check); the rest are the checker's own state. */
#define AN_I_GET(fn, field, bit, dflt)                                     \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AN_I_SET(fn, field, bit)                                           \
    void fn(void *node, int v) {                                           \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }
#define AN_B_GET(fn, field, bit, dflt)                                     \
    bool fn(const void *node) {                                            \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AN_B_SET(fn, field, bit)                                           \
    void fn(void *node, bool v) {                                          \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }
/**
 * @brief 判断一次显式转换要不要在运行期做范围检查。
 *
 * 整数加宽、浮点到浮点，以及引用转 64 位整数都不需要检查；其余收窄、变号与浮点转整数
 * 都要。
 *
 * @param e  转换表达式。
 * @return   要生成运行期检查则 `true`；没有记录时为 `false`。
 */
bool planConvCheck(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_CONV_CHECK) ? r->an.convCheck : false;
}
/**
 * @brief 记下一次显式转换要不要在运行期做范围检查。
 *
 * @param e  转换表达式。
 * @param v  要检查为 `true`。
 */
void planSetConvCheck(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.convCheck = v; resultsSetBit(r, AN_CONV_CHECK);
}
/**
 * @brief 判断一次二元运算是不是因为操作数是类型参数而推迟到实例化再解析。
 *
 * @param e  二元表达式。
 * @return   需要重新解析则 `true`；没有记录时为 `false`。
 */
bool planNeedOp(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_NEED_OP) ? r->an.needOp : false;
}
/**
 * @brief 记下一次二元运算需要推迟到实例化再解析。
 *
 * @param e  二元表达式。
 * @param v  需要重新解析为 `true`。
 */
void planSetNeedOp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.needOp = v; resultsSetBit(r, AN_NEED_OP);
}

AN_I_GET(anMinAt, minAt, AN_MIN_AT, 0)
AN_I_SET(anSetMinAt, minAt, AN_MIN_AT)
AN_B_GET(anReuse, reuse, AN_REUSE, false)
AN_B_SET(anSetReuse, reuse, AN_REUSE)
AN_B_GET(anBorrowed, borrowed, AN_BORROWED, false)
AN_B_SET(anSetBorrowed, borrowed, AN_BORROWED)
AN_B_GET(anQualified, qualified, AN_QUALIFIED, false)
AN_B_SET(anSetQualified, qualified, AN_QUALIFIED)

/* ---- call / receiver facts -----------------------------------------------------------
 *
 * All six are read by code generation (that is why they are here and not in `an`): where a
 * task is handed (`extDom`), which field an `EX_FIELD` resolved to, the instance type an
 * `EX_ASSOC` resolved to, and how a `dyn` call reaches its receiver. */

#define PLAN_GET(fn, type, field, bit, dflt)                               \
    type fn(const Expr *e) {                                               \
        NodeResults *r = resultsOf(e, false);                              \
        return (r && resultsBit(r, bit)) ? r->field : (dflt);            \
    }
#define PLAN_SET(fn, type, field, bit)                                     \
    void fn(Expr *e, type v) {                                             \
        if (!e) return;                                                    \
        NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);         \
        if (!r) return;                                                    \
        r->field = v; resultsSetBit(r, bit);                                 \
    }

PLAN_GET(planExtDom, Expr *, extDom, PLAN_EXT_DOM, NULL)
PLAN_SET(planSetExtDom, Expr *, extDom, PLAN_EXT_DOM)
PLAN_GET(planField, FieldDef *, field, PLAN_FIELD, NULL)
PLAN_SET(planSetField, FieldDef *, field, PLAN_FIELD)
PLAN_GET(planAssocOwner, Type *, assocOwner, PLAN_ASSOC_OWNER, NULL)
PLAN_SET(planSetAssocOwner, Type *, assocOwner, PLAN_ASSOC_OWNER)
PLAN_GET(planDynRecvViaRef, bool, dynRecvViaRef, PLAN_DYN_RECV_VIA_REF, false)
PLAN_SET(planSetDynRecvViaRef, bool, dynRecvViaRef, PLAN_DYN_RECV_VIA_REF)
PLAN_GET(planCallViaFn, bool, callViaFn, PLAN_CALL_VIA_FN, false)
PLAN_SET(planSetCallViaFn, bool, callViaFn, PLAN_CALL_VIA_FN)

/* `storedAt` is the one field of this group whose **unset value is not the type's zero**:
 * "has this value been seen being published?" is answered `-1` (not yet) or a level `>= 0`
 * (the level it was published at), and `0` is a real answer. Seeding it from `exprNew`
 * would make the AST constructor reach into the plan's storage -- a dependency pointing up
 * from the AST layer into the plan layer. The default belongs to the reader, not to the
 * constructor: a node nothing has published is "not yet published", always. */
#define AN_STORED_AT_GET(fn)                                               \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, AN_STORED_AT)) ? r->an.storedAt : -1;   \
    }
AN_STORED_AT_GET(anStoredAt)
/**
 * @brief 记下一个值被发布到的层级。
 *
 * `-1` 表示还没被发布过，`>= 0` 是发布所在的层级；`0` 是一个真实的层级，所以未设置的默认值
 * 只能由读者给出。
 *
 * @param node  目标值节点；为 NULL 时忽略本次写入。
 * @param v     发布层级：`-1`（还没发布）或 `>= 0`。
 */
void anSetStoredAt(void *node, int v) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.storedAt = v; resultsSetBit(r, AN_STORED_AT);
}

/* ---- depth and origin facts ----------------------------------------------------------
 *
 * `void *` on purpose: `refDepth` lives on an `Expr` **and** on the checker's `Sym`, and one
 * accessor answers both. */
#define AN_D_GET(fn, field, bit, dflt)                                     \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
/* One kind for the whole group: `refDepth` is stamped on an `Expr` **and** on the checker's
 * `Sym`, and a slot has one kind. Both are pointer-stable, so nothing else collides --
 * `RKIND_EXPR` is the honest label (`Sym` is not an AST node, but it is not a `Stmt` either,
 * and a slot is written as one kind for its whole life). */
#define AN_D_SET(fn, field, bit)                                           \
    void fn(void *node, int v) {                                           \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }
AN_D_GET(anRefDepth, refDepth, AN_REF_DEPTH, 0)
AN_D_SET(anSetRefDepth, refDepth, AN_REF_DEPTH)
AN_D_GET(anHomeDepth, homeDepth, AN_HOME_DEPTH, 0)
AN_D_SET(anSetHomeDepth, homeDepth, AN_HOME_DEPTH)
AN_D_GET(anLexicalLevel, lexicalLevel, AN_LEXICAL_LEVEL, 0)
AN_D_SET(anSetLexicalLevel, lexicalLevel, AN_LEXICAL_LEVEL)

/* ---- call-site markers ---------------------------------------------------------------
 *
 * Three builtin forms have no `Expr.func` to recognise them by: an assoc builtin call and
 * a body-less declaration reach code generation with a NULL callee, so keying on the
 * callee can never fire. Each gets its own marker, written by the checker at the call
 * site and read by code generation. */

/**
 * @brief 取 `parallel::run(worker, ...)` 调用里的 worker 函数。
 *
 * @param e  调用节点。
 * @return   worker 函数；不是这种调用时为 NULL。
 */
FuncDef *planParWorker(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return (r && resultsBit(r, PLAN_PAR_WORKER)) ? r->parWorker : NULL;
}
/**
 * @brief 记下 `parallel::run(worker, ...)` 调用里的 worker 函数。
 *
 * @param e   调用节点；为 NULL 时忽略本次写入。
 * @param wf  worker 函数。
 */
void planSetParWorker(Expr *e, FuncDef *wf) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->parWorker = wf; resultsSetBit(r, PLAN_PAR_WORKER);
}

/**
 * @brief 判断这个调用是不是构造一个域的 builtin 形式。
 *
 * 这种形式没有可依赖的被调者指针，所以由调用节点上的标记识别。
 *
 * @param e  调用节点。
 * @return   是则 `true`；没有记录时为 `false`。
 */
bool planDomNew(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DOM_NEW) ? r->domNew : false;
}
/**
 * @brief 标记这个调用是构造一个域的 builtin 形式。
 *
 * @param e  调用节点；为 NULL 时忽略本次写入。
 * @param v  是则为 `true`。
 */
void planSetDomNew(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->domNew = v; resultsSetBit(r, PLAN_DOM_NEW);
}

/**
 * @brief 判断这个调用是不是 `extc_viewOf` 这个没有函数体的 builtin 形式。
 *
 * 声明没有函数体，没有可依赖的被调者指针，所以由调用节点上的标记识别。
 *
 * @param e  调用节点。
 * @return   是则 `true`；没有记录时为 `false`。
 */
bool planViewOf(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_VIEW_OF) ? r->viewOf : false;
}
/**
 * @brief 标记这个调用是 `extc_viewOf` 这个 builtin 形式。
 *
 * @param e  调用节点；为 NULL 时忽略本次写入。
 * @param v  是则为 `true`。
 */
void planSetViewOf(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->viewOf = v; resultsSetBit(r, PLAN_VIEW_OF);
}

/* ---- impl / trait / module facts ---------------------------------------------------- */

/**
 * @brief 取一个 `impl` 块实现的 trait。
 *
 * @param im  `impl` 块定义。
 * @return   实现的 trait；固有 `impl` 或尚未解析时为 NULL。
 */
TraitDef *planImplTrait(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TRAIT)) ? r->implTrait : NULL;
}
/**
 * @brief 记下一个 `impl` 块实现的 trait。
 *
 * @param im  `impl` 块定义；为 NULL 时忽略本次写入。
 * @param tr  实现的 trait。
 */
void planSetImplTrait(ImplDef *im, TraitDef *tr) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTrait = tr; resultsSetBit(r, PLAN_IMPL_TRAIT);
}

/**
 * @brief 取一个 `impl` 块解析到的具体类型。
 *
 * @param im  `impl` 块定义。
 * @return   目标类型；没有记录时为 NULL。
 */
Type *planImplTarget(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TARGET)) ? r->implTarget : NULL;
}
/**
 * @brief 记下一个 `impl` 块解析到的具体类型。
 *
 * @param im  `impl` 块定义；为 NULL 时忽略本次写入。
 * @param t   目标类型。
 */
void planSetImplTarget(ImplDef *im, Type *t) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTarget = t; resultsSetBit(r, PLAN_IMPL_TARGET);
}

/**
 * @brief 判断一个 trait 是否被 `dyn` 形式用到过。
 *
 * 只有用到过，它的虚表才会被生成。
 *
 * @param tr  目标 trait。
 * @return    用到过则 `true`；没有记录时为 `false`。
 */
bool planUsedDyn(const TraitDef *tr) {
    NodeResults *r = resultsOf(tr, false);
    return r && resultsBit(r, PLAN_USED_DYN) ? r->usedDyn : false;
}
/**
 * @brief 记下一个 trait 被 `dyn` 形式用到过。
 *
 * @param tr  目标 trait；为 NULL 时忽略本次写入。
 * @param v   用到过为 `true`。
 */
void planSetUsedDyn(TraitDef *tr, bool v) {
    if (!tr) return;
    NodeResults *r = resultsAs(tr, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usedDyn = v; resultsSetBit(r, PLAN_USED_DYN);
}

/**
 * @brief 判断一个模块里是否出现过 `dyn` 形式。
 *
 * 出现过才需要生成 `dyn` 运行期。
 *
 * @param m  目标模块。
 * @return   出现过则 `true`；没有记录时为 `false`。
 */
bool planUsesDyn(const Module *m) {
    NodeResults *r = resultsOf(m, false);
    return r && resultsBit(r, PLAN_USES_DYN) ? r->usesDyn : false;
}
/**
 * @brief 记下一个模块里出现过 `dyn` 形式。
 *
 * @param m  目标模块；为 NULL 时忽略本次写入。
 * @param v  出现过为 `true`。
 */
void planSetUsesDyn(Module *m, bool v) {
    if (!m) return;
    NodeResults *r = resultsAs(m, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usesDyn = v; resultsSetBit(r, PLAN_USES_DYN);
}

/**
 * @brief 判断一个函数是否被调用过。
 *
 * 代码生成只发出被调用过的函数，外加 `main`、`@export` 与 `dyn` 表项这些根。
 * 没有记录过调用时读作未使用，这正是安全方向：漏掉的调用点不会让产物里多出一具没人引用的
 * 函数体。
 *
 * @param f  函数定义。
 * @return   记录过调用则返回 true；没有记录时返回 false。
 */
bool planUsed(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_USED) ? r->used : false;
}

/**
 * @brief 记录一个函数是否被调用过。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否被调用。
 */
void planSetUsed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->used = v;
        resultsSetBit(r, PLAN_USED);
    }
}

/**
 * @brief 按包含它的那具函数体或类型实例，记录一个调用点解析出的被调函数。
 *
 * 泛型体里的一个调用点被该体的所有实例共享，各实例可以解析到不同的函数（`wrap<i32>` 调
 * `pick_i32`，`wrap<meter>` 调 `pick_meter`），所以答案按（调用点，包含体）成对记录，
 * `enclosing` 与 `inst` 至少要给一个。同一对重复写入只覆盖被调函数，因为延迟调用闭包会重放
 * 多轮。
 *
 * @param e          调用点表达式；为 NULL 时忽略本次写入。
 * @param enclosing  包含该调用点的函数实例；在方法体里解析时传 NULL。
 * @param inst       包含该调用点的类型实例；在自由函数体里解析时传 NULL。
 * @param callee     该包含体内解析到的函数定义。
 */
void planSetAltCallee(Expr *e, FuncDef *enclosing, const Type *inst, FuncDef *callee) {
    if (!e || (!enclosing && !inst)) return;
    for (size_t i = 0; i < g_altLen; i++) {
        AltCallee *a = &g_alt[i];
        if (a->node == e && a->enclosing == enclosing && a->inst == inst) {
            a->callee = callee;           /* idempotent: the fixpoint replays rounds */
            return;
        }
    }
    if (g_altLen == g_altCap) {
        g_altCap = g_altCap ? g_altCap * 2 : 64;
        AltCallee *na = realloc(g_alt, g_altCap * sizeof *na);
        if (!na) abort();
        g_alt = na;
    }
    g_alt[g_altLen].node = e;
    g_alt[g_altLen].enclosing = enclosing;
    g_alt[g_altLen].inst = inst;
    g_alt[g_altLen].callee = callee;
    g_altLen++;
}

/**
 * @brief 声明接下来生成的是哪个函数实例，让 `planCallee` 选中按体记录的解析结果。
 *
 * @param f  即将生成的函数实例。
 */
void planEnterFunc(const FuncDef *f) { g_emitFunc = f; }
/**
 * @brief 结束函数实例的生成上下文。
 */
void planLeaveFunc(void) { g_emitFunc = NULL; }
/**
 * @brief 声明接下来生成的是哪个类型实例的方法，让 `planCallee` 选中按体记录的解析结果。
 *
 * @param t  即将生成其方法的类型实例。
 */
void planEnterInst(const Type *t) { g_emitInst = t; }
/**
 * @brief 结束类型实例的生成上下文。
 */
void planLeaveInst(void) { g_emitInst = NULL; }

/**
 * @brief 取一个分配点该用的 arena 层级。
 *
 * `ARENA_HOME` 哨兵表示用调用者给的 home arena；数值越大寿命越短。
 *
 * @param e  分配点表达式。
 * @return   已记录的层级；没有记录时为 0。
 */
int planArenaLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ARENA_LEVEL)) ? s->arenaLevel : 0;
}
/**
 * @brief 取一个池站点记录下来的 zone 层级。
 *
 * `ZONE_HOME` 表示用调用者收到的家 zone（库函数对地方透明，把选择权交给调用者），
 * k >= 1 表示第 k 层块的 zone；站点没记录时返回 0。
 *
 * @param e  要查询的表达式（建池的调用点）。
 * @return   记录下来的 zone 层级；没记录时返回 0。
 */
int planZoneLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ZONE_LEVEL)) ? s->zoneLevel : 0;
}
/**
 * @brief 取一次调用往下传的 arena 实参。
 *
 * `-1` 表示调用者自己的 home arena，`>= 1` 表示那个块层级，`0` 表示当前块。
 *
 * @param e  调用节点。
 * @return   已记录的实参；没有记录时为 0。
 */
int planArenaArg(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ARENA_ARG)) ? s->arenaArg : 0;
}
/**
 * @brief 判断一个表达式是否要先把某部分算进临时变量。
 *
 * 该表达式（或其参数、元素计数）在生成的 C 里会出现不止一次，或需要在循环外只求值一次时
 * 记为 true，代码生成据此先发一条临时变量声明。
 *
 * @param e  要查询的表达式。
 * @return   需要临时变量则为 true；没记录时返回 false。
 */
bool planNeedTemp(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_NEED_TEMP)) ? s->needTemp : false;
}

/**
 * @brief 判断一个函数是否接收调用者的家 arena 作隐藏参数。
 *
 * 以「函数体真的读到了传入的家 arena，或把家 arena 往下交给被调者」为准；该值要等汇总闭包
 * 跑完才最终确定。
 *
 * @param f  要查询的函数。
 * @return   需要隐藏的家 arena 参数则为 true；没记录时返回 false。
 */
bool planUsesHome(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_USES_HOME) ? s->usesHome : false;
}
/**
 * @brief 判断一个函数体自己会不会分配存储。
 *
 * 为 false 时函数体不分配任何东西，代码生成就不为它声明 arena 数组、也不发释放调用。
 *
 * @param f  要查询的函数。
 * @return   可能分配则为 true；没记录时返回 false。
 */
bool planMayUseArena(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAY_USE_ARENA) ? s->mayUseArena : false;
}
/**
 * @brief 判断一个函数会不会建池，直接建或经由被调者到达都算。
 *
 * 该值要等调用图闭包跑完才最终确定。
 *
 * @param f  要查询的函数。
 * @return   会建池则为 true；没记录时返回 false。
 */
bool planMakesPool(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAKES_POOL) ? s->makesPool : false;
}
/**
 * @brief 判断一个循环的条件里会不会分配存储。
 *
 * 为 true 时代码生成给循环包一层每轮回收的 arena，让条件里的分配在轮末归还。
 *
 * @param st  要查询的循环语句。
 * @return    条件里分配则为 true；没记录时返回 false。
 */
bool planCondAllocs(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && resultsBit(s, PLAN_COND_ALLOCS) ? s->condAllocs : false;
}
/**
 * @brief 记录一个声明的初始化式是否指向帧外存储。
 *
 * 帧外存储包括全局、`global` 声明、`effects Ret=0` 的结果与字符串字面量；这条结果在作用域
 * 还活着时写下，供函数体检查完之后再问。
 *
 * @param st  变量声明语句。
 * @param v   初始化式指向帧外存储则为 true。
 */
void planSetVarInitOutOfFrame(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->varInitOO = v; resultsSetBit(s, PLAN_VAR_INIT_OO);
}
/**
 * @brief 读回一个声明的初始化式是否指向帧外存储。
 *
 * @param st  要查询的变量声明语句。
 * @return    初始化式指向帧外存储则为 true；没记录时返回 false。
 */
bool planVarInitOutOfFrame(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && resultsBit(s, PLAN_VAR_INIT_OO) ? s->varInitOO : false;
}
/**
 * @brief 取脱糖 `for` 的步进语句。
 *
 * 步进语句是语法，写在循环体上，因为 `continue` 必须能走到它；循环被改写成迭代器之后步进
 * 不再存在，这一处返回 NULL。
 *
 * @param s  要查询的循环体语句。
 * @return   步进语句；步进已被丢弃、`s` 为空或没有步进时返回 NULL。
 */
Stmt *planForStep(const Stmt *s) {
    if (!s) return NULL;
    NodeResults *r = resultsOf(s, false);
    if (r && resultsBit(r, PLAN_FOR_STEP_DROPPED) && r->forStepDropped) return NULL;
    return s->forStep;
}

/**
 * @brief 记录一个脱糖 `for` 的步进语句已不再存在。
 *
 * 循环被改写成迭代器之后由迭代器的 `next()` 推进，`continue` 不再需要跳到步进标签，该标签
 * 也就不再发出。
 *
 * @param s  承载步进语句的循环体语句。
 */
void planSetForStepDropped(Stmt *s) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->forStepDropped = true;
    resultsSetBit(r, PLAN_FOR_STEP_DROPPED);
}

/**
 * @brief 判断一个函数是不是协程。
 *
 * 协程的值是一个帧句柄，而不是一个普通 C 函数。
 *
 * @param f  要查询的函数。
 * @return   是协程则为 true；没记录时返回 false。
 */
bool planIsCoro(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_IS_CORO) ? s->isCoro : false;
}
/**
 * @brief 取协程 `yield` 交出的值类型。
 *
 * 对 `coroutine<A, B>` 取最后一个类型参数，对单参数的 `coroutine<T>` 取那一个参数。
 *
 * @param f  要查询的函数。
 * @return   yield 值的类型；不是协程或没记录时返回 NULL。
 */
Type *planYieldType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_YIELD_TYPE) ? s->yieldType : NULL;
}
/**
 * @brief 取协程帧的 C 结构体类型。
 *
 * 帧结构体的字段布局与代码生成发出来的字段一致，驱动协程的协议方法挂在它上面。
 *
 * @param f  要查询的函数。
 * @return   帧结构体类型；不是协程或还没建立帧时返回 NULL。
 */
Type *planCoroFrameType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_FRAME_TYPE) ? s->coroFrameType : NULL;
}
/**
 * @brief 判断一个协程是否需要属于自己的地方（zone）。
 *
 * 会建池、或帧曾被装箱成句柄的协程总要有一个任务，它的地方由任务持有；不需要时直接驱动
 * 步进函数。
 *
 * @param f  要查询的函数。
 * @return   需要自己的 zone 则为 true；没记录时返回 false。
 */
bool planCoroNeedsZone(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_NEEDS_ZONE) ? s->coroNeedsZone : false;
}
/**
 * @brief 取一个合成协议方法的协议编号。
 *
 * 协议方法没有函数体，代码生成在调用点内联展开，编号说明它是哪一个方法：帧上的
 * next/value/send 记为 1/2/5，句柄上的记为 3/4/6。
 *
 * @param f  要查询的函数。
 * @return   协议编号；普通函数返回 0。
 */
int planCoroProto(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_PROTO) ? s->coroProto : 0;
}
/**
 * @brief 判断一个协程的帧是否在某个赋值处被转换成过句柄。
 *
 * 装箱后的协程总会得到一个任务，所以它算作需要自己的 zone；这条事实在发生转换的地方写下。
 *
 * @param f  要查询的函数。
 * @return   帧曾被装箱成句柄则为 true；没记录时返回 false。
 */
bool planCoroBoxed(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_BOXED) ? s->coroBoxed : false;
}
