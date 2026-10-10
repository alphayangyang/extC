/* The results side: implementation of the dense-id storage declared in results.h.
 *
 * Two arrays, one hash. The id is an index:
 *
 *     g_nodes[id]   - the node this id belongs to (NULL once the hash says so)
 *     g_results[id] - that node's result slot
 *
 * and one open-addressing hash maps a pointer to its id, because id assignment happens
 * at first use rather than during parsing (nothing in the parser has to change for the
 * results side to exist). Growth doubles; ids are stable, so a caller may keep one.
 *
 * The key is an id, never a node pointer: an id is a value that can be stored, compared
 * and checked, while a pointer key silently matches a recycled address -- the hazard LLVM
 * documents for cached analyses ("If a function is deleted in a module pass, its address
 * is still used as the key for cached analyses").
 *
 * Life of the storage: one process, one compilation. Nothing is freed, and
 * `resultsCount()` reports the range that was handed out.
 */
#include "results.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>
#include "results.h"

/* id -> node, and id -> result slot, as **tables of pointers to fixed blocks**.
 *
 * Growing a block table with `realloc` would be a latent bug with teeth: `realloc` can
 * move the array, so every `NodeResults *` handed out before the growth dangles -- and
 * the accessors do exactly the thing that walks into it ("read the old value, then write
 * the new one": `r->an.arenaSites = v`, where `v` was read out of `r`).
 *
 * Fixed blocks keep a handed-out pointer valid for the life of the process, which is what
 * the rest of this file assumes. */
#define ID_BLOCK 1024

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
bool resultsBit(const NodeResults *r, uint64_t bit) {
    return bit < 64 ? (r->loMask >> bit) & 1u : (r->hiMask >> (bit - 64)) & 1u;
}

/**
 * @brief 置掩码里的第 `bit` 位。
 *
 * @param r    结果槽。
 * @param bit  位号。
 */
void resultsSetBit(NodeResults *r, uint64_t bit) {
    if (bit < 64) r->loMask |= 1ull << bit;
    else          r->hiMask |= 1ull << (bit - 64);
}

static const void ***g_nodes;     /* block table: g_nodes[id / ID_BLOCK][id % ID_BLOCK] */
static NodeResults **g_results;
static size_t        g_len;       /* ids handed out so far */
static size_t        g_cap;       /* ids the blocks can hold */

static const void **h_keys;       /* pointer -> id hash */
static NodeId      *h_vals;
static size_t       h_cap, h_len;

/**
 * @brief 把一个节点指针散列成桶下标用的整数。
 *
 * @param p  节点指针；非 NULL，节点按指针对齐，所以低位没有信息。
 * @return   散列值；调用方再与容量掩码与。
 */
static size_t hashPtr(const void *p) {
    size_t h = (size_t)p >> 4;              /* nodes are aligned: low bits carry nothing */
    h *= 0x9E3779B97F4A7C15ull;
    return h;
}

#define NODE_AT(id) (g_nodes[(id) / ID_BLOCK][(id) % ID_BLOCK])
#define SLOT_AT(id) (g_results[(id) / ID_BLOCK][(id) % ID_BLOCK])

/**
 * @brief 追加一块 id 表，把容量扩到下一块边界。
 *
 * 只移动块指针表本身，已发放的块不搬，所以交给调用方的 `NodeResults *` 不会失效。
 */
static void growIds(void) {
    size_t nblocks = (g_cap ? g_cap / ID_BLOCK : 0) + 1;
    const void ***nb = realloc((void *)g_nodes, nblocks * sizeof *nb);
    NodeResults **rb = realloc((void *)g_results, nblocks * sizeof *rb);
    if (!nb || !rb) abort();
    g_nodes = nb;
    g_results = rb;
    g_nodes[nblocks - 1] = calloc(ID_BLOCK, sizeof *g_nodes[0]);
    g_results[nblocks - 1] = calloc(ID_BLOCK, sizeof *g_results[0]);
    if (!g_nodes[nblocks - 1] || !g_results[nblocks - 1]) abort();
    g_cap = nblocks * ID_BLOCK;
}

/**
 * @brief 把指针→id 哈希表容量翻倍，并把旧条目重新插入。
 */
static void growHash(void) {
    size_t ncap = h_cap ? h_cap * 2 : 256;
    const void **nk = calloc(ncap, sizeof *nk);
    NodeId *nv = calloc(ncap, sizeof *nv);
    if (!nk || !nv) abort();
    for (size_t i = 0; i < h_cap; i++) {
        if (!h_keys[i]) continue;
        size_t j = hashPtr(h_keys[i]) & (ncap - 1);
        while (nk[j]) j = (j + 1) & (ncap - 1);
        nk[j] = h_keys[i];
        nv[j] = h_vals[i];
    }
    free((void *)h_keys);
    free(h_vals);
    h_keys = nk;
    h_vals = nv;
    h_cap = ncap;
}

/**
 * @brief 取 `node` 的 id，`create` 为 true 时给它分配一个。
 *
 * @param node    节点；NULL 没有 id。
 * @param create  为 true 时，没有 id 就分配一个（稠密、从 1 开始）。
 * @return        节点的 id；`node` 为 NULL 或没有 id 且不创建时返回 0。
 */
NodeId nodeIdOf(const void *node, bool create) {
    if (!node) return 0;
    if (h_cap == 0 || (create && (h_len + 1) * 10 >= h_cap * 7)) growHash();
    size_t j = hashPtr(node) & (h_cap - 1);
    while (h_keys[j]) {
        if (h_keys[j] == node) return h_vals[j];
        j = (j + 1) & (h_cap - 1);
    }
    if (!create) return 0;
    if (g_len + 1 >= g_cap) growIds();
    NodeId id = (NodeId)(g_len + 1);        /* 0 stays "no id" */
    NODE_AT(id) = node;
    g_len++;
    h_keys[j] = node;
    h_vals[j] = id;
    h_len++;
    return id;
}

/* ---- owners ----------------------------------------------------------------------- */

static const FuncDef *g_owner;          /* the body whose results are being recorded now */
static size_t        g_ownerMismatch;   /* accesses from a body other than the one that owns the slot */

/**
 * @brief 进入一具函数体；之后新建的结果槽都盖它的主人戳。
 *
 * @param owner  当前正在检查的函数体；NULL 表示"不在任何函数体里"（模块级工作），
 *               那里建的槽没有主人，对它的任何访问都不会不匹配。
 */
void resultsEnterOwner(const FuncDef *owner) { g_owner = owner; }

/**
 * @brief 离开当前函数体，回到模块级工作。
 */
void resultsLeaveOwner(void) { g_owner = NULL; }

/**
 * @brief 取当前的主人。
 * @return  正在检查的函数体；模块级工作时返回 `NULL`。
 */
const FuncDef *resultsCurrentOwner(void) { return g_owner; }

/**
 * @brief 有多少次访问来自建立该槽的函数体之外。
 *
 * 每次都计（一次分支），编译期可以拿它当信号。第一次跨界就把槽标成 `shared` 并计数，
 * 之后不再报：泛型体里的一个节点会被该体的每个实例够到，那是设计如此 —— 树是共享的，
 * 所以答案住在树旁边。检查仍然抓得住危险的那一种：只有一个体会够到的槽被第二个碰了。
 *
 * @return  首次跨界访问的累计次数。
 */
size_t resultsOwnerMismatches(void) { return g_ownerMismatch; }

/**
 * @brief 主人不匹配是否值得报告。
 *
 * `EXTC_DBG_OWNER=1` 时计数并打印；`EXTC_DBG=1` 遇不匹配直接中止（不匹配是编译器 bug）。
 * 环境变量只读一次并缓存，进程中途改它不改变本函数的结果。
 *
 * @return  环境变量非空且首字符不是 `0` 时为 true。
 */
static bool ownerReportOn(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("EXTC_DBG_OWNER");
        on = (v && *v && *v != '0');
    }
    return on != 0;
}

/**
 * @brief 按 id 取结果槽。
 * @param id  节点 id。
 * @return    结果槽；id 为 0 或超出已发放范围时返回 `NULL`。
 */
NodeResults *resultsById(NodeId id) {
    if (id == 0 || id > g_len) return NULL;
    return &SLOT_AT(id);
}

/**
 * @brief 取 `node` 的结果槽。
 *
 * @param node    节点。
 * @param create  为 true 时没有槽就建一个。
 * @return        结果槽；没有槽且不创建时返回 `NULL`。
 */
NodeResults *resultsOf(const void *node, bool create) {
    NodeId id = nodeIdOf(node, create);
    return id ? &SLOT_AT(id) : NULL;
}

/**
 * @brief 取结果种类的诊断名。
 *
 * @param k  结果种类。
 * @return   静态字符串；`RKIND_NONE` 与未知值返回 `"none"`。
 */
static const char *kindName(ResultKind k) {
    switch (k) {
    case RKIND_EXPR:  return "Expr";
    case RKIND_STMT:  return "Stmt";
    case RKIND_FUNC:  return "FuncDef";
    case RKIND_OTHER: return "other";
    default:          return "none";
    }
}

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
NodeResults *resultsAs(const void *node, bool create, ResultKind kind, int line) {
    NodeResults *r = resultsOf(node, create);
    if (!r || kind == RKIND_NONE) return r;
    if (r->kind == RKIND_NONE) {
        r->kind = kind;
        r->line = line;
        /* The first writer also decides the scope: a slot created while checking one body
         * belongs to that body. Module-level work (owner NULL) leaves it unscoped. */
        r->owner = g_owner;
        return r;
    }
    /* Scope check: a slot created while checking one body must not be touched while
     * checking another -- the class of bug rustc's owner-guarded accessors catch. */
    if (r->owner && g_owner && r->owner != g_owner) {
        /* The first time a slot crosses a body boundary it is *shared* from then on (the
         * generic-body case). Only the first crossing is reported, because that is the one
         * that tells us the node is shared -- after that the slot is exempt by construction. */
        if (!r->shared) {
            r->shared = true;
            g_ownerMismatch++;
            /* Observation only, for now: a node inside a generic body is *reached* by
             * every instance of that body, and the analysis writes per-instance results
             * onto it (last writer wins), so this is a worklist, not a verdict. It becomes
             * a hard failure once the analysis families own their results per owner. */
            if (ownerReportOn())
                fprintf(stderr, "[owner] slot first reached from another body"
                                " (first write at line %d, count %zu)\n",
                        r->line, g_ownerMismatch);
        }
    }
    /* One slot serves one node, and a node is one kind of thing: a result slot written
     * as a `Stmt`'s and then read as an `Expr`'s means the id was mixed up (or the same
     * address was reused for a different node). The assert aborts under `EXTC_DBG=1`;
     * with the switch off the mismatch is ignored and the slot is still returned, so a
     * false positive cannot break a release. */
    EXTC_DBG_ASSERT_MSGF(r->kind == kind,
        "result slot used as %s but first written as %s (first write at line %d)",
        kindName(kind), kindName(r->kind), r->line);
    return r;
}

/**
 * @brief 已经发出去多少个 id（稠密区间的长度）。
 *
 * 诊断与按主人划分的工作读它；编译器里没有东西依赖这个数。
 *
 * @return  已发放的 id 个数。
 */
size_t resultsCount(void) { return g_len; }
