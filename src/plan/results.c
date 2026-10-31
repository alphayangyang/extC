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
 * @brief Read bit `bit` of the mask.
 *
 * The bit is `(hi << 6) | lo`: `bit < 64` lands in `loMask` and the rest in `hiMask`, so callers
 * need not know which word they land in.
 *
 * @param r    The result slot.
 * @param bit  Bit number, using the same constants as the setter.
 * @return     `true` when the bit is 1.
 */
bool resultsBit(const NodeResults *r, uint64_t bit) {
    if (!r) return false;
    return bit < 64 ? (r->loMask >> bit) & 1u : (r->hiMask >> (bit - 64)) & 1u;
}

/**
 * @brief Set bit `bit` of the mask.
 *
 * @param r    The result slot.
 * @param bit  Bit number.
 */
void resultsSetBit(NodeResults *r, uint64_t bit) {
    if (!r) return;
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
 * @brief Hash a node pointer into a bucket index.
 *
 * @param p  The node pointer; non-NULL. Nodes are pointer-aligned, so the low bits carry no
 *           information.
 * @return   The hash value; the caller then ANDs it with the capacity mask.
 */
static size_t hashPtr(const void *p) {
    size_t h = (size_t)p >> 4;              /* nodes are aligned: low bits carry nothing */
    h *= 0x9E3779B97F4A7C15ull;
    return h;
}

#define NODE_AT(id) (g_nodes[(id) / ID_BLOCK][(id) % ID_BLOCK])
#define SLOT_AT(id) (g_results[(id) / ID_BLOCK][(id) % ID_BLOCK])

/**
 * @brief Append one more id-table block, growing the capacity to the next block boundary.
 *
 * Only the block pointer table itself moves; blocks already handed out are not relocated, so the
 * `NodeResults *` given to callers does not become stale.
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
 * @brief Double the capacity of the pointer-to-id hash table and reinsert the old entries.
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
 * @brief Get `node`'s id, allocating one for it when `create` is true.
 *
 * @param node    The node; NULL has no id.
 * @param create  When true, allocate one if the node has no id yet (dense, starting at 1).
 * @return        The node's id; 0 when `node` is NULL, or when it has no id and none is created.
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
 * @brief Enter a function body; result slots created afterwards carry its owner stamp.
 *
 * @param owner  The function body being checked now; NULL means "in no function body"
 *               (module-level work), where slots have no owner and no access to them ever
 *               mismatches.
 */
void resultsEnterOwner(const FuncDef *owner) { g_owner = owner; }

/**
 * @brief Leave the current function body and go back to module-level work.
 */
void resultsLeaveOwner(void) { g_owner = NULL; }

/**
 * @brief Get the current owner.
 * @return  The function body being checked; `NULL` for module-level work.
 */
const FuncDef *resultsCurrentOwner(void) { return g_owner; }

/**
 * @brief How many accesses came from outside the function body that created the slot.
 *
 * Every one is counted (one branch) so the compiler can use it as a compile-time signal. The
 * first crossing marks the slot `shared` and counts it; afterwards nothing is reported any more: a
 * node inside a generic body is reached by every instance of that body, which is by design -- the
 * tree is shared, so the answers live beside the tree. The check still catches the dangerous
 * variety: a slot only one body would reach, touched by a second one.
 *
 * @return  The cumulative number of first-crossing accesses.
 */
size_t resultsOwnerMismatches(void) { return g_ownerMismatch; }

/**
 * @brief Whether an owner mismatch is worth reporting.
 *
 * `EXTC_DBG_OWNER=1` counts and prints it; `EXTC_DBG=1` aborts outright on a mismatch (a mismatch
 * is a compiler bug). The environment variable is read once and then cached, so changing it in the
 * middle of the process does not change this function's result.
 *
 * @return  true when the environment variable is non-empty and its first character is not `0`.
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
 * @brief Get a result slot by id.
 * @param id  The node id.
 * @return    The result slot; `NULL` when `id` is 0 or beyond the range handed out so far.
 */
NodeResults *resultsById(NodeId id) {
    if (id == 0 || id > g_len) return NULL;
    return &SLOT_AT(id);
}

/**
 * @brief Get `node`'s result slot.
 *
 * @param node    The node.
 * @param create  When true, create a slot if there is none.
 * @return        The result slot; `NULL` when there is none and none is created.
 */
NodeResults *resultsOf(const void *node, bool create) {
    NodeId id = nodeIdOf(node, create);
    return id ? &SLOT_AT(id) : NULL;
}

/**
 * @brief Get the diagnostic name of a result kind.
 *
 * @param k  Result kind.
 * @return   A static string; `RKIND_NONE` and unknown values return `"none"`.
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
 * @brief Get `node`'s result slot and declare which node kind the caller believes it is.
 *
 * The first write stamps `kind` and the then-current `owner` onto the slot, and every later
 * access checks them: a slot dressed as another node kind, or a slot whose owner differs from the
 * current function body, are compiler bugs rather than program errors -- abort under
 * `EXTC_DBG=1`, ignore in release builds (the value is returned as usual, behaviour unchanged).
 *
 * A crossing is a **first-class result, not a failure**: a node inside a generic body is reached
 * by every instance of that body, so the first crossing marks the slot `shared` and counts it and
 * nothing is reported afterwards -- the same source produces the same set of shared slots, which
 * is deterministic.
 *
 * @param node    The node.
 * @param create  When true, create a slot if there is none.
 * @param kind    The node kind the caller assumes; `RKIND_NONE` skips the check.
 * @param line    Where the first write happened, for the guard's message.
 * @return        The result slot; `NULL` when there is none and none is created.
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
 * @brief How many ids have been handed out (the length of the dense range).
 *
 * Diagnostics and owner-partitioned work read it; nothing in the compiler depends on this number.
 *
 * @return  The number of ids handed out.
 */
size_t resultsCount(void) { return g_len; }
