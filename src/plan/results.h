/**
 * @file results.h
 * @brief 结果侧：分析结果住在这里，按稠密的节点 id 寻址。
 *
 * 单独一层的原因（docs/topics/AST-DECOUPLING.md）：语法树在解析器建完之后必须冻结，所以
 * 后续阶段关于它算出的一切都住在树**旁边**，而不是树上。形状：
 *
 *   - 每个主人一个结果槽，而不是每个节点 N 个字段；
 *   - 按**稠密整数 id** 寻址，每个节点分配一次，于是查找是数组下标而不是指针哈希；
 *   - 节点可能消失之后，指针不能再当缓存键 —— 所以结果存的名字是 id，不是指针。
 *
 * id 在节点第一次需要结果时分配，之后永不移动。`NodeId` 故意是普通整数：它能被存下、
 * 比较、记日志，不用拖着一个指针。
 *
 * 三个入口：
 *
 *   nodeIdOf(node, create)   - 节点的 id（create = 新节点就分配一个）
 *   resultsOf(node, create)  - 节点的结果槽，按指针
 *   resultsById(id)          - 节点的结果槽，按 id
 *
 * 不创建时，还没有结果的节点查到 NULL，而 `plan.c` 里每个访问器都把 NULL 读作"默认值"。
 */
#ifndef EXTC_RESULTS_H
#define EXTC_RESULTS_H

#include <stdint.h>

#include "ast.h"

/* A dense node index. 0 is never handed out, so it doubles as "no id". */
typedef unsigned NodeId;

/* Which kind of node a result slot belongs to. The first writer stamps it, and every
 * later access checks it -- rustc does the same with `validate_hir_id_for_typeck_results`
 * ("this table is from function A, this node is from function B"). A mismatch is a
 * compiler bug, not a program error, so it aborts under `EXTC_DBG=1` rather than
 * returning a default. */
typedef enum { RKIND_NONE = 0, RKIND_EXPR, RKIND_STMT, RKIND_FUNC, RKIND_OTHER } ResultKind;

/* Everything the plan side knows about one node, in one object -- the "one result slot
 * per owner" shape. A node that is only a call site uses two of these fields; a function
 * uses another set; nothing forces a node to spread its results across the tree. */
typedef struct {
    /* expression sites */
    int  arenaLevel, zoneLevel, arenaArg;
    bool needTemp;
    /* function summary */
    bool usesHome, mayUseArena, makesPool;
    /* statements: this loop's condition allocates (released per round) */
    bool condAllocs;
    /* This declaration's initializer names storage that outlives every frame (a global, a
     * `global` declaration, an `effects Ret=0` result, a string literal). A **result**, not
     * syntax: the checker computes it while the scopes are live, and `coroCheckDeferred` reads it
     * after the body -- by then a function-local name can no longer be resolved, so asking
     * `exprOutOfFrame` again would answer "no". */
    bool varInitOO;
    /* A desugared `for` carries its step statement as **syntax** (`Stmt.forStep`, written by
     * the parser). The checker drops that step when it retargets the loop onto an iterator,
     * and that decision is a result, not syntax -- so it is recorded here. */
    bool forStepDropped;
    /* coroutines */
    bool isCoro, coroNeedsZone, coroBoxed;
    Type *yieldType, *coroFrameType;
    int  coroProto;
    /* identity */
    const char *instName;
    FuncDef    *tmpl;          /* instance -> template */
    FuncDef    *func;          /* the callee a call site / function value resolves to */
    bool        used;          /* was this function called? the emission gate reads it */
    const char *cname;         /* the C name a binding is emitted under */
    /* ---- plan facts about a struct definition ---- */
    bool        builtinHolder; /* the struct behind a builtin name needs no C definition */
    FuncDef    *coroOf;        /* this frame struct belongs to that coroutine */
    TraitDef   *implTrait;     /* the trait an `impl` block implements (NULL = inherent) */
    Type       *implTarget;    /* the type an `impl` block resolves to */
    FuncDef    *parWorker;     /* `parallel::run(worker, ...)`: the worker, for the trampoline */
    bool        domNew;        /* `sys::domain::single()`: the assoc builtin with no callee */
    bool        viewOf;        /* `std::sys::heap::extc_viewOf`: the plate layer's primitive */
    bool        isExtTarget;   /* an extern target the ABI calls directly */
    bool        isParWorker;   /* used as a `parallel::run` worker */
    bool        parTlsArena;   /* a worker that may use the thread-local arena */
    bool        deref;         /* value position, but the type is `ref T`: emit `*(…)` */
    int         coroKind;      /* the index codegen's prepass gave this coroutine */
    bool        dynTable;      /* its body is named by a `dyn` vtable thunk, from outside */
    Type       *coroRetProto;  /* the declared return type of a coroutine, before substitution */
    bool        boxedCoro;     /* a coroutine call stored as a handle: codegen boxes it */
    uint64_t    addrMask;
    /* ---- call / receiver facts ---- */
    Expr       *extDom;        /* `EX_EXT`: the domain this task is handed to */
    FieldDef   *field;         /* the field an `EX_FIELD` resolved to */
    Type       *assocOwner;    /* `EX_ASSOC`: the instance type it resolved to */
    bool        dynRecvViaRef; /* the receiver of a `dyn` call arrives through a reference */
    bool        callViaFn;     /* the call goes through a function value, not a declaration */
    bool        usedDyn;       /* a trait named by a `dyn` form (only then is its vtable emitted) */
    bool        usesDyn;       /* a module that saw a `dyn` form (the runtime is needed) */

    /* ---- the checker's own analysis facts (code generation never reads these) ----
     *
     * Same storage shape, different audience: these are working state the checker needs to
     * carry between its passes, and no later phase may depend on them. Keeping them in the
     * same slot keeps one id space and one arena; the field list in `plan.c`
     * (ANALYSIS_FIELDS) is what keeps the two audiences from quietly merging. */
    struct {
        const char *lamSig;    /* a lambda's written signature, for diagnostics */
        bool        makesPoolAny; /* some site of it builds a pool */
        /* depth and origin facts. `refDepth` exists on an `Expr` **and** on the checker's
         * `Sym`: one question, one slot, one accessor for both. */
        int  refDepth;
        int  homeDepth;
        int  lexicalLevel;
        int  storedAt;
        /* operator / comparison facts */
        int  minAt;            /* the block level a borrow must outlive */
        bool reuse;            /* the value may be reused at the same site */
        bool borrowed;         /* the value is borrowed (mirrors `Sym.borrowed`) */
        bool qualified;        /* the user wrote a qualified name (`a::b`) */
        bool convCheck;        /* a narrowing conversion whose runtime check is emitted */
        bool needOp;           /* a comparison over a type parameter, resolved at instantiation */
        /* per-function facts (staged: call sites still use the fields) */
        bool isAssoc; bool lamChecked; bool lamInferRet; bool needsHome;
        int  allocState; int mayPrintState; int freshCount;
        Vec  arenaSites; int nParamSyms; void *paramSyms[64];
        /* The effect summary itself: one bit per parameter, "a call to
         * this function may store that parameter's address / contents into the caller's
         * frames (`…Addr` / `…Cont`), into this function's home arena (`home…`), or
         * somewhere else (`otherMask`)". `addrFromLocal` is the same question for a value
         * whose storage is a local. Read only by the checker. */
        uint64_t addrMask, contMask, otherMask;
        uint64_t homeAddrMask, homeContMask;
        bool     addrFromLocal;
        int  owSites;          /* sites the write-once pass recorded */
        bool owLocal;          /* the write-once local pass applies to this function */
        /* The effect closure's state. `effState` is **work state**: it is
         * written and read only while the closure runs (`EFF_IN_PROGRESS` marks a cycle on the
         * way down), and nothing outside `computeEffectsTransitive` reads it. It lives here
         * rather than in a local table because the closure is a memoised recursion over a
         * shared call graph -- the variant a per-call table would need is a `FuncDef*` hash,
         * which is what this slot already is. `effComplete`/`effUnknown` are real results:
         * later passes (and the closure itself) ask whether the summary can be trusted. */
        int  effState;
        bool effComplete;
        bool effUnknown;
    } an;
    /* which of the fields above were written; an unset field reads as its default */
    /* Which fields of this slot were written. **Two words on purpose**: the plan side alone
     * has grown past 64 facts (`PLAN_DEREF` is bit 63), and a single word means the next fact
     * cannot be added without rewriting every bit test. Bits are `(hi << 6) | lo` -- see
     * `resultsBit` / `resultsSetBit` in results.c. */
    uint64_t loMask, hiMask;
    ResultKind kind;           /* which kind of node this slot belongs to (see ResultKind) */
    const FuncDef *owner;      /* the body whose results this slot holds (NULL = module level) */
    bool shared;               /* more than one body legitimately reaches this slot */
    int line;                  /* where the first write happened, for the guard's message */
} NodeResults;

/**
 * @brief 取 `node` 的 id，`create` 为 true 时给它分配一个。
 *
 * @param node    节点；NULL 没有 id。
 * @param create  为 true 时，没有 id 就分配一个（稠密、从 1 开始）。
 * @return        节点的 id；`node` 为 NULL 或没有 id 且不创建时返回 0。
 */
NodeId nodeIdOf(const void *node, bool create);

/* ---- 主人：结果空间的分区 ----------------------------------------------------------
 *
 * id 是稠密的，而稠密 id 只有在作用域是结构化的时候才有用：一个 id 属于一具函数体
 * （"主人"），跨体访问是编译器 bug，不是程序错误。
 */

/**
 * @brief 进入一具函数体；之后新建的结果槽都盖它的主人戳。
 *
 * @param owner  当前正在检查的函数体；NULL 表示"不在任何函数体里"（模块级工作），
 *               那里建的槽没有主人，对它的任何访问都不会不匹配。
 */
void resultsEnterOwner(const FuncDef *owner);

/**
 * @brief 离开当前函数体，回到模块级工作。
 */
void resultsLeaveOwner(void);

/**
 * @brief 取当前的主人。
 * @return  正在检查的函数体；模块级工作时返回 `NULL`。
 */
const FuncDef *resultsCurrentOwner(void);

/**
 * @brief 有多少次访问来自建立该槽的函数体之外。
 *
 * 每次都计（一次分支），编译期可以拿它当信号。第一次跨界就把槽标成 `shared` 并计数，
 * 之后不再报：泛型体里的一个节点会被该体的每个实例够到，那是设计如此 —— 树是共享的，
 * 所以答案住在树旁边。检查仍然抓得住危险的那一种：只有一个体会够到的槽被第二个碰了。
 *
 * @return  首次跨界访问的累计次数。
 */
size_t resultsOwnerMismatches(void);

/**
 * @brief 取 `node` 的结果槽。
 *
 * @param node    节点。
 * @param create  为 true 时没有槽就建一个。
 * @return        结果槽；没有槽且不创建时返回 `NULL`。
 */
NodeResults *resultsOf(const void *node, bool create);

/**
 * @brief 取 `node` 的结果槽，并声明调用方认为它是哪种节点。
 *
 * 首次写入把 `kind` 与当时的 `owner` 盖在槽上，之后每次访问都检查：一个槽装成另一种
 * 节点，或槽的主人与当前函数体不同，都是编译器 bug 而不是程序错误 —— `EXTC_DBG=1` 下
 * 中止，发布构建里忽略（值照旧返回，行为不变）。
 *
 * 跨界是**一等结果而非失败**：泛型体里的节点会被该体的每个实例够到，第一次跨界把槽标成
 * `shared` 并计数，此后不再报 —— 同一份源码产生同一组共享槽，是确定性的。
 *
 * @param node    节点。
 * @param create  为 true 时没有槽就建一个。
 * @param kind    调用方认为的节点种类；`RKIND_NONE` 跳过检查。
 * @param line    首次写入的位置，用于守卫的消息。
 * @return        结果槽；没有槽且不创建时返回 `NULL`。
 */
NodeResults *resultsAs(const void *node, bool create, ResultKind kind, int line);

/**
 * @brief 读掩码里的第 `bit` 位。
 *
 * 位是 `(hi << 6) | lo`：`bit < 64` 落在 `loMask`，其余落在 `hiMask`，调用方不必知道
 * 自己落在哪个字里。
 *
 * @param r    结果槽。
 * @param bit  位号，与 setter 用的是同一批常量。
 * @return     该位为 1 则 `true`。
 */
bool resultsBit(const NodeResults *r, uint64_t bit);

/**
 * @brief 置掩码里的第 `bit` 位。
 *
 * @param r    结果槽。
 * @param bit  位号。
 */
void resultsSetBit(NodeResults *r, uint64_t bit);

/**
 * @brief 按 id 取结果槽。
 * @param id  节点 id。
 * @return    结果槽；id 为 0 或超出已发放范围时返回 `NULL`。
 */
NodeResults *resultsById(NodeId id);

/**
 * @brief 已经发出去多少个 id（稠密区间的长度）。
 *
 * 诊断与按主人划分的工作读它；编译器里没有东西依赖这个数。
 *
 * @return  已发放的 id 个数。
 */
size_t resultsCount(void);

#endif /* EXTC_RESULTS_H */
