/**
 * @file check_internal.h
 * @brief 检查器的内部头文件：`check_*.c` 之间共享的状态，以及跨文件的原型。
 *
 * 唯一对外的入口是 `check.h` 里的 `checkModule`。这个头文件只放类型与原型，不放逻辑。
 */

#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

#include "check.h"
#include "typelayer.h"   /* the read-only type questions, shared with codegen */

/* 编译计划不在这棵 AST 上：存储与读写口都在计划侧（`plan.h` 的访问器/setter + 按节点寻址的侧表），
 * codegen 只经访问器读。`tools/check_plan_seam.py` 钉住三件事：
 * codegen 直读计划字段 = 0、AST 上不再出现已搬走的字段、回写不超基线。
 * 已搬走：arenaLevel/zoneLevel/arenaArg/needTemp、usesHome/mayUseArena/makesPool/condAllocs、
 * isCoro/yieldType/coroFrameType/coroNeedsZone/coroProto。仍在 AST 上：Expr.func、
 * FuncDef.tmpl、instName、coroKind、Stmt.forStep（半语法，解析器与检查器各一半）。 */
/* 检查器的阶段：定义放这里是因为 `Checker` 结构体里要存它（契约见本文件后面的"阶段契约"一节）。*/
typedef enum {
    PHASE_INIT = 0,      /* 开始之前：还没有任何表 */
    PHASE_DECLS,         /* ckB0–ckB5：声明、签名、C-ABI 检查 */
    PHASE_BODIES,        /* ckB6–ckB7：函数体（作用域存在） */
    PHASE_POST           /* ckB8 起：摘要、电平、重放、复核（**没有作用域**） */
} CheckPhase;

/**
 * @brief 取检查阶段的短名字符串。
 *
 * @param p  检查阶段枚举值。
 * @return   该阶段对应的名字（`init`/`decls`/`bodies`/`post`）；不在其中时返回 `"?"`。
 */
static inline const char *phaseName(CheckPhase p) {
    switch (p) {
    case PHASE_INIT:   return "init";
    case PHASE_DECLS:  return "decls";
    case PHASE_BODIES: return "bodies";
    case PHASE_POST:   return "post";
    }
    return "?";
}

/* Display name of a declaration for diagnostics: `DN(x)` takes a `StructDef *` or a
 * `TypeDef *`.
 *
 * After module mangling `name` is the internal `io$reader`, while `srcName` is the
 * `io::reader` the user wrote. Printing `->name` leaks the internal encoding into the
 * message, as in "struct `alpha$pair` has no field `zzz`".
 */
#define DN(d) ttDispName((d) ? (d)->srcName : NULL, (d) ? (d)->name : NULL)

/**
 * @brief 把改写过的函数名还原成用户书写的形式，用于诊断。
 *
 * 结果可能指向一个静态缓冲区，下一次调用会覆盖它。`name` 为 NULL 时返回 `"?"`；模块名为空
 * （根模块）或名字里没有 `$` 前缀时原样返回 `name`。
 *
 * `FN(f)` 是它的诊断别名：`FN(f)` 展开成 `checkFnDisplay(f->name, f->modName)`。
 *
 * @param name     改写后的名字，可为 NULL。
 * @param modName  模块前缀；根模块里为空或 NULL。
 * @return         形如 `mod::fn` 的显示名；缓冲区放不下时返回 `name`。
 */
const char *checkFnDisplay(const char *name, const char *modName);
#define FN(f) checkFnDisplay((f) ? (f)->name : NULL, (f) ? (f)->modName : NULL)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------ shared state and shared types
 * Whatever more than one file needs, so that whoever uses it can see it. */

typedef struct {
    const char *name;    /* name as written in the source */
    const char *cname;   /* name used in generated C; a shadowed `a` becomes `a__2` */
    Type       *type;    /* declared type, or the inferred one for an unannotated `let` */
    bool        mut;     /* true for `var`; false for `let` and for payload bindings */
    /* True once the address of this binding has been handed out: `ref x`, `x[..]`, or
     * passing it as a `mut ref` argument.
     *
     * With a taken address an alias can change the contents behind our back, so the
     * depth bookkeeping may only be updated weakly. Without one an entry is overwritten
     * outright, which removes the false rejections where writing null or a shallower
     * value could not lower the recorded depth. */
    bool        addressed;
    /* Per-field depths, at most four entries. To be read as one field block, not as
     * four independent ones.
     *
     * A field such as `h.p` gets a depth of its own instead of sharing one number with
     * `h`, so `h.p = null` can really overwrite that entry. Writes that cannot be
     * attributed to a single field, such as an element write, go to `otherDepth`. The
     * effective depth of the root is the maximum of `otherDepth` and the field entries. */
    struct { const char *name; int depth;  /* field name; recorded depth of the value in it */
             /* Which value was last written into this field, or NULL when that is
              * unknown. It is used only to find the allocation site inside a container,
              * never to change any bookkeeping.
              *
              * It is needed because `var h: box = { p: null, q: null }` has an origin
              * made of nulls which never mentions the `x` that a later `h.q = x` stored,
              * so walking origins would never reach that site. */
             Expr *src;
             /* Depth of the write that produced `src`. Compare against this number, not
              * against `depth`: `depth` is merged with the maximum on a weak update, so
              * after `h.q = x  h.q = null` it is still deep, and comparing with it would
              * call the null write the deeper one, clear the source and break the chain. */
             int   srcDepth;
             /* The shallowest level this field has been required to reach; 0 means it
              * must outlive the frame. `out = h` stores at level 1, but `return out`
              * needs the frame-outliving level, so the site has to end up in the home
              * arena. Looking only at the current destination computes a level that is
              * too shallow and leaves the site there, which dangles. */
             int   minReq; } fields[4];
    /* Whether the field table is complete.
     *
     * Complete means every field of the binding has an entry, or is known to be 0; only
     * then may an entry be cleared and the root recomputed as the maximum of the
     * remaining ones. An incomplete table, such as one copied from elsewhere, can have
     * fields with values that are missing here, and lowering a depth would make those
     * values vanish, so a missing value must not be treated as 0. The default is false,
     * the conservative answer. */
    bool        fieldsComplete;
    int         nfields;    /* number of valid entries in `fields` */
    int         otherDepth; /* depth for writes that belong to no single field */
/* A store a **callee** made through this binding as a `mut ref` argument. Its own slot: the
 * "no single field" one is shared with element writes, and feeding both through it would give
 * every element write promotion pressure. The source expression is what lets `promoteFieldsAt`
 * move the site behind it: with a source the store can be promoted, without one it cannot, and
 * reading through the binding after the frame is gone dangles. */
    Expr       *callSrc;
    int         callSrcDepth;
    int         callMinReq;
/* The same facts as one `fields[]` entry, for a store that belongs to no single field: an
 * element write, or a store the callee made through a `mut ref` out-parameter. Without a source
 * expression there is nothing to promote, and a `new` handed to a callee that stores it would be
 * rejected where the same program written as a direct `b.p = n` is accepted. */
    Expr       *otherSrc;
    int         otherSrcDepth;
    int         otherMinReq;
    /* 声明处初始化式留下的**别名来源**（`var head = a`）：与 `otherSrc` 分开一格，因为
     * `otherSrc`/`otherDepth` 同时被 `targetDepth` 当成绑定深度的上界读走，把声明式记进去
     * 会让普通形状多算一层（`head = p` 于是被误拒）。这里只服务提权：`promoteFieldsAt`
     * 沿它把要求传给来源背后的分配站点。**不展平**：`head` 与 `a` 共享所指对象，a 之后被
     * 改指（`a = p`）时才知道该提哪个站点；展平过的 `origin` 是拷贝当时的来源。 */
    Expr       *initSrc;
    int         initSrcDepth;
    int         initMinReq;
    int         depth;      /* lexical depth: a parameter is 0, a local in the function
                             * body is 1, and every nested block adds 1. The escape
                             * checks compare this number. */
    /* 引用型绑定**可能**指向的最浅深度（越小活得越久）；-1 = 还没记过。
     *
     * 目的地的深度要的是"可能活得最久的被指物"：`cur` 可能指向参数（0）也可能指向帧内 `new`
     * （1），把 `(*cur).f = v` 的目的地读成 max（DFA 给的那个）会把帧内的 `v` 放进调用者的
     * 对象，调用一返回就悬垂。这里按每次改指取 min，只往更长寿的一侧偏（安全方向）。 */
    int         minPointee;
    /* For a binding that holds a reference: how deep is the thing it points at?
     *
     * This differs from `depth`. In `var cur: ?ref node = head` the slot holding the
     * reference is in this frame, at depth 1, while the node it points at is outside,
     * at depth 0. The rule "a reference may not outlive what it points at" asks about
     * the pointee, so it needs this number. Using the slot depth everywhere falsely
     * rejected the most ordinary list search, `while cur != null { ... return cur }`,
     * as pointing at a local that dies.
     *
     * It is meaningful for a reference-typed binding, and it doubles as an upper bound
     * on where the references inside a reference-carrying value point. The default is
     * the slot depth, the conservative answer; a declaration or a retarget narrows it
     * to the real depth of the value. */
    /* Set when this binding was initialized from an expression whose storage is provably **not**
     * in any frame: a chain rooted at a global, or a call whose declaration says `effects Ret=0`
     * (the plate's views; `mmap`-style handles). Then a **view**-typed binding answers with
     * `refDepth` instead of its slot's block depth, which is what lets
     *
     *     var v: mut slice<u8> = pl.view(off, n)!      // 板的内存，不在帧里
     *     c.func(v.data)                               // 未签字的 C 调用也接受
     *
     * work without the user signing anything. Conservative by default: an unmarked binding keeps
     * the slot depth -- a `new` block, a stack array, an arena allocation, or a callee that did
     * not claim it. */
    bool        outOfFrame;
    /* The value this binding was initialized with, or NULL when it had no initializer.
     *
     * Promotion walks origins backwards:
     *     var n = new node        // the origin of `n` is this `new`
     *     head = n                // stored into an outer place, so follow `n` back and
     *                             // raise that `new` to the level of `head`
     * Only the initializer is followed, and only through explicit assignments; aliases
     * are not chased, so a site that cannot be reached keeps its depth error. */
    Expr       *origin;
    /* The expression that last gave this binding its value, kept unflattened.
     *
     * `origin` answers "where does the storage inside this binding come from" and flattens
     * a chain of bindings to the expression at its root, which is what the arena-level walk
     * needs. The level pass asks a narrower question -- which expression is this binding
     * holding -- and there the root of a chain is the wrong answer, because it belongs to
     * whichever binding the chain happened to end at.
     *
     * It still describes only the last assignment, so it cannot stand for a binding written
     * in both arms of an `if`; the publication records are the authority for that, and this
     * field is a shortcut for the chain-of-assignments case. */
    Expr       *heldSrc;
    /* 这一格是**池的提权**专用的：绑定由一次"建池的调用"初始化时，把那个调用的表达式记在这里。
     *
     * 为什么不复用 `origin`：`originOf` 只认分配站点的形状（`new` / `alloc`），对调用返回 NULL，
     * 因为 arena 的层级不由调用决定而在调用点决定（见 `origin` 的注释）；而池的层级恰恰**就是**
     * 由调用点决定的（`extc_pool_new_at` 的第二个实参）。单开一格，别的消费者（`origin` 那一族）
     * 一个都不受影响。 */
    Expr       *poolSite;

    int         line;      /* source line of the declaration, for diagnostics */
    /* The module this binding belongs to; set for globals, NULL for locals. Name
     * resolution uses it to reject an unqualified reference to another module's name. */
    const char *modName;
} Sym;

/* Pins the binding a name resolved to onto the AST node that used it.
 *
 * The pass that runs after all bodies have been checked cannot look a local up by name,
 * because its scope is gone by then; the same trap is described in `noteOrigin`. What
 * the pinning avoids is a re-check that silently uses the depth frozen before the
 * solver ran and rejects a correct program.
 *
 * A binding pointer is stored rather than a name, because one name in different scopes
 * is a different binding: the answer at the moment of resolution is the authoritative
 * one, while looking the name up again later can find another binding that happens to
 * have the same name. The lifetime is not a problem, because bindings are
 * arena-allocated, so leaving a scope only removes them from visibility.
 *
 * The wrapper is there for type safety: the AST layer cannot name `Sym` and therefore holds
 * an opaque pointer, the same arrangement a `cname` uses, so each side casts once
 * instead of passing a bare `void *` around. */
typedef struct { Sym *sym; } IdentBinding;

/**
 * @brief 取出 `EX_IDENT` 节点上解析到的绑定。
 *
 * @param e  要查看的表达式。
 * @return   该标识符解析到的绑定；节点不是 `EX_IDENT` 或尚未解析时返回 NULL。
 */
static inline Sym *identBindOf(Expr *e) {
    return (e && e->kind == EX_IDENT && e->u.ident.sym) ? ((IdentBinding *)e->u.ident.sym)->sym : NULL;
}

/**
 * @brief 把分配站点的 arena 层级换算成引用的深度。
 *
 * 层级与深度必须是同一个数的两种写法，换算只保留这一处。`ARENA_HOME` 和非正数（层级尚未
 * 定下）都算 0，正数层级 k 换算为深度 k。
 *
 * @param arenaLevel  站点的 arena 层级：`ARENA_HOME`、正的块层级，或尚未定下的 0/负数。
 * @return            换算出的深度；要区分"未定"与"深度 0"的调用者须自己看 `arenaLevel`。
 */
static inline int arenaDepthOf(int arenaLevel) {
    if (arenaLevel == ARENA_HOME) return 0;
    return arenaLevel > 0 ? arenaLevel : 0;
}

/* How often a name is used in the current function; the C name generator uses the count
 * to decide whether a rename is needed. */
typedef struct { const char *name; int count; } NameUse;

/* One lexical scope: the bindings declared in it. The innermost scope is last. */
typedef struct { Vec syms; } Scope;

/* A call site whose choice of home arena depends on which locals escape.
 *
 * Everything needed to recompute that choice is recorded here, so the pass that runs
 * after all bodies have been checked does not have to look anything up in a scope that
 * is gone by then. */
typedef struct {
    Expr  *call;         /* the EX_CALL / EX_METHOD / EX_ASSOC node; its arenaArg is
                          * rewritten by the later pass */
    char  *argRoot[8];   /* root name of each `mut ref` argument, NULL when that position
                          * is not a `mut ref` argument */
    int    argDepth[8];  /* depth of that argument at the call site; 0 = a parameter or a
                          * global, which already outlives this frame */
    int    n;            /* number of slots in use, at most 8 */
    bool   overflow;     /* more arguments than slots, so the picture is incomplete and
                          * the call is treated as receiving the home arena: conservative
                          * but sound */
} EArenaSite;

/**
 * @brief 判断 codegen 是否会为这个语句建立 place 边界。
 *
 * 答案必须覆盖 codegen 的行为：答"是"的集合只会更大，绝不会漏掉 codegen 真正括起来的块。
 * 协程体内不建立这种边界，因此 `inCoro` 为真时返回 false。
 *
 * @param s       要判断的语句。
 * @param inCoro  该语句是否位于协程体内。
 * @return        语句含 `new` 或会建池时为 true；否则为 false。
 */
bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro);

/**
 * @brief 判断类型里任意深处是否含有指定的协议标记。
 *
 * 标记可以出现在类型本身、类型实参里，或 view/数组/引用之后。为避免自引用类型让遍历无法
 * 终止，只沿这些位置下行，不展开结构体字段；递归深度超过上限时按保守答案返回 true。
 *
 * @param tt    类型表。
 * @param t     要检查的类型；NULL 视为不含该标记。
 * @param name  标记的名字，例如 `coroutine`。
 * @return      找到该标记时为 true。
 */
bool typeContainsProto(TypeTable *tt, Type *t, const char *name);

/* A `yield` binding: it becomes a frame field, but the frame's Vec only exists after the body is
 * checked, so the name and type are recorded and consumed by the frame layout pass. */
typedef struct {
    struct FuncDef *fn;
    const char    *name;
    Type          *type;
    int            line;
} CoroBind;

typedef struct Checker {
    /* How many enclosing blocks will reclaim their own storage (codegen brackets them with
     * `zoneEnter`/`zoneLeaveTo` + `arena_release`). A `yield` inside one of them is a compile
     * error: that block's arena would be gone before the coroutine is resumed. */
    int      placeBoundaryDepth;

    Vec      coroDeferred;
    Vec      coroBinds;        /* `var x = yield e` bindings (CoroBind), consumed by coroFrameLay */

    Ctx       *ctx;         /* parser and module context, for diagnostics and lookup */
    Arena     *arena;       /* arena the checker allocates its own bookkeeping from */
    TypeTable *tt;          /* type table of the module being checked */
    Module    *m;           /* the module being checked */
    Vec        scopes;      /* Scope*, innermost last */
    FuncDef   *curFunc;     /* body being checked, or NULL outside a function body */
    Vec       *curParams;   /* names of the type parameters in scope, NULL outside a
                             * generic context */
    Vec        opChecks;    /* OpCheck*: overloadable operators deferred to instantiation */
    Vec        methodChecks; /* MethodCheck*: method calls on a type parameter */
    Vec        deferredUses; /* DeferredUse*: uses of such a call's result, re-checked */
    /* Explicit type arguments written at a call site (`f<i32>(...)`): the node plus the arguments.
     * The `EX_GENCALL` path rewrites itself into an ordinary `EX_CALL`, and inference has to start
     * from these written arguments; otherwise the written form would be pure decoration and would
     * work only where inference happened to succeed anyway. Kept **here** and not as a field on
     * `Expr`: that union is not zeroed for every node kind, so a field only one path sets reads as
     * garbage on the others. */
    Vec        explicitTargs; /* ExplicitTargs* */
    /* Instances of generic free functions: `fn f<T>` gets one instance per set of type
     * arguments. They are created here, at the call site that infers the arguments, and
     * the code generator emits them as ordinary functions. */
    Vec        funcInsts;   /* FuncDef*, with tmpl/targs/instName filled in */
    Vec        globals;     /* Sym* for module-level bindings (depth 0). They are not in
                             * `scopes`; see the comment in `lookup` */
    /* Every binding the checker has declared, appended by `declare`.
     *
     * The self-check mode walks this list to verify that the `refDepth` of a binding
     * agrees with the allocation site it came from. */
    Vec        allSyms;
    /* 一次 `promoteFieldsAt` 走查正踩着的绑定栈（环守卫）。
     *
     * 来源表可以成环（`a = b  b = a`，或两个结构体的字段表互指）——按来源链走会无限递归。
     * 原来只靠跳数上限兜底，撞上限会走 `EXTC_DBG_FALLBACK`（断言版直接判红），而且把 32 步
     * 白白烧在一个环上。同一个绑定在一次走查里只走一次就够了：要求已经正沿那条路施加。
     * 存储只有 64 项、按栈式 push/pop，不需要清零。 */
    Sym       *fsChain[64];
    int        fsDepth;
    /* The value currently being stored somewhere.
     *
     * `noteFieldDepthWrite` records which value wrote a field, and its callers already
     * hold that value; passing it through a field keeps the signature of that function
     * unchanged, which would otherwise ripple through every call site. */
    Expr      *curStoreVal;
    Vec        nameUses;    /* NameUse*: how often each name is used in the current
                             * function, for the C name generator */
    Vec        moduleNames;     /* const char*, sorted; the sort enables a compile-time
                                 * lookup optimization */
    size_t     moduleNamesSize;
    /* Non-null narrowing for `?ref T`: which bindings are known not to be null at the
     * current position.
     *
     * A nullable reference may not be dereferenced before it has been shown to be
     * non-null, and the only way to show that is a comparison with null inside an `if`.
     * Narrowing is lexical, so it unwinds together with pushScope and popScope, and it
     * generates no runtime check at all: what the compiler can prove leaves no trace in
     * the generated code. */
    /* Non-zero while the current position cannot emit a statement prefix, which makes
     * every `??` that needs one an error. Two positions behave that way: the condition
     * of a `while`, where a hoisted prefix would be evaluated once before the loop
     * instead of once per round, and a global initializer, which has no statement to
     * hang a prefix on. */
    /* Checks deferred out of a generic body.
     *
     * A generic body is checked once, as a template, with `T` opaque, so every rule of
     * the form "only check this when the type carries a reference" returns early and an
     * instantiation with `T = slice<u8>` is never checked at all. The depth is therefore
     * computed when the rule is recorded, assuming that `T` may carry a reference, while
     * the scope is still open and `lookup` still works; instantiation then only has to
     * answer whether `T` really carries one.
     *
     * Deferring the depth computation itself does not work: the function scope is gone
     * by then, `lookup` cannot find the parameters, the depth comes out as 0 and the
     * check silently stops working. */
    Vec        refChecks;   /* RefCheck*: reference rules re-run at instantiation */
    /* Level-dependent rejections, recorded so the same question can be asked again once
     * the data flow and the level pass have settled the numbers. Reporting stays where it
     * was; the record is what makes the two answers comparable. */
    Vec        lvlRejects;  /* LvlRejection* */
    Vec        callChecks;  /* CallCheck*: call sites resolved at instantiation */
    Vec       *substParams; /* type arguments substituted while an instance is being
                             * re-checked, NULL when not re-checking */
    Vec       *substArgs;
    int        noHoist;     /* non-zero while a statement prefix cannot be emitted */
    /* How many allocation sites have been created so far. A loop condition asks for the
     * difference across its own check to learn whether it allocates at all (`Stmt.condAllocs`),
     * which is what decides between the plain `while (cond)` and the per-round-release shape. */
    int        allocSites;
    CheckPhase phase;      /* 见上面的"阶段契约"：误用由断言看守 */
    /* How many subexpressions with a side effect have already been checked in the
     * current statement.
     *
     * A `??` whose subject is not pure has to be evaluated into a temporary, and that
     * prefix is emitted before the whole statement, which would move it ahead of earlier
     * side effects in the same statement. Once a side effect has been seen and this `??`
     * still needs a temporary, the program is rejected and the user is asked to split
     * the line. Only calls count, because the relative order of allocations, literals
     * and conversions is not observable. */
    int        stmtFx;
    /* Nodes in the current function body whose arena assignment has to be decided again
     * once every effect summary is closed: `new` sites, and the call sites whose
     * allocation arena is still open. After the body has been checked they move to
     * `FuncDef.arenaSites`, and their levels are fixed once the pass that computes
     * `needsHome` has run. */
    Vec        curArenaSites;
    /* Call sites whose choice of home arena depends on which locals escape.
     *
     * That choice has to wait until the effect summaries of all functions are closed,
     * because the rule "pass my home arena when the callee may move the argument out of
     * this function" depends on the set of escaping locals, and computing that set needs
     * the summary of the callee. The summary is not closed while bodies are still being
     * checked, so with an empty summary the call would look like one that never
     * publishes its argument, the home arena would be taken to be the caller's own
     * frame, and everything the callee stored in that container would be released as
     * soon as the call returned. The call sites are recorded here and recomputed by the
     * final pass. */
    Vec        eSites;      /* EArenaSite* */
    /* The table of "this value was stored at level k" requirements.
     *
     * Every entry is handed over by an escape check that was going to run anyway, so no
     * extra traversal is written to find the store sites. The final pass replays them to
     * a fixed point, and the level of an allocation site ends up as the smallest of all
     * the requirements that reached it, since a smaller level lives longer.
     *
     * The fixed point is needed because one requirement only moves the sites it reaches,
     * and once those live longer, further requirements start to reach further sites.
     * This has the same shape as the call-site arena decision and as the transitive
     * closure of `needsHome`. */
    Vec        lvlFacts;    /* LvlFact* */
    /* Publications, appended while the bodies are checked one after another.
     *
     * The vector is **module-wide** (a body's records have to survive the body, because the
     * module-wide requirement replay and the `[store]` dump read them), but every reader asks a
     * question about **one** body, and each record carries the body it came from (`StoreSite.fn`)
     * plus the index that body's records start at (`storeBase`). A reader that wants "the
     * publications of this body" walks `[storeBase, stores.len)` and skips records whose `fn` is
     * not its own (a nested body's records fall inside the slice, hence the second test). The
     * module-wide requirement table, which is deliberately kept across bodies, is `lvlFacts`. */
    Vec        stores;      /* StoreSite* -- publications, folded over by the level pass */
    size_t     storeBase;   /* index in `stores` where the current body's records begin */
    /* ---- `EXTC_DBG_FX=1`: what the level pass and the effect closures cost ----
     *
     * Cheap counters, in the style of the other `EXTC_DBG_*` switches: each one is bumped
     * next to work that already walks a value, so reading them costs nothing that the pass
     * was not already doing, and they are printed once by `checkModule` when the switch is
     * on. They exist because "the front end is quadratic in the length of a call chain"
     * cannot be told apart from "the front end is linear and slow" without knowing which
     * quantity is growing: the counters below separate the records a pass **visits** from
     * the records that are its own, and the table **comparisons** from the calls that make
     * them. `fxOn` caches the switch so the per-element loops never call `dbgOn`. */
    bool       fxOn;          /* `EXTC_DBG_FX`, read once per pass */
    long       fxLvlPasses;   /* level-pass runs (one per checked body) */
    long       fxStoresSeen;  /* store records the level-pass loops looked at */
    long       fxStoresOwn;   /* ... of those, records of the body being solved */
    long       fxTailRecords; /* sum over bodies of the records each body owns */
    long       fxSymCalls;    /* `symLevel` + `setSymLevel` calls */
    long       fxSymCmp;      /* level-table entries compared by those calls */
    long       fxTblSum;      /* sum over passes of the table size at the end */
    long       fxTblMax;      /* largest table a single pass built */
    long       fxLvlRounds1;  /* rounds the "binding level" fixed point ran */
    long       fxLvlRounds2;  /* rounds the "site level" fixed point ran */
    long       fxLvlMoved;    /* bindings whose level really moved */
    long       fxAliasWalks;  /* extra values walked by the join rule of step two */
    long       fxDedupSteps;  /* backward scans of `recordStore`'s duplicate test */
    long       fxPreBodyStores;  /* records made before the first body was walked */
    long       fxAltScan;     /* store records the "every assignment of `dst`" loop looked at */
    long       fxReplayRounds, fxReplayFacts;   /* the module-wide fact replay */
    long       fxEffCalls, fxEffEdges;          /* `computeEffectsTransitive` */
    long       fxEffCached;   /* those calls answered from the memo instead of recursing */
    long       fxReachRounds;                   /* rounds of the `closeReach` driver */
    long       fxReachVisits;                   /* functions a round of `closeReach` considered */
    /* True while requirements are being replayed. Recording a new requirement during a
     * replay would make the table grow without bound, so recording is suppressed then. */
    bool       lvlSolving;
    Vec        narrow;      /* const char*: cnames proved to be non-null */
    /* Names of the locals of the current function that can be moved out of it.
     *
     * This only affects which arena is chosen as the home arena, and the conservative
     * direction is to include too many names: that costs memory, while missing one would
     * let a live pointer dangle. */
    Vec        escapees;    /* const char* */

    Vec        narrowMarks; /* size_t: length of `narrow` when each scope was entered;
                             * popping the scope truncates `narrow` back to it */

    /* How many **domains** the checker is inside right now (`sched::run { … }`,
    * `par::pool(n) { … }`). `ext` is legal only when this is non-zero: outside a domain there is
    * nobody to hand the task to, and that is a compile error rather than a runtime surprise. */
    int        domainDepth;
    /* **The nearest domain** (`d.run { … }`), i.e. the receiver expression whose block we are
     * checking right now. The scope rule is "`ext` hands the task to the nearest domain", which is a
     * stack; saving and restoring this one field around a block *is* that stack, with no allocation.
     * NULL means "no domain in scope", and that is a compile error at the `ext`. */
    Expr      *curDom;
    /* How many lambda bodies are being checked right now. A lambda inside a lambda would have its
     * environment laid out as part of the outer one; the checker refuses that instead of laying it
     * out wrongly. */
    int        lamDepth;
    /* How many lambda environments this compilation has generated, for unique names. */
    int        lamSeq;

    Type *tI32, *tI64, *tF64, *tBool;  /* cached primitive types */
    StructDef *sliceDef;/* declaration of `slice<T>` from the prelude, which is the view
                         * protocol */
    Type *tSliceU8;     /* type of a string literal: `slice<u8>` */
} Checker;

/* One reference rule deferred to instantiation. */
typedef struct {
    Expr       *val;     /* the value being checked */
    Expr       *target;  /* non-NULL when the value is being stored into this target */
    int         at;      /* destination depth, computed at record time while the scope
                          * still existed */
    int         depth;   /* depth of the references in the value, computed the same way
                          * and assuming `T` may carry a reference */
    bool        borrowed;/* whether the value is borrowed, computed the same way */
    /* A second kind of deferred rule: the zero value.
     *
     * `var local: T` with no initializer looks harmless while `T` is opaque, but the
     * instantiation `T = slice<u8>` produces a null reference, and the language promises
     * that a `ref` is never null. */
    bool        isZero;
    /* A third kind: the size in `new T[n]`.
     *
     * While a body is a template, `T` has no size, so the check can neither pass nor
     * fail; it is recorded and then run for every concrete instantiation. */
    bool        isNewSize;
    Type       *declType;/* declared type the deferred rule applies to */
    int         line;    /* source line to report */
    const char *what;    /* short noun phrase naming the operation, for the diagnostic */
    FuncDef    *func;    /* the generic function this rule belongs to */
} RefCheck;

/* The requirement "this value has to fit into level `at`".
 *
 * It is kept as a fact rather than as a decision because a smaller level means a longer life,
 * so the final level of an allocation site is the smallest of all the requirements that
 * reached it, while which requirements reach it depends on the level the site currently has.
 * Replaying to a fixed point settles both. Recording a decision as soon as a `new` is seen
 * would allow movement in one direction only and make it impossible to tell afterwards whether
 * a level was decided or computed. */
typedef struct { Expr *val; int at; } LvlFact;

/* One publication: a value observed being handed to a place that may outlive it.
 *
 * Every store, return, and "may keep it" argument call is one of these. Recording is
 * the checking pass's entire involvement with arena levels; deciding which arena each
 * allocation site ends up in is a separate pass that folds over this table. Keeping the
 * two apart is what makes the decision independent of the order the checker walks the
 * tree in.
 *
 * Params are captured rather than derived later, because `at` needs the scope stack and
 * because the binding's origin may legitimately be retargeted later in the body. */
typedef struct {
    Expr       *value;   /* the value being published */
    Expr       *target;  /* the place it is published into; for a return, the returned value */
    int         at;      /* arena level of the destination; 0 = beyond this frame */
    int         line;    /* for diagnostics */
    /* The body this publication came out of (`Checker.curFunc` at the time), or NULL for one
     * recorded outside every body.
     *
     * The record table is per module, because it is filled while the functions are checked
     * one after another, but every reader of it asks a question about **one** body: the level
     * pass folds the publications "recorded while the body was checked", and the extent of a
     * walk is a property of the body's own expressions and bindings. The owner is what lets a
     * reader say which records are its own, so the level pass of one function neither re-folds
     * every other function's publications nor enters their bindings into a level table that
     * cannot reach them. A record made outside every body has no owner and is never folded. */
    FuncDef    *fn;
} StoreSite;

/* A rejection that depends on a level, recorded so it can be re-judged against the
 * numbers that are final rather than the ones that happened to hold while the body was
 * being walked. */
typedef struct {
    Expr       *val;    /* the value that was rejected */
    int         at;     /* the depth it was judged against */
    int         depth;  /* the depth the check saw */
    int         line;   /* source line */
    int         late;   /* depth the settled numbers gave */
} LvlRejection;

/* An operator use on a type parameter, deferred to instantiation: recorded while an
 * expression is checked and consumed by the pass that runs afterwards.
 *
 * On the template `T` is opaque, so whether it supports the operator is only answerable per
 * instance; the comparison, ordering and arithmetic operators all use this record. `op` carries
 * which operator it was, and the consumer re-asks the same predicate the concrete-type path
 * uses.
 *
 * `func` is the function the expression belongs to, which is how the consumer asks
 * whether that function was ever called. */
typedef struct { Expr *node; StructDef *owner; const char *op; FuncDef *func; } OpCheck;

/* A method call on a type parameter, deferred to instantiation.
 *
 * While a template is checked `T` is opaque, so `k.hash()` has no method to resolve: which
 * method it means is decided by whatever the instance substitutes for `K`. Recording it here
 * and resolving per instance is also the only place a good diagnostic can come from -- only
 * the instance knows whether its type defines the method at all.
 *
 * The template pass hands back the error type for such a call: the statement keeps checking
 * and `checkAssignable` waves an error type through, while the instance pass is the one that
 * decides -- and it names the instance, exactly as `OpCheck` does.
 *
 * `nargs` is the number of arguments written at the call, so the instance pass can report an
 * arity mismatch instead of letting it reach the C compiler. `func` is the template holding the
 * call, which is how the driver picks the instances this record applies to. */
typedef struct {
    Expr       *node;     /* the EX_METHOD node */
    StructDef  *owner;    /* enclosing generic struct/enum, or NULL inside a free function */
    const char *name;     /* the method name being called */
    size_t      nargs;    /* arguments written at the call site */
    FuncDef    *func;     /* the template function the call sits in */
} MethodCheck;

/* A call written `f<i32>(...)`: what the rewrite to `EX_CALL` must hand to inference. */
typedef struct {
    Expr *node;      /* the call node the arguments belong to */
    Vec   targs;     /* Type*: the written arguments, in declaration order */
} ExplicitTargs;

/* A use of a deferred method call's RESULT, re-checked at instantiation.
 *
 * On the template, `k.hash()` hands back the error type (`MethodCheck` explains why), and
 * `checkAssignable` waves an error type through, which keeps the template pass from inventing
 * errors. But "no error here" must not be the last word: `fn f<T>(x: T) -> i8 { return
 * x.hash() }` must be rejected as a lossy conversion when the instance's `hash` returns `i64`,
 * exactly as the same code written on a concrete type is.
 *
 * The expected type IS known at the use site, so it is recorded here and checked again per
 * instance, once the real signature is resolvable. `want` is that expected type; `call` is the
 * deferred call the value came from. */
typedef struct {
    Expr      *call;      /* the expression the value came from: a method call deferred to
                           * instantiation, or any expression whose type is a **type parameter**
                           * (the two shapes `checkAssignable` records below) */
    Type      *want;      /* the type the context expects */
    StructDef *owner;     /* enclosing generic struct/enum, or NULL in a free function */
    FuncDef   *func;      /* the template holding the use, which picks the instances */
} DeferredUse;

/* A call site whose resolution is deferred to instantiation.
 *
 * In `fn twice<T>(x: T) -> T { return idOf(x) }` the template is checked with `T`
 * opaque, so the type argument of `idOf` is `T` itself and no correct instance can be
 * built at that moment: the only one available would be `idOf_T`, an instance that
 * still takes a type parameter. This is the same family of problem as `RefCheck` and
 * `OpCheck` and is solved the same way: record it while the template is checked and
 * resolve it again for each instantiation.
 *
 * Skipping the checks that follow is not an option, because the rest of the template
 * body uses the type of `e->func` to check arguments and to compute the return type, so
 * some signature has to be available. A provisional instance with `T` as its argument
 * is therefore created, which is self-consistent because `T` is used as an opaque type,
 * and instantiation later points `e->func` at the concrete instance.
 *
 * The difference from `RefCheck` is that this rewrites `e->func` in the AST, so it has
 * to run in the context of the instance being re-checked. That context already exists,
 * because the re-check pass sets `c.substParams` and `c.substArgs`. */
typedef struct {
    Expr    *node;      /* the EX_CALL / EX_METHOD / EX_ASSOC node */
    FuncDef *tmpl;      /* the template being called: a free function or a method of a
                         * generic type */
    Vec      targs;     /* arguments inferred while the template was checked; they may
                         * contain `T` and have to be passed through `tsub` */
    FuncDef *func;      /* the template function this entry belongs to, which decides
                         * the instances it applies to */
} CallCheck;

/* -------------------------------------------------- cross-file prototypes
 *
 * The prototypes that cross between the `check_*.c` files. Each signature is written here
 * once, and its definition lives in exactly one of those files. */

/**
 * @brief 造一个整数常量表达式节点。
 *
 * 类型固定为 `i32`；编译器用它补上用户省略的切片边界，好让 codegen 看到两个字面边界，
 * 从而知道该范围不需要运行时检查。
 *
 * @param c     检查器，节点从它的 arena 分配。
 * @param v     常量的值。
 * @param line  源行号。
 * @return      新建的 `EX_INT` 节点。
 */
 Expr *intLit (Checker *c, long long int v, int line);
/**
 * @brief 按名字在结构体定义里查一个字段。
 *
 * @param sd    要查的结构体定义。
 * @param name  字段名。
 * @return      命中的字段定义；没有该名字时返回 NULL。
 */
 FieldDef *findField (StructDef *sd, const char *name);
/**
 * @brief 按名字在模块里查一个模块级函数。
 *
 * 泛型函数的实例不是程序能写出的名字，因此不参与匹配，模板本身可以匹配。
 *
 * @param c     检查器。
 * @param name  已经取平的函数名。
 * @return      匹配到的模板或普通函数；没有时返回 NULL。
 */
 FuncDef *findFunc (Checker *c, const char *name);
/**
 * @brief 在类型上按名字查一个方法。
 *
 * 方法挂在类型自己的定义体里，因此不考虑同名自由函数；`Type.mholder` 里的方法比类型定义
 * 体更具体，先在那里查。
 *
 * @param st    要查的类型。
 * @param name  方法名。
 * @return      命中的方法定义；没有时返回 NULL。
 */
 FuncDef *findMethod (Type *st, const char *name);
/**
 * @brief 查一个类型的运算符方法，必要时退回另一个名字。
 *
 * `!=` 没有自己的方法时会退回 `==`，由 codegen 取反实现。
 *
 * @param tt        类型表；泛型接收者的实参在这里替换。
 * @param b         已经剥掉外层包装的类型。
 * @param sym       要找的运算符名，如 `==`、`<`、`<<`。
 * @param rhs       右操作数的类型；运算符按它精确匹配。
 * @param fallback  `sym` 不存在时改试的运算符名；不需要退回时为 NULL。
 * @return          命中的方法定义；两个名字都没有对应操作数时返回 NULL。
 */
 FuncDef *findOp (TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);
/**
 * @brief 查一个类型上右操作数类型恰好匹配的运算符方法。
 *
 * 运算符的方法名可以重复，签名形状固定为 `self: ref T` 加一个操作数，因此只能按操作数类型
 * 来区分。泛型类型的方法用所属类型的参数书写，比较前先替换接收者的类型实参。
 *
 * @param tt    类型表，用于替换泛型接收者的实参。
 * @param st    要查方法的类型。
 * @param name  运算符名，如 `<<`。
 * @param rhs   右操作数的类型；NULL 表示只看名字，取第一个同名方法。
 * @return      匹配的方法定义；该类型没有这个运算符时返回 NULL。
 */
 FuncDef *findOperator (TypeTable *tt, Type *st, const char *name, Type *rhs);
/**
 * @brief 取类型背后的结构体定义。
 *
 * `TY_STRUCT` 与 `TY_GENERIC` 都指向一个 `StructDef`，其他类型没有。
 *
 * @param t  要查的类型。
 * @return   对应的结构体定义；类型为 NULL 或其他种类时返回 NULL。
 */
 StructDef *structOf (Type *t);

/**
 * @brief 对按值拷贝 `@sharesStorage` 类型的绑定位置发出警告。
 *
 * 这类拷贝给同一块存储起了第二个名字：写一边会从另一边看见，`release` 一边会让另一边悬垂。
 * 只对读已有值的形状报告，新构造的值不报；库自身代码里的拷贝也不报。
 *
 * @param c  检查器，用于报诊断。
 * @param e  处于值位置的表达式。
 * @param t  它的类型。
 */
void warnSharedCopy(Checker *c, Expr *e, Type *t);

/**
 * @brief 对按值交出一个已有的 `@sharesStorage` 值发出警告。
 *
 * 交出已有值时调用者与本帧会共用同一块存储；刚在本函数体里新建的值属于移交，不报。判据是
 * 绑定声明的行号相对函数签名行号的位置。库自身代码里的返回不报。
 *
 * @param c  检查器，用于报诊断。
 * @param e  被返回的表达式。
 * @param t  它的类型。
 */
void warnSharedReturn(Checker *c, Expr *e, Type *t);
/**
 * @brief 在最内层作用域声明一个绑定并返回它。
 *
 * 引用类型的绑定以所在槽位的深度作为指向深度，随后由知道初始化式的一方收窄。调用者负责把
 * 生成的 C 名字写回 AST。
 *
 * @param c       检查器。
 * @param name    源码里的名字，生成的 C 名字由它派生。
 * @param t       声明的类型。
 * @param mut     `var` 为 true，`let` 为 false。
 * @param shadow  是否允许同名绑定遮蔽本作用域里已有的绑定。
 * @param line    源行号，用于诊断。
 * @param depth   新绑定的词法深度。
 * @return        新建的绑定；同名 `var` 被拒时返回已有的那个绑定。
 */
 Sym *declare (Checker *c, const char *name, Type *t, _Bool mut, _Bool shadow, int line, int depth);
/**
 * @brief 按名字解析出一个绑定，先查最内层作用域，最后查模块级绑定。
 *
 * 同一作用域内后声明的 `let` 胜出，因此从后往前扫。模块级绑定单独存放而不占一个作用域，
 * 否则函数体的深度会因全局多出一层。
 *
 * @param c     检查器。
 * @param name  要解析的源码名字。
 * @return      最内层同名绑定；没有时返回 NULL。
 */
 Sym *lookup (Checker *c, const char *name);
/* The binding at the root of a place, found by walking through fields, indexes and slices; NULL
 * when the place is not rooted in a binding. */
/* ---------------------------------------------------------------- 阶段契约
 *
 * `checkModule` 是一条**流水线**，阶段是显式状态（`Checker.phase`）：
 *
 *   | 阶段 | 何时 | 能看到什么 | **看不到什么** ⇒ 因此必须靠什么记住 |
 *   |---|---|---|---|
 *   | `PHASE_DECLS` | 结构体/枚举/trait/`@export`/C-ABI 检查 | 声明表、类型表 | 函数体的任何事实 |
 *   | `PHASE_BODIES` | 逐个函数体 | **作用域**（`lookup`/`placeRoot` 可用）、`stores`、各 `recordXxx` 记录端 | 效果摘要（要么没算、要么只按签名）、`makesPool` |
 *   | `PHASE_POST` | 效果摘要 → 延迟调用不动点 → 操作符/协议/结果类型/引用规则重放 → `eSites` 的 arena 与 zone 通道 → `lvlFacts` 重放 → `allSyms` 提升 → 逃逸集合 union 不动点 | 全部表；`refChecks`/`opChecks`/`lvlFacts` 这些**延迟机制**正是为这一阶段准备的 | **作用域**（已全部关闭）⇒ 对**局部**名字的解析必然落空或错认同名全局 |
 *
 * 因此 `PHASE_POST` 里凡是要"认身份"的事，都必须用**记在节点/符号上的**信息
 * （`identBindOf`、`FuncDef.paramSyms`），不要靠 `lookup`。`lookup`/`placeRoot` 需要作用域：
 * 调试构建（`EXTC_DBG=1`）在 `PHASE_POST` 里调用它们会当场 abort 并指出是哪一行，而不是
 * 静默给出一个"认不出"的答案。反过来，`PHASE_BODIES` 里不要读只在该阶段之后才成立的量
 * （例如 `makesPool`）：那类"当时还算不出来"的事实应当走 `recordLvlFact` 之类的延迟机制。 */

/* ---------------------------------------------------------------- 深度 / 身份：四个问题
 *
 * 这里唯一权威：**问哪件事，用哪个函数，极性是什么**。
 *
 * **未知（⊤）**：`DEPTH_UNKNOWN` —— 语义是「**证不出它活得够久**」，因此在**比较**那一侧必须
 * 导致**拒绝**。为什么不用 `0`：`0` 在本仓库是「活得最久」= **放行**的方向。它只允许出现在
 * 比较/记录里，**不许**当层号传给提权（`promoteInto2` 的 `want` 是「要提到哪一层」，不是深度）。
 *   ① 存储有多深？（`a`、`a.f`、`a[i]` 这些**地方**）      → `slotDepth`
 *   ② 值指向的东西有多深？（`p`、`f(x)` 这些**值**）        → `targetDepth`
 *   ③ 这个根的**绑定**是谁？（要身份，不要拼写）             → `placeRoot`（查作用域）
 *                                                           → `rootSymNoScope`/`identBindOf`（只看节点）
 *   ④ 这个根的**名字**？（诊断）                             → `placeRootName`
 *
 * **极性契约（最重要的一条）**：①② 返回的是**深度**，`0` = "活得最久"（全局、参数所指、
 * 调用者选的地方）；**数越小活得越久**。任何"认不出来"的兜底都必须落在**更长寿**那一侧，
 * 或者干脆拒绝。
 *
 * 一个事实一个指称：**新代码不要**再写第四份"从表达式取根"的循环；需要别的步进组合时，
 * 把 `EXTC_ROOT_*` 掩码传给 `extcRootLeaf`。三个入口的步进差异就是下面这些掩码：
 *
 *   | 入口 | 步进 | 跳数 | `EX_IDENT` 怎么解 |
 *   |---|---|---|---|
 *   | `placeRoot`      | FIELD INDEX SLICE | 不限 | 查作用域（`lookup`） |
 *   | `placeRootName`  | FIELD INDEX DEREF | 不限 | 取**语法名字**（不解析） |
 *   | `rootSymNoScope` | FIELD INDEX DEREF SIGN | 16 | 取节点上的绑定（`identBindOf`） |*/
/* 未知深度（⊤）：见上面「深度 / 身份」一节的极性契约 —— 比较处一律当「更深」处理（⇒ 拒绝），
 * 不许当层号传给提权。它是比较处的安全兜底：证不出活得够久，就当活得不够久。*/
#define DEPTH_UNKNOWN  (1 << 24)
/* 这个数是不是「未知」。*/
#define depthIsUnknown(d)  ((d) == DEPTH_UNKNOWN || (d) > DEPTH_UNKNOWN)

#define EXTC_ROOT_FIELD  (1u << 0)
#define EXTC_ROOT_INDEX  (1u << 1)
#define EXTC_ROOT_SLICE  (1u << 2)
#define EXTC_ROOT_DEREF  (1u << 3)
#define EXTC_ROOT_SIGN   (1u << 4)

/**
 * @brief 沿掩码允许的步进走到根的标识符节点。
 *
 * 只沿语法链走，不解析名字也不查作用域，返回的节点保证是 EX_IDENT。空掩码或含未知位
 * 在调试构建中被断言拒绝；走不到根时由调用者按"这不是一个地方"保守处理。
 *
 * @param e        起点表达式。
 * @param steps    允许的步进掩码，EXTC_ROOT_* 位的或。
 * @param maxHops  跳数上限；<= 0 表示不限跳数。
 * @return         根部的标识符节点；走不到根或超过跳数上限时返回 NULL。
 */
Expr *extcRootLeaf(Expr *e, unsigned steps, int maxHops);

/**
 * @brief 解析一个地方表达式根部的绑定。
 *
 * 沿字段、下标与切片走到根标识符，再用作用域查找解析成绑定；因此它要求至少有一个打开的
 * 作用域（调试构建中有断言）。所有作用域关闭后调用只能命中模块级绑定。
 *
 * @param c  检查器，提供作用域栈。
 * @param e  地方表达式。
 * @return   根标识符对应的绑定；走不到标识符或名字解析不到时返回 NULL。
 */
 Sym *placeRoot (Checker *c, Expr *e);
/**
 * @brief 检查一个表达式并把类型写回节点，引用不被解引用。
 *
 * 这是检查器其余部分使用的入口，结果同时留在 e->type 上；表达式含调用时置上语句效果
 * 标志，供 `??` 判断是否需要提前取临时量。
 *
 * @param c  检查器。
 * @param e  表达式；NULL 直接得到错误类型。
 * @return   表达式的类型；无法确定时返回错误类型，调用者不会看到空类型。
 */
 Type *checkExpr (Checker *c, Expr *e);
/**
 * @brief 按期望类型检查一个表达式。
 *
 * 期望类型是 option 或 result 时先补全裸构造器；期望的是值类型时，`@noCopy` 值的拷贝与
 * 流运算符的借用结果都会被拒绝。引用出现在非引用的期望处会报错。
 *
 * @param c     检查器。
 * @param want  上下文期望的类型；NULL 表示只要一个值。
 * @param e     被检查的表达式。
 * @return      表达式的类型；引用出现在值位置时返回被指向的类型（此时已报错）。
 */
 Type *checkInto (Checker *c, Type *want, Expr *e);
/**
 * @brief 检查一个可能是 `e?` 的表达式。
 *
 * `?` 只在少数语句位置合法，那些位置走这条入口，其余表达式按值位置检查。
 *
 * @param c  检查器。
 * @param e  表达式，可能是 EX_TRY 节点。
 * @return   `?` 解包后的载荷类型，否则值位置的类型。
 */
 Type *checkMaybeTry (Checker *c, Expr *e);

/**
 * @brief 拒绝把流运算符的借用结果留存下来。
 *
 * @param c  检查器。
 * @param e  初始化式、实参或返回表达式。
 * @return   是流借用时已报错并返回 true；否则返回 false。
 */
 bool rejectStreamBorrow (Checker *c, Expr *e);

/**
 * @brief 判断表达式是不是返回引用的流运算符结果。
 *
 * `fin >> x` 交回的是同一个流对象而不是一个新值；移位的运算结果类型是整数，因此不会命中。
 *
 * @param e  被检查的表达式。
 * @return   表达式是 `<` 或 `>` 开头的二元运算且其类型为引用时返回 true。
 */
 bool isStreamBorrow (Expr *e);
/**
 * @brief 检查 `print` 或 `println` 的一个实参，引用在此自动解引用。
 *
 * 打印一个引用只可能意味着打印它指向的值，所以这里是仅存的一处保留自动解引用的值位置。
 *
 * @param c  检查器。
 * @param e  实参表达式。
 * @return   被打印值的类型；实参是引用时返回其指向的类型，并标记该节点需要解引用。
 */
 Type *checkPrintArg (Checker *c, Expr *e);
/**
 * @brief 检查 `e?` 并把失败转发给外围函数。
 *
 * 操作数必须是 `option` 或 `result`；外围函数必须返回同类，`result` 时错误类型还要完全
 * 相同。在 `main` 里且操作数确实带载荷时直接解包，失败即 trap。
 *
 * @param c  检查器。
 * @param e  `?` 表达式。
 * @return   解包后的载荷类型；操作数或外围返回类型不合适时返回错误类型。
 */
 Type *checkTryInner (Checker *c, Expr *e);
/**
 * @brief 在值位置检查表达式，引用必须显式写成 `*p`。
 *
 * `ref x` 与 `alloc` 这类自身就要求引用的表达式保留引用类型；成员选择是导航而不是读值，
 * 因此仍然看穿引用。`@noCopy` 的值在这里被拒绝拷贝。
 *
 * @param c  检查器。
 * @param e  值位置的表达式。
 * @return   值的类型；引用出现在值位置时返回被指向的类型（此时已报错）。
 */
 Type *checkValue (Checker *c, Expr *e);
/**
 * @brief 取变体第 k 个载荷的类型。
 *
 * 枚举是泛型实例时，载荷类型里的类型参数会被替换成类型实参，例如 `maybe<i64>` 中 `just`
 * 的载荷是 `i64` 而不是 `T`。
 *
 * @param tt  类型表，用于做替换。
 * @param et  枚举实例类型；不是实例时载荷类型原样返回。
 * @param v   变体定义。
 * @param k   载荷下标，从 0 起。
 * @return    该位置载荷的类型。
 */
 Type *payloadType (TypeTable *tt, Type *et, Variant *v, size_t k);
/**
 * @brief 用 prelude 里的声明构造 `slice<elem>` 类型。
 *
 * @param c     检查器，持有 prelude 里 slice 的定义。
 * @param elem  元素类型。
 * @return      构造出的视图类型；prelude 里找不到 slice 定义时返回错误类型。
 */
 Type *sliceOf (Checker *c, Type *elem);
/**
 * @brief 把当前实例的类型实参替换进一个类型。
 *
 * @param c  检查器；替换上下文是它的参数表与实参表。
 * @param t  要替换的类型。
 * @return   替换后的类型；没有正在进行的实例化或 t 为空时原样返回 t。
 */
 Type *tsub (Checker *c, Type *t);
/**
 * @brief 取视图类型的元素类型。
 *
 * 视图协议是 `data` 加 `len` 两个成员，这里按名字 `slice` 与恰好一个类型实参识别它。
 *
 * @param t  要检查的类型。
 * @return   元素类型；t 不是单实参的 slice 实例时返回 NULL。
 */
 Type *viewElemOf (Type *t);
/**
 * @brief 按名字在枚举定义里查一个变体。
 *
 * @param td    要查的枚举定义。
 * @param name  变体名。
 * @return      命中的变体；td 为空或没有该名字时返回 NULL。
 */
 Variant *findVariant (TypeDef *td, const char *name);
/**
 * @brief 把表达式读成整数字面量。
 *
 * 一元负号包裹的整数字面量也算，负的切片边界因此能在编译期被拒绝，而不是留到运行时。
 *
 * @param e    要读取的表达式。
 * @param out  读到时写出字面量的值。
 * @return     e 是整数字面量（可以带一元负号）时返回 true 并写出 out；否则返回 false。
 */
 _Bool asIntLit (Expr *e, long long int *out);
/**
 * @brief 判断语句是否总会离开所在的块。
 *
 * 这是识别 `if p == null { return }` 这种守卫的依据：走到 `if` 之后的代码就说明条件不成立。
 * 块取最后一条语句递归判断，空块不算离开。
 *
 * @param s  语句，通常是 `if` 的 then 分支。
 * @return   语句本身是 return、break 或 continue，或块的最后一条如此时返回 true。
 */
 _Bool blockExits (Stmt *s);
/**
 * @brief 检查 `got` 类型的值能否赋到期望 `want` 的位置。
 *
 * 只有无损转换是隐式的：加宽、非空引用转可空引用、丢掉 `mut` 等；其余转换要用户写出来。
 * 这是赋值、实参、字段初始化与载荷值的统一闸门。类型里含类型参数的赋值会被记录下来，
 * 留到实例化时按真实类型复核。
 *
 * @param c     检查器，用于渲染类型与报错。
 * @param want  期望类型。
 * @param got   被赋值表达式的类型。
 * @param node  该表达式，用于取行号与字面量适配。
 * @param what  目标描述（如 "argument"、"field `x`"），出现在诊断里。
 * @return      赋值合法时返回 true；不合法时已报错并返回 false。
 */
 _Bool checkAssignable (Checker *c, Type *want, Type *got, Expr *node, const char *what);
/**
 * @brief 检查值里的引用不会活得比它被放入的存储更久。
 *
 * 先提权值里所有分配点，再用深度比较判定，因为浅到看不出问题时早退会让分配点失去约束。
 * 值类型提及类型参数时记下延迟检查并返回 false，留到实例化时判定。
 *
 * @param c     检查器。
 * @param val   被存储或返回的值。
 * @param at    目标深度；0 表示必须活过本帧。
 * @param line  源码行号，用于诊断。
 * @param what  被检查的操作，出现在诊断里。
 * @return      值会留下悬垂引用时已报错并返回 true；否则返回 false。
 */
 _Bool checkEscape (Checker *c, Expr *val, int at, int line, const char *what);
/**
 * @brief 检查一次存储所需的全部逃逸规则。
 *
 * 深度规则之外还有"借来的值不得存到活过本次调用的地方"。目标深度为 0 且值可追到参数、
 * 目的地又能追到参数时，改由调用点判断寿命。类型提及类型参数时记录延迟检查。
 *
 * @param c       检查器。
 * @param val     被存储的值。
 * @param target  被存入的地方。
 * @param line    源码行号，用于诊断。
 * @return        拒绝这次存储时返回 true；允许时返回 false。
 */
 _Bool checkStoreEscape (Checker *c, Expr *val, Expr *target, int line);

/**
 * @brief 记录一次发布：值被放到层 at 上，本身不做判定。
 *
 * 检查这一趟只记录，之后由一趟折叠结合深度不动点统一求解层号，因此没有判定依赖遍历顺序。
 * 同一个值加同一个目的地若已按不更深的层记录过就跳过。记录挂在 c->stores 上，并带上所在
 * 函数体，供按体折叠使用。
 *
 * @param c       检查器；记录追加到它的发布表。
 * @param val     被发布的值。
 * @param target  目的地；NULL 表示返回值发布。
 * @param at      目标层；0 表示活过本帧。
 * @param line    源码行号，用于诊断。
 */
 void recordStore (Checker *c, Expr *val, Expr *target, int at, int line);

/**
 * @brief 记录一次与层号有关的拒绝，供随后复核。
 *
 * 这里不报错，只把这次判定用到的值、层号与看到的深度留下来。
 *
 * @param c      检查器；记录追加到它的拒绝表。
 * @param val    被拒绝的值。
 * @param at     判定时用的深度。
 * @param depth  检查当时看到的深度。
 * @param line   源码行号。
 */
 void recordLvlRejection (Checker *c, Expr *val, int at, int depth, int line);

/**
 * @brief 用各趟求解后的数字重新核对记录下来的拒绝。
 *
 * 只更新记录里的后算深度并可选打印一行，不改变已经报出的诊断。
 *
 * @param c  检查器。
 */
 void recheckLevelRejections (Checker *c);

/**
 * @brief 把绑定字段表里可达的分配点提升到需要活到的层。
 *
 * 只做提升那一半：记录里带来源的槽才被提权，而且只有提权成功后才允许把该槽的记账降到
 * 目标层。地址被取过的绑定仍然提升（提权只把分配挪到更长命的 arena），只是跳过记账下降。
 *
 * @param c     检查器。
 * @param sy    要走的字段表所属绑定。
 * @param at    容器被存到的深度；0 表示必须活过本帧。
 * @param hops  已经走过的载体步数。
 * @return      每个带来源的字段都提升成功时返回 true；有提升不动的字段时返回 false，
 *              由调用者报深度错误。
 */
 bool promoteFieldsAt (Checker *c, Sym *sy, int at, int hops);
/**
 * @brief 判断 C 能否直接比较该类型的值。
 *
 * 数字、`bool` 与无载荷枚举（在 C 里就是整数）可以直接比较；`str` 被排除，因为它的 `==`
 * 会退化成指针比较；带载荷枚举在 C 里是标记结构体，结构体没有 `==`。
 *
 * @param t  要判断的类型。
 * @return   true 表示 C 的 `==` 比较的是值而不是地址。
 */
 _Bool cmpIsNative (Type *t);
/**
 * @brief 判断枚举是否至少有一个变体带载荷。
 *
 * @param td  枚举定义。
 * @return    true 表示存在带载荷的变体；td 为空或所有变体都不带载荷时返回 false。
 */
 _Bool enumHasPayload (TypeDef *td);
/**
 * @brief 判断名字是不是比较运算符。
 *
 * @param op  运算符名。
 * @return    `==`、`!=`、`<`、`<=`、`>`、`>=` 之一时返回 true。
 */
 _Bool isCmpOp (const char *op);
/**
 * @brief 判断名字是不是相等运算符。
 *
 * `==` 与 `!=` 共享同一条原生比较规则，`!=` 也会改用 `==` 取反。
 *
 * @param op  运算符名。
 * @return    `==` 或 `!=` 时返回 true。
 */
 _Bool isEqualityOp (const char *op);
/**
 * @brief 判断名字是不是类型可以重复定义的重载运算符。
 *
 * 可重载集合就是语言的运算符集合，也是解析器允许作为方法名的集合；集中在一处判断，
 * 解析器、重复定义检查与查找就不会各说各话。
 *
 * @param name  方法名。
 * @return      比较运算符、算术运算符与流运算符 `<<`、`>>` 时返回 true。
 */
 _Bool isOverloadableOp (const char *name);
/**
 * @brief 判断名字是不是可重载的算术运算符。
 *
 * `-` 只指二元减法：extC 没有一元运算符重载，名为 `-` 的方法只由 `a - b` 触及。
 *
 * @param op  运算符名。
 * @return    `+`、`-`、`*`、`/`、`%` 之一时返回 true。
 */
 _Bool isArithOp (const char *op);
/**
 * @brief 判断名字是不是逻辑运算符。
 *
 * @param op  运算符名。
 * @return    `&&` 或 `||` 时返回 true。
 */
 _Bool isLogicOp (const char *op);
/**
 * @brief 判断能否对这个表达式取地址。
 *
 * @param e  表达式。
 * @return    表达式是绑定、字段或 `*p`（即 `p` 指向的地方）时返回 true。
 */
 _Bool isLvalue (Expr *e);
/**
 * @brief 判断某个生成名在当前位置已被证明非空。
 *
 * 非空证明跟着绑定而不是源码里写的名字走，所以查询用绑定的生成 C 名。
 *
 * @param c      检查器，证明表在它里面。
 * @param cname  绑定的生成 C 名。
 * @return       证明仍然有效时返回 true；cname 为 NULL 或从未被证明时返回 false。
 */
 _Bool isNarrowed (Checker *c, const char *cname);
/**
 * @brief 判断表达式是不是整数或浮点字面量。
 *
 * @param e  表达式；必须非 NULL。
 * @return   EX_INT 或 EX_FLOAT 时返回 true，这两类字面量的类型由上下文决定。
 */
 _Bool isNumericLit (Expr *e);
/**
 * @brief 判断表达式是不是一个地方。
 *
 * 地方是绑定、字段、下标与切片组成的链。切定长数组要求操作数是地方，因为要取元素地址，
 * 否则会切到返回值的临时存储；切字符串字面量例外，它的字节是静态存储期的。
 *
 * @param e  表达式。
 * @return   链的根是绑定、每一步都是字段、下标或切片时返回 true，否则返回 false。
 */
 _Bool isPlace (Expr *e);
/**
 * @brief 判断该类型的值能否交给 `print` 或 `println`。
 *
 * 结构体、泛型实例与数组由编译器生成的 `_debug` 帮助函数递归打印；无载荷枚举总有名字可印。
 *
 * @param t  实参类型。
 * @return   存在打印方式时返回 true；t 为空或类型不受支持时返回 false。
 */
 _Bool isPrintable (Type *t);
/**
 * @brief 判断类型是不是名字与实参个数都匹配的 prelude 容器。
 *
 * @param t      要判断的类型。
 * @param name   保留的容器名，如 `option`、`result`。
 * @param nargs  要求的类型实参个数。
 * @return       泛型结构体或泛型枚举的声明名等于 name 且类型实参个数等于 nargs 时返回 true。
 */
 _Bool isProtoType (Type *t, const char *name, size_t nargs);
/**
 * @brief 判断能否通过这个表达式写入。
 *
 * 写权限有三个来源：绑定看 `var` 还是 `let`，参数看类型是 `mut ref` 还是 `ref`，字段与
 * 元素看根。`*p` 单独判断，权限来自引用类型本身；只读视图也挡写。
 *
 * @param c  检查器。
 * @param e  地方表达式；NULL 不可写。
 * @return   允许写入时返回 true；路径跨过只读引用、视图只读或根是 `let` 时返回 false。
 */
 _Bool isWritablePlace (Checker *c, Expr *e);
/**
 * @brief 判断字面量的值能否放进目标类型。
 *
 * 字面量在上下文给它类型之前没有自己的类型，所以 `3.14` 在 `f32` 上下文里成为 `f32`，
 * `200` 在 `u8` 上下文里成为 `u8`。无符号目标拒绝负的整数字面量；错误类型接受任何字面量，
 * 以免一条诊断级联出更多。
 *
 * @param e     字面量：EX_INT、EX_FLOAT 或 EX_BOOL。
 * @param want  上下文期望的类型。
 * @return      字面量的值在 want 里可表示时返回 true。
 */
 _Bool literalFits (Expr *e, Type *want);
/**
 * @brief 判断类型是否直接或间接提及类型参数。
 *
 * @param t  要检查的类型。
 * @return   类型本身是 TY_PARAM，或内部含类型参数时返回 true。
 */
 _Bool mentionsParam (Type *t);
/**
 * @brief 判断通往这个地方的路径上跨过的引用是否都是 `mut ref`。
 *
 * 跨过只有两种方式：显式地经过 `*p`，或 `p.f`、`v[i]` 里 `p` 是引用从而隐式落在它指向的
 * 存储上。目标自身的类型不算在内，所以重新指向一个绑定仍可写。
 *
 * @param e        地方表达式。
 * @param crossed  出参：存储位于跨过的引用之后时置 true。
 * @return         每个跨过的引用都是 `mut ref` 时返回 true，遇到只读引用时返回 false。
 */
 _Bool pathRefsAllMut (Expr *e, _Bool *crossed);
/**
 * @brief 判断通往这个地方的路径是否跨过或落在只读引用上。
 *
 * 与 `pathRefsAllMut` 问的不是同一个问题：这里还把地方自身的类型算进来，例如 `c.bump()`
 * 里 `c` 是 `ref counter`。两个答案都要，因为诊断措辞不同。
 *
 * @param e  地方表达式。
 * @return   地方自身的类型或路径上任一引用是只读引用时返回 true。
 */
 _Bool pathHasReadonlyRef (Expr *e);
/**
 * @brief 在没有非空证明时拒绝解引用可空引用。
 *
 * 可空引用不能直接解引用，必须先与 null 比较，让证明落在语法里而不是靠推断。
 *
 * @param c     检查器。
 * @param t     被解引用的类型。
 * @param node  该表达式，用于取行号。
 * @param what  被检查的操作，出现在诊断里。
 * @return      t 是可空引用时已报错并返回 true；否则返回 false。
 */
 _Bool rejectNullableDeref (Checker *c, Type *t, Expr *node, const char *what);
/**
 * @brief 判断重复求值这个表达式是否观察不到区别。
 *
 * `??` 生成的条件表达式里主体出现两次，有副作用的主体就会跑两遍。判据是保守的语法判断：
 * 含有调用、`?` 或 `??` 的表达式一律拒绝，而不是去推断纯度。
 *
 * @param e  要检查的表达式。
 * @return   表达式只由字面量、绑定、字段、下标、切片与一元、二元运算组成时返回 true。
 */
 _Bool repeatablePure (Expr *e);
/**
 * @brief 检查一个地方是否允许被写入。
 *
 * 三种原因各自有诊断：写 `*p` 需要 `mut ref`；路径跨过只读引用或只读视图需要类型带 `mut`；
 * 重新赋值 `let` 绑定直接被禁。
 *
 * @param c     检查器。
 * @param e     被写入的地方。
 * @param line  源码行号，用于诊断。
 * @param what  被检查的操作，出现在诊断里。
 * @return      写入被拒绝且已报错时返回 true；允许写入时返回 false。
 */
 _Bool requireMutable (Checker *c, Expr *e, int line, const char *what);
/**
 * @brief 判断类型能否携带引用。
 *
 * 直接是引用、数组元素、任一变体的载荷或结构体字段里含引用都算。递归有深度上限，超过时
 * 保守地答"是"。泛型实例先替换类型实参再走，否则 `T` 看起来无害而检查会漏过。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型；NULL 不携带引用。
 * @return    true 表示类型的任何部分都能持有引用。
 */
 _Bool typeContainsRef (TypeTable *tt, Type *t);
/**
 * @brief 判断类型是否就是函数类型或内部含有函数类型。
 *
 * 与 `typeContainsRef` 是同一族的走法，存在只为一处诊断的措辞：`fn(i64) -> i64` 没有零值，
 * 但原因不是引用。递归同样有深度上限，超过时保守地答"是"。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型；NULL 不含函数类型。
 * @return    true 表示类型本身是 TY_FN，或数组、任一变体载荷、结构体字段里有函数类型。
 */
 _Bool typeContainsFn (TypeTable *tt, Type *t);
/**
 * @brief 判断类型是否没有零值。
 *
 * 非空引用与函数类型都没有零值（后者清零会得到空函数指针），含它们的聚合也没有。带载荷
 * 枚举只看第一个变体，因为它的零值是 tag 0 加载荷清零，所以变体顺序有意义。递归有深度
 * 上限，超过时保守地答"是"。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型。
 * @return    true 表示把该类型清零会产生空引用或空函数指针。
 */
 _Bool typeLacksZeroValue (TypeTable *tt, Type *t);
/**
 * @brief 判断该类型的值能否用运算符 op 参与运算。
 *
 * 原生规则按族分别判定：`==`、`!=` 对数字、`bool` 与无载荷枚举原生，序比较只对数字原生，
 * 算术对数字原生而 `%` 只对整数。数组的 `==` 由编译器派生，但要求右操作数是同一个数组
 * 类型且元素可比较；其余情况查找按操作数类型精确匹配的运算符方法。
 *
 * @param tt   类型表，用于替换泛型接收者的实参。
 * @param t    运算符作用的类型。
 * @param op   运算符名，如 `==`、`<`、`+`、`<<`。
 * @param rhs  右操作数的类型。
 * @return     true 表示原生支持，或该类型上定义了匹配的运算符方法。
 */
 _Bool typeSupportsOp (TypeTable *tt, Type *t, const char *op, Type *rhs);
/**
 * @brief 给源名字分配它在生成 C 里的名字。
 *
 * `let` 只给值起名而不分配存储，所以同一作用域里第二个同名 `let` 在生成的 C 里被改名，
 * 源码里的名字始终不变。同一函数内后缀单调递增且不复用；模块级名字与 C 关键字都要避开，
 * 否则局部会在生成的 C 里遮住函数名或关键字。返回的字符串活在检查器的 arena 里。
 *
 * @param c     检查器，持有每函数的名字使用计数。
 * @param name  源码里写的名字。
 * @return      生成 C 里使用的名字；第一次使用返回原名字。
 */
 const char *cNameFor (Checker *c, const char *name);
/**
 * @brief 取条件所证明非空的绑定名。
 *
 * `p != null` 在真分支证明成立，`p == null` 在假分支成立，后者正是把
 * `if p == null { return }` 变成守卫的依据。字段路径 `h.p` 只在根是未被取址的局部时成立，
 * 否则别名可能改掉它。
 *
 * @param c        检查器。
 * @param cond     要检查的条件表达式。
 * @param whenTrue 出参：证明在真分支成立时置 true，在假分支成立时置 false。
 * @return         绑定的生成 C 名，或字段路径键 `root.field`；条件证明不了任何东西时 NULL。
 */
 const char *narrowTarget (Checker *c, Expr *cond, _Bool *whenTrue);
/**
 * @brief 把类型渲染成用户书写的源码文本，用于诊断。
 *
 * lambda 的类型是生成的闭包结构体，用户写的是签名，所以诊断里显示签名。
 *
 * @param c  检查器，文本分配在它的 arena 里。
 * @param t  要渲染的类型；NULL 渲染成 `void`。
 * @return   以 NUL 结尾的字符串，在本次运行的余下时间里一直有效。
 */
 const char *typeStr (Checker *c, Type *t);
/* Check a whole module: the declarations, the function bodies, the deferred rules, and the arena
 * decisions that wait for closed effect summaries. */
/**
 * @brief 判断某个局部名是否在逃逸集合里。
 *
 * 逃逸集合 E 是"可能离开本帧"的局部名字的过近似，关成不动点；多一个名字只多花内存，
 * 少一个才会留下悬垂指针。
 *
 * @param c  检查器；逃逸集合就是它的 escapees。
 * @param n  绑定名。
 * @return   名字在集合里时返回 true；n 为 NULL 或不在时返回 false。
 */
 bool isEscapeeName (Checker *, const char *);
/**
 * @brief 传递地闭合函数的效应摘要。
 *
 * 摘要只在真正需要时计算并缓存；正在计算中的函数说明遇到环，标记为不完整；无法解析的
 * 调用让摘要永远不完整。外部声明按签名收集到的摘要视为完整，不因没有函数体而变空。
 * 只有返回 true 的摘要才能用来跳过检查。
 *
 * @param c  检查器。
 * @param f  要闭合并缓存的函数；NULL 不可用。
 * @return   摘要完整时返回 true；成环、有未解析调用或被调方不完整时返回 false。
 */
 bool computeEffectsTransitive (Checker *, FuncDef *);
/**
 * @brief 取绑定字段表里某个字段的深度槽。
 *
 * 字段表每个绑定至多四个槽，装不下的写进 `otherDepth` 做保守兜底。槽里存的名字来自 AST，
 * 生命周期长于绑定记录，因此指针不会悬垂。
 *
 * @param c       检查器；未使用，保留 checker 参数约定。
 * @param s       要查的绑定。
 * @param field   字段名。
 * @param create  槽不存在时是否新建。
 * @return        指向该字段深度槽的指针；create 为 false 或表已满时返回 NULL。
 */
 int *fieldDepthEntry (Checker *, Sym *, const char *, bool);
/**
 * @brief 写/读字段槽用的路径键（根绑定到这次投影的整条字段链）。
 *
 * 键字符串建在检查器的 arena 里：`bx.mid.next` 记成 `mid.next`，元素下标是常量时写进名字
 * （`arr[0]` 记 `[0]`）。字段表的读与写必须用同一个键，否则两层投影的来源记不下、提不动。
 *
 * @param c  检查器。
 * @param e  投影表达式（`EX_FIELD` / `EX_INDEX` 链）。
 * @return   路径键；命不了名（含解引用/切片步进）时返回 NULL。
 */
 const char *fieldSlotKey (Checker *, Expr *);
/**
 * @brief 记录一次写到绑定的字段或元素上的深度。
 *
 * 字段名非空时更新对应槽，地址被取过的绑定只能取最大值，字段表完整时可以强更新；字段表
 * 不完整时不允许任何下降。元素写与整值写走 `otherDepth`；引用型绑定自身指向的深度只抬
 * 不降，以免"我指向谁"被"我指向的对象里有什么"覆盖。
 *
 * @param c      检查器；被写入的表达式放在 c->curStoreVal 上。
 * @param root   字段表被更新的绑定；可为 NULL。
 * @param field  字段名；NULL 表示整值写或元素写。
 * @param d2     被写入值的深度。
 */
 void noteFieldDepthWrite (Checker *, Sym *, const char *, int);
/**
 * @brief 记录一次整值赋值，并保留它的逐字段来源。
 *
 * 目的地未被取址、来源是标识绑定且其字段表完整时，整张字段表原样复制过来，之后仍能从容器
 * 走到每个字段的分配点；否则退化成保守的整值更新。
 *
 * @param c      检查器。
 * @param dst    目的地绑定。
 * @param value  来源表达式。
 * @param d2     这次写入的深度。
 */
 void noteWholeValueDepthWrite (Checker *, Sym *, Expr *, int);
/**
 * @brief 取一个地方表达式根部的标识符名字。
 *
 * 只沿字段、下标与解引用走到根标识符，不解析也不查作用域，所以未解析的节点也能用；这正是
 * 它与 `placeRoot` 的关键差别。
 *
 * @param e  地方表达式；可为 NULL。
 * @return   根部标识符的名字；表达式不是这样的链时返回 NULL。
 */
 const char *placeRootName (Expr *);
/**
 * @brief 由 `mut ref` 实参算出这次调用要传的 arena 深度。
 *
 * 被调方分配的东西至少要活到它可能存入的对象那么久，所以取最浅的 `mut ref` 实参所指对象的
 * 深度。实参是本函数参数或全局（深度 0），或是逃逸集合里的局部时，改传调用者的 home arena。
 * 答案依赖逃逸集合的站点记录到 eSites，留给摘要闭合后的最后一趟重算。
 *
 * @param c         检查器。
 * @param args      按调用顺序排列的实参表达式。
 * @param params    被调方声明的参数，顺序相同。
 * @param callNode  调用表达式；为 NULL 时不记录站点。
 * @return          要传的 arena 深度；没有 `mut ref` 实参时返回 0，要传调用者 home arena 时
 *                  返回 -1。
 */
  int callHomeDepth (Checker *c, Vec *args, Vec *params, Expr *callNode);
/**
 * @brief 算出一次存储要记录的值的引用深度。
 *
 * 取引用深度与结构上界的较大者，保证携带活指针的值不会被低估。只有类型可能携带引用时才
 * 看结构上界，因此纯值拷贝不受来源对象寿命的约束，容器存字节时不会把分配点拖到调用者
 * 的 arena 里。
 *
 * @param c  检查器。
 * @param e  被存储的值。
 * @return   被存储的字节可能携带的引用深度的上界。
 */
int valDepthForStore (Checker *, Expr *);
/**
 * @brief 算出值所携带的引用指向的存储有多深。
 *
 * 数字越小活得越久，0 表示参数或全局，也就是本帧之外。这个数是引用规则拿来与目标存储深度
 * 比较的量，所以每个近似都必须偏大：偏大只会误拒，偏小就会放过悬垂引用。结果可能缓存在
 * 节点上，但类型提及类型参数的节点不缓存。
 *
 * @param c  检查器。
 * @param e  要检查的表达式。
 * @return   值里引用的深度；类型不可能携带引用时返回 0。
 */
 int targetDepth (Checker *c, Expr *e);
/**
 * @brief 算出地方表达式所指存储的深度。
 *
 * 局部给它的块深度，参数与全局给 0（本帧之外），引用型绑定给的是它指向的对象的深度而不是
 * 自己槽位的深度；`*p` 不创造存储，取的是引用本身的深度。根部无法命名的形状退回 `targetDepth`，
 * 使兜底落在更长寿的一侧，而不是把深度记成 0。
 *
 * @param c  检查器，提供作用域栈与类型表。
 * @param e  地方表达式。
 * @return   存储的词法深度；e 为 NULL 时返回 0。
 */
 int slotDepth (Checker *c, Expr *e);
/**
 * @brief 判断表达式的存储是否可证明不在任何帧里。
 *
 * 只有两个来源：根是全局的名字链（静态存储活过每一帧），以及声明带 `effects Ret=0` 的调用
 * （"我返回的引用不指向调用者的帧"）。其余一律答否，保持保守规则：`new`、arena 分配、栈上
 * 数组或没有这样声明的被调方都可能随帧消失。
 *
 * @param c  检查器。
 * @param e  绑定的初始化式或右值。
 * @return   存储确定活过本帧时返回 true，否则返回 false。
 */
 bool exprOutOfFrame (Checker *c, Expr *e);
/**
 * @brief 判断一个初始化表达式是不是常量表达式。
 *
 * 只看表达式的形状：字面量、取反、字面量之间的算术、枚举常量、所有字段都是常量的
 * 结构体字面量、函数地址、`null`，以及无需运行时检查的转换都算常量。
 *
 * @param e  被判断的初始化表达式；可以为 NULL。
 * @return   常量表达式返回 `true`；NULL 与其它形状返回 `false`。
 */
 bool isConstInit (Expr *e);
/**
 * @brief 求一个写目标所在存储的 arena 层。
 *
 * 它与 `slotDepth` 回答的不是同一个问题：这里问的是存储本身活多久，而不是引用指向的
 * 对象活多久。
 *
 * @param c  检查器。
 * @param e  写目标的位置表达式。
 * @return   存储所在的层：局部绑定是它所在块的深度，复制进本帧的参数是 1，全局是 0；
 *           投影自引用或参数的位置退回 `slotDepth` 的答案。
 */
 int storeLayer (Checker *c, Expr *e);
/**
 * @brief 判断一个值的根是不是来自某个函数的参数。
 *
 * 是的话，生命周期义务就交给调用点，由它知道对应实参活多久。
 *
 * @param c    检查器；本判断用不到，保留是为了与同类谓词形状一致。
 * @param f    提供参数名单的函数定义。
 * @param val  要追溯的值。
 * @return     值的根是 `f` 的参数名时返回 `true`；值为 NULL、函数为 NULL 或根无法命名时返回 `false`。
 */
 _Bool valTracesToParam (Checker *c, FuncDef *f, Expr *val);
/**
 * @brief 把值内部的每个分配站点提升到它必须达到的层。
 *
 * 只沿值的载体走（绑定、`!`、`??`、结构体字面量、枚举负载、数组字面量），不追别名、
 * 不做跨函数推理，最多 32 步。提升只把生命周期拉长，不会让不安全的存储变成接受。
 *
 * @param c    检查器。
 * @param val  被存储或返回的值。
 * @param at   值必须达到的层；0 表示要活过本帧。
 * @return     值里每个分配站点都到达 `at` 时返回 `true`；有载体提不动时返回 `false`，
 *             调用方据此退回深度判据报错。
 */
 _Bool promoteInto (Checker *c, Expr *val, int at);
/**
 * @brief 记下一个绑定值的来源，并把载体链展平到根。
 *
 * 来源必须展平：链中间的绑定可能在后续再被追溯时已经出了作用域。已经取过地址的绑定不记。
 *
 * @param c   检查器。
 * @param sy  被赋值或初始化的绑定。
 * @param val 要记来源的表达式。
 */
 void noteOrigin (Checker *c, Sym *sy, Expr *val);
/**
 * @brief 求一个值的根来源（把绑定链压平到根表达式）。
 *
 * 写进字段表的来源用它压平：中间绑定到提权那一刻可能已经出了作用域，到时 `lookup` 查不到，
 * 来源链就断了、站点也就提不动。根的表达式（`new` 之类）没有作用域问题。
 *
 * @param c    检查器。
 * @param val  要求来源的表达式。
 * @param hops 已走过的绑定步数；超过上限停止。
 * @return     根表达式；没有可追的来源时就是 `val` 本身。
 */
 Expr *originOf (Checker *c, Expr *val, int hops);
/**
 * @brief 给说不出自己类型的表达式套上上下文期望的类型。
 *
 * 只处理裸 `null`、数组字面量和没有限定名的结构体字面量，并且只在上下文唯一确定类型时
 * 生效；`null` 只接受可空引用的上下文，因为它无从知道目标类型。
 *
 * @param e     要补类型的表达式。
 * @param want  上下文期望的类型。
 */
 void adoptContextType (Expr *e, Type *want);
/**
 * @brief 检查一次调用的引用实参能否活过这次调用，并据此定下 arena 的层。
 *
 * 实参与形参个数不一致时直接返回。extern 被调者是 C 侧的黑盒：按签名里的 effects 位
 * 逐位判断哪个参数可能被保存，被保存的引用实参必须活到层 0。有函数体的被调者先做
 * 效果闭包，再分别判地址流与内容流，两个判据各自决定是否运行；可提升的值先尝试提升，
 * 提不动才报错。`mut ref` 实参是写权限，其非空证明在这里作废。
 *
 * @param c          检查器。
 * @param callee     被调函数定义；NULL 表示被调者未解析，按保守处理。
 * @param args       实参表达式表。
 * @param params     被调函数的形参表，顺序与 `args` 一致。
 * @param homeDepth  被调者声明的 arena 层；小于等于 0 表示没有显式声明。
 * @param line       报错用的源码行。
 * @param fname      被调者的名字，用于诊断。
 */
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
/**
 * @brief 在定义处检查运算符方法的签名。
 *
 * 只检查可重载集合里的比较与算术运算符（含 `<<` 与 `>>`），其它名字一律放过。要求恰好
 * 两个参数、第一个参数是所属类型的引用；比较运算符必须返回 `bool`，算术运算符必须返回
 * 所属类型的值，只有 `<<` 与 `>>` 允许返回该类型的引用。
 *
 * @param c  检查器。
 * @param f  被检查的函数定义。
 */
 void checkOperatorSig (Checker *c, FuncDef *f);
/**
 * @brief 检查一条语句并落实它带来的深度与收窄后果。
 *
 * 进入时清零本语句的副作用计数，然后按语句种类分派：声明解析注解并登记绑定，赋值走
 * 写点入口，`if` / `while` 记录条件证明的事实并检查分支，`match` 逐臂开作用域，块与
 * domain 块各走自己的作用域路径。
 *
 * @param c  检查器。
 * @param s  要检查的语句。
 */
 void checkStmt (Checker *c, Stmt *s);
/**
 * @brief 把签名里的类型名就地解析到类型表。
 *
 * 在任何函数体被检查之前运行，因此之后的检查不必再解析类型名。方法看到所属类型的
 * 类型参数，自由函数看到自己的，类型名放在声明所在文件的上下文里解析。返回类型属于
 * `void` 时置为 NULL。
 *
 * @param c  检查器。
 * @param f  参数与返回类型要就地解析的函数。
 */
 void resolveSignature (Checker *c, FuncDef *f);
/**
 * @brief 在一个源码行上报告一个致命编译错误。
 *
 * 消息先渲染进固定大小的缓冲区，过长时截断而不是溢出。报告遵循"首个错误为准"：一旦
 * 错误标志置起，后续错误被丢掉，避免连锁诊断涌向用户。
 *
 * @param c     收集诊断的检查器。
 * @param line  诊断指向的源码行；没有已知位置时为 0。
 * @param note  消息下额外打印的一句说明，或 NULL。
 * @param fmt   消息的 printf 格式。
 * @param ...   `fmt` 的格式参数。
 */
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
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
 void ckWarn  (Checker *c, int line, const char *note, const char *fmt, ...);
/**
 * @brief 解析复合赋值的运算符，返回 `target op value` 的类型。
 *
 * 语句保持书写原样，运算交给 `x + y` 会走的同一个算术检查，因此算术规则、字面量适配、
 * 引用拒绝与用户自定义运算符都只有一处答案。用户自定义运算符把接收者当引用，生成的
 * 调用要把目标再命名一次，所以目标里含调用时报错并提示拆成两行。
 *
 * @param c       检查器。
 * @param op      书写形式的复合运算符；首字符就是对应的二元运算符。
 * @param target  已经检查过的左侧目标。
 * @param value   已经检查过的右侧值。
 * @param tgt     目标的类型。
 * @param val     值的类型。
 * @param out     接收解析出的 `EX_BIN` 节点。
 * @return        `target op value` 的类型；运算符不适用或目标含调用时返回错误类型。
 */
 Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                       Type *tgt, Type *val, Expr **out);
/**
 * @brief 把裸的构造器名就地改写成限定变体构造。
 *
 * 只认 `success` / `failure` / `some` / `none` 四个名字，期望类型必须是对应的 `result`
 * 或 `option` 实例；用户定义了同名函数时名字原样保留。`none` 是唯一可以省略括号的。
 *
 * @param c     检查器。
 * @param e     要改写的表达式；不是上述形状时原样不动。
 * @param want  期望的类型，用来判断该名字是不是已知构造器。
 */
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
/**
 * @brief 要求条件表达式的类型是 `bool`，否则报错。
 *
 * extC 没有隐式真值：整数或引用当条件用会被报出，而不是被转换。
 *
 * @param c     检查器。
 * @param t     条件的类型。
 * @param node  条件表达式，用于取源码位置。
 */
 void expectBool (Checker *c, Type *t, Expr *node);

/* Mark the call as allocating into the caller's home arena when the value it produces escapes
 * the current frame. */
/* ---------------------------------------------------------------- 写点：一个入口
 *
 * 一次写要同时做四件事，**顺序不能变**：
 *   ① `recordStore`  —— 记下"这个值被发布到了 `at` 这一层"（电平求解器据此折叠）
 *   ② `promoteInto`  —— 试着把值背后的分配站点提到那一层（提不动就保持保守）
 *   ③ `markCallHomeIfEscaping` —— 逃逸集合相关的那一半
 *   ④ 字段表          —— 整值写走 `noteWholeValueDepthWrite`，字段/元素写走 `noteFieldDepthWrite`
 *                        （后者内部会记 `src`，让后续提权找得到站点）
 *
 * **能走这个入口的写一律走它**；只有语义确实不同的（返回值、实参发布、证明失效 `unNarrow`、
 * fresh 集合失效 `freshDrop`）才自己组合，并在调用点写明理由。*/
/* ---------------------------------------------------------------- 写点失效：一个概念，两处实现
 *
 * 一次写不只"发布"，还要让**关于它的导出事实失效**。这类事实活在两个 pass 里，
 * 所以"统一"在这里只能是"一个概念 + 两处实现各自点名"，而不是硬合成一个函数：
 *
 *   | 事实 | 谁让它失效 | 活在哪个 pass | 不失效的后果 |
 *   |---|---|---|---|
 *   | 非空证明（`?ref T` 已证非空，含字段路径 `h.p`） | `unNarrow(c, cname)` | 检查期（`Checker.narrow` 栈） | `mut ref` 方法调用后证明还在 ⇒ 生成物解引用 NULL |
 *   | fresh 集合（这个名字装着本帧新分配） | `freshDrop(fresh, name)` | 摘要期（摘要用的局部 `Vec`） | 重指向后集合还在 ⇒ 摘要说"天然安全" ⇒ 调用点整段检查被跳过 |
 *
 * 两处的**触发条件不同**，都写在各自的定义处，这里只记共同形状：
 *   "让这个根（以及经由它的路径）不再有任何仍然成立的事实"。
 * 新增同类事实时，请在这张表里加一行，并在**同一个写点**触发，让"写"与"失效"成对出现。*/

/**
 * @brief 一次写的统一入口：发布、提权、逃逸标记与字段表按固定顺序完成。
 *
 * 顺序不能变：`recordStore` 要在 `promoteInto` 之前，求解器先看到要求；字段表要在提权
 * 之后，因为字段表的提升会读它。整值写走 `noteWholeValueDepthWrite`，字段或元素写走
 * `noteFieldDepthWrite`；不涉及引用的写直接返回。
 *
 * @param c       检查器。
 * @param value   被写入的值。
 * @param target  写入的目标位置；没有目标时只做发布与提权。
 * @param at      目标所在的 arena 层；0 表示要活过本帧。
 * @param line    源码行，用于诊断。
 */
 void noteStore (Checker *c, Expr *value, Expr *target, int at, int line);
/**
 * @brief 按层 0 发布一个返回值。
 *
 * 目标就是值本身：交回给调用方的东西一律按"活过本帧"发布。两个返回站点共用这一半，
 * 各自的后续步骤不同。
 *
 * @param c      检查器。
 * @param value  被返回的值。
 * @param line   源码行，用于诊断。
 */
 void noteReturnPublish (Checker *c, Expr *value, int line);

/**
 * @brief 当调用结果逃出当前块时，把这次调用标成走调用方的家 arena。
 *
 * 只处理有具体被调者、且被调者需要家 arena 的调用。目的地比当前作用域浅（`at` 小于
 * 作用域数）时 `homeDepth` 记 -1，否则记当前块的深度；随后立刻调用 `setCallArenaArg`，
 * 让 `homeDepth` 与实际传的 arena 两格保持一致。
 *
 * @param c   检查器，读取作用域栈。
 * @param v   结果被存储的调用表达式；其它形状忽略。
 * @param at  接收结果的位置的作用域深度；0 表示结果被返回。
 */
 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
/**
 * @brief 把调用的 `homeDepth` 解析成它实际要传的 arena 实参。
 *
 * `homeDepth` 为 -1 时传家 arena；大于等于 1 时传对应块的 arena；否则没有 `mut ref`
 * 实参给出理由，先按当前块记下并置 `arenaArgPending`，等末轮在所有函数体检查完之后
 * 再定。codegen 只读这里写下的结果。
 *
 * @param c  检查器，读取作用域栈。
 * @param e  已经设好 `homeDepth` 的调用表达式。
 */
 void setCallArenaArg (Checker *c, Expr *e);

/**
 * @brief 定下这次建池的调用该用哪个 zone 层。
 *
 * 在库函数里（`modName` 非空）库对"地方"透明，选择权交给调用者（`ZONE_HOME`）；在入口
 * 文件的函数里默认用当前块。只往下调不往上抬：同一个节点可能被检查不止一次，而提权已经
 * 把这一格改小了。
 *
 * @param c  检查器，读取当前函数与作用域栈。
 * @param e  调用表达式。
 */
 void setCallZoneArg (Checker *c, Expr *e);

/**
 * @brief 判断被调者产出的结果会不会住在池上。
 *
 * 问的是类型的性质：声明了 `@poolObject` 的类型拥有池，闭包算出的 `makesPool` 与按名字
 * 认出的池构造 extern 也算。泛型方法的模板与实例是两份 `FuncDef`，所以关联函数那一族
 * 用结构体上的标记放宽。
 *
 * @param f  被调函数定义；NULL 视为否。
 * @return   所属类型标了 `@poolObject`、闭包查到该函数建池、函数没有函数体且名字属于池
 *           构造那一族，或它是建池类型的关联函数时返回 `true`；否则 `false`。
 */
 bool calleeMakesPool (FuncDef *f);
/* 运行期**建池**的那一族按名字认（extern 没有函数体，效果推断看不穿它）。规则**只有这一处**：
 *   · `calleeMakesPool` / `calleeCreatesPool` 问"这一次调用会不会建池" ⇒ 全族；
 *   · codegen 的改写点只给**不带 zone 参数**的那两个补 home zone（`_at` 自己带着）。
 * 漏登记一个名字的后果：`extc_pool_new_table` 拿不到 zone ⇒
 * `extc_pool_new_at(parent, extc_zoneTop)` 撞上 `extc_zoneTop == -1` ⇒ **返回 -1**。 */

/* The pool primitives form **one list with one spelling**: the checker validates their arity
 * and code generation lowers them by hand, both keyed on the same names. Adding a name on one
 * side only would leave the other reading arguments it never checked and re-deriving an
 * arity of its own. */

/**
 * @brief 判断这一次调用会不会走到 `extc_pool_new`。
 *
 * 与"结果会不会住在池上"是两个问题：这里只问被调者这一侧会不会建池，供块级 zone 钩子
 * 使用。未解析的被调者按保守为真处理。
 *
 * @param f  被调函数定义；NULL 表示被调者未解析。
 * @return   闭包查到该函数建池、函数没有函数体且名字属于池构造那一族，或它是建池类型的
 *           关联函数时返回 `true`；NULL 返回 `true`，其余返回 `false`。
 */
bool calleeCreatesPool (FuncDef *f);

/**
 * @brief 装上"由表达式解析出被调者"的钩子。
 *
 * 泛型体里对类型参数的协议方法在节点上故意没有记录被调者（一份模板体、多份实例），
 * 钩子由实例那一轮代入类型实参把它找回来；没有装钩子时答案保持保守。
 *
 * @param fn  解析函数；传 NULL 表示撤掉钩子。
 */
 void setPoolCalleeResolver (FuncDef *(*fn)(Expr *e));

/**
 * @brief 记下"这个值必须装进某一层"这条电平事实。
 *
 * 记在站点而不是绑定上：重放跑在所有函数体检查完之后，那时局部绑定已查不到。值是标识符
 * 时换成它的来源站点或建池站点。电平求解过程中不记，免得事实表无界增长。
 *
 * @param c    检查器；记录追加到 `c->lvlFacts`。
 * @param val  被存储或返回的值。
 * @param at   值必须装进的层；负值忽略。
 */
 void recordLvlFact (Checker *c, Expr *val, int at);
/**
 * @brief 在定义处检查构造器的签名。
 *
 * 只检查所属类型里名为 `new` 的关联函数。返回类型必须是所属类型本身，或者是首个类型
 * 实参为所属类型的 `result<T, E>`（可失败的构造器）；否则在定义处报错，即使还没有人调用它。
 *
 * @param c  检查器。
 * @param f  被检查的函数定义。
 */
 void checkCtorSig (Checker *c, FuncDef *f);

/**
 * @brief 当使用了别的模块的名字而没有写模块限定时报错。
 *
 * 同一文件、本模块与自动导入的 prelude 豁免；`use mod::*` 或 `use mod::{name}` 已把名字
 * 带进当前模块作用域时也放行。`@private` 仍在声明处拦截，打开模块不是绕过它的办法。
 *
 * @param c          检查器。
 * @param what       被引用的名字。
 * @param whatMod    拥有这个名字的模块。
 * @param qualified  这次引用是否来自限定名；限定名在加载时被改写成扁平名，这个标志是
 *                   书写方式的唯一痕迹。
 * @param line       报错用的源码行。
 */
 void requireQualified (Checker *c, const char *what, const char *whatMod, bool qualified, int line);
/**
 * @brief 判断某个 import 是否已把一个名字带进当前模块的作用域。
 *
 * 通配条目覆盖该模块的所有公开名字，列名条目只覆盖它写出的那一个。比较按加载器记下的
 * 模块名对进行，所以一个文件里的 `use` 不会漏进另一个文件。线性扫描 `opens` 表。
 *
 * @param m         已合并的模块，它的 `opens` 表由加载器填好。
 * @param importer  做引用的模块；根文件为 NULL。
 * @param opened    拥有该名字的模块。
 * @param what      要查的名字。
 * @return          该模块用 `use opened::*` 或 `use opened::{what}` 带进了这个名字时返回
 *                  `true`；否则返回 `false`，`m` 为 NULL 时也是 `false`。
 */
 bool nameBrought (Module *m, const char *importer, const char *opened, const char *what);
/**
 * @brief 取出一个函数可见的类型参数表。
 *
 * 定义在结构体里的方法看到所属类型的类型参数，自由函数看到自己的，两种情况都从这里回答。
 *
 * @param f  要查看的函数或方法；可以是 NULL。
 * @return   函数自己的类型参数，或所属类型的类型参数；不是泛型时返回 NULL。
 */
 Vec *funcTParams (FuncDef *f);
/**
 * @brief 取一个泛型自由函数的具体实例，没有就建一个。
 *
 * 同一份模板加同一组类型实参只对应一个实例，命中已有的就返回它。新实例是模板的浅拷贝，
 * 回指模板、填好类型实参与按类型实参改编的 C 名，参数与返回类型逐个代换，并追加到检查器
 * 的实例表与模块的函数表。类型实参嵌套过深或实例总数超限时报内部错误并返回 NULL。
 *
 * @param c      检查器；实例追加到 `c->funcInsts` 与模块的函数表。
 * @param tmpl   泛型模板；NULL 返回 NULL。
 * @param targs  具体的类型实参。
 * @param line   调用点的源码行，用于超限诊断。
 * @return       独立的 `FuncDef`，codegen 按普通函数发射；NULL 表示模板为空或触到上限。
 */
 FuncDef *funcInstance (Checker *c, FuncDef *tmpl, Vec *targs, int line);
/**
 * @brief 用形参类型与实参类型做合一，把类型实参填进 `targs`。
 *
 * 类型参数直接填入对应格子，已填的格子必须与新类型相等；引用与泛型实例逐层下钻。
 * 形状不一致却还提到类型参数时算推断失败。
 *
 * @param tt     类型表。
 * @param tp     类型参数的名字，顺序与 `targs` 一致。
 * @param targs  每个类型参数一格，在合一过程中填入。
 * @param want   可能提到类型参数的形参类型。
 * @param got    实参的类型。
 * @return       无法推断时返回 `false`，调用点就必须把类型实参写出来；否则返回 `true`。
 */
 _Bool unifyTParams (TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got);
/**
 * @brief 记下条件能证明的每一条非空事实。
 *
 * `&&` 的两边都走：能到达条件成立的这一支，说明两边都成立。只有在该支为"真"的证明才记。
 *
 * @param c     检查器。
 * @param cond  要从中取事实的条件；NULL 忽略。
 */
 void narrowFactsOf (Checker *c, Expr *cond);
/**
 * @brief 退出最内层词法作用域，丢掉它的绑定并撤销它建立的非空事实。
 *
 * 收窄表按进入作用域时记下的水位截回，所以收窄与其它绑定一样是词法作用域的。
 *
 * @param c  检查器。
 */
 void popScope (Checker *c);
/**
 * @brief 记下当前位置某个绑定已被证明非空。
 *
 * 记的是绑定的生成 C 名而不是源码里写的名字，因为收窄跟着绑定走。重复记同一个名字不改变
 * 任何东西。
 *
 * @param c      检查器。
 * @param cname  绑定的生成 C 名；NULL 忽略。
 */
 void pushNarrow (Checker *c, const char *cname);
/**
 * @brief 进入一个新的词法作用域。
 *
 * 同时记下当前收窄表的长度作为水位，块结束时把收窄表恢复到它：块内证明的可空引用事实随块一起消失。
 *
 * @param c  检查器。
 */
 void pushScope (Checker *c);
/**
 * @brief 把 `new T[n]` 的尺寸检查推迟到实例化。
 *
 * 只在当前处于泛型体（可见类型参数非空）里记录；具体类型当场就能判定，推迟只会让错误来得更晚。
 *
 * @param c     检查器；记录追加到 `c->refChecks`。
 * @param t     声明的元素类型。
 * @param line  源码行，用于诊断。
 */
 void recordNewSizeCheck (Checker *c, Type *t, int line);
/**
 * @brief 把声明缺少初值时的零值检查推迟到实例化。
 *
 * 泛型体里 `T` 有没有零值要等类型实参才知道，所以记录在这里，实例化时再判。只在可见类型
 * 参数非空时记录。
 *
 * @param c     检查器；记录追加到 `c->refChecks`。
 * @param t     声明的类型。
 * @param line  源码行，用于诊断。
 * @param name  绑定名，用于诊断。
 */
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
/**
 * @brief 丢掉一个绑定以及以它为前缀的路径的非空证明。
 *
 * 证明按栈保存，从这个绑定的条目起整段截掉：对根的写入或取地址都会让 `h.p` 这类路径事实
 * 失效。绑定从未被证明过时什么也不做。
 *
 * @param c      检查器。
 * @param cname  绑定的生成 C 名；NULL 忽略。
 */
 void unNarrow (Checker *c, const char *cname);
/**
 * @brief 判断一条语句能不能直接或经过它调用的函数建出池来。
 *
 * 答案读的是被调者，因此调用点必须先解析好被调者；块与 domain 块各自是一处"地方"，
 * `descendBlocks` 为假时停在这里，为真时走进去，供传递闭包使用。
 *
 * @param s               要检查的语句；NULL 视为否。
 * @param descendBlocks   是否走进嵌套的块与 domain 块。
 * @return                语句里出现池构造调用时返回 `true`，否则 `false`。
 */
 bool stmtMakesPool (Stmt *s, bool descendBlocks);

#endif /* EXTC_CHECK_INTERNAL_H */
