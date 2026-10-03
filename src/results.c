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
 * This replaced an open-addressing table **keyed by the node pointer**. The difference
 * that matters is not speed: an id is a value that can be stored, compared and checked,
 * while a pointer key silently matches a recycled address -- the hazard LLVM documents
 * for cached analyses ("If a function is deleted in a module pass, its address is still
 * used as the key for cached analyses").
 *
 * Life of the storage: one process, one compilation. Nothing is freed -- the same
 * lifetime the old table had -- and `resultsCount()` reports the range that was handed
 * out.
 */
#include "results.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>

/* id -> node, and id -> result slot, as **tables of pointers to fixed blocks**.
 *
 * These used to be two arrays that grew with `realloc`, which is a latent bug with teeth:
 * `realloc` can move the array, so every `NodeResults *` handed out before the growth
 * dangles -- and the accessors do exactly the thing that walks into it ("read the old value,
 * then write the new one": `r->an.arenaSites = v`, where `v` was read out of `r`).
 *
 * Fixing it in the accessors was not an option: every one of them would have to re-look-up
 * its slot after any operation that might grow the table, and "remember to re-look-up" is a
 * rule nobody keeps. A stable table makes the pointer valid for the life of the process,
 * which is what the rest of this file already assumes. */
#define ID_BLOCK 1024

static const void ***g_nodes;     /* block table: g_nodes[id / ID_BLOCK][id % ID_BLOCK] */
static NodeResults **g_results;
static size_t        g_len;       /* ids handed out so far */
static size_t        g_cap;       /* ids the blocks can hold */

static const void **h_keys;       /* pointer -> id hash */
static NodeId      *h_vals;
static size_t       h_cap, h_len;

static size_t hashPtr(const void *p) {
    size_t h = (size_t)p >> 4;              /* nodes are aligned: low bits carry nothing */
    h *= 0x9E3779B97F4A7C15ull;
    return h;
}

#define NODE_AT(id) (g_nodes[(id) / ID_BLOCK][(id) % ID_BLOCK])
#define SLOT_AT(id) (g_results[(id) / ID_BLOCK][(id) % ID_BLOCK])

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

/* The id of `node`, or 0 when it has none and `create` is false. */
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
static size_t        g_ownerMismatch;   /* accesses from a body other than the slot's */

void resultsEnterOwner(const FuncDef *owner) { g_owner = owner; }
void resultsLeaveOwner(void) { g_owner = NULL; }
const FuncDef *resultsCurrentOwner(void) { return g_owner; }
size_t resultsOwnerMismatches(void) { return g_ownerMismatch; }

/* Is the owner reported? `EXTC_DBG_OWNER=1` counts; `EXTC_DBG=1` aborts (it means a bug). */
static bool ownerReportOn(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("EXTC_DBG_OWNER");
        on = (v && *v && *v != '0');
    }
    return on != 0;
}

NodeResults *resultsById(NodeId id) {
    if (id == 0 || id > g_len) return NULL;
    return &SLOT_AT(id);
}

NodeResults *resultsOf(const void *node, bool create) {
    NodeId id = nodeIdOf(node, create);
    return id ? &SLOT_AT(id) : NULL;
}

static const char *kindName(ResultKind k) {
    switch (k) {
    case RKIND_EXPR:  return "Expr";
    case RKIND_STMT:  return "Stmt";
    case RKIND_FUNC:  return "FuncDef";
    case RKIND_OTHER: return "other";
    default:          return "none";
    }
}

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
     * checking another. That is the same class of bug as a table from function A indexed
     * by a node of function B, which rustc's owner-guarded accessors exist to catch. */
    if (r->owner && g_owner && r->owner != g_owner) {
        /* The first time a slot crosses a body boundary it is *shared* from then on (the
         * generic-body case). Only the first crossing is reported, because that is the one
         * that tells us the node is shared -- after that the slot is exempt by construction. */
        if (!r->shared) {
            r->shared = true;
            g_ownerMismatch++;
            /* Observation only, for now: a node inside a generic body is *reached* by
             * every instance of that body, and today the analysis writes per-instance
             * results onto it (last writer wins -- the same shape the callee pointer had
             * before X3). That is the coupling T4 removes, so this is a worklist, not a
             * verdict. It becomes a hard failure once the analysis families own their
             * results per owner. */
            if (ownerReportOn())
                fprintf(stderr, "[owner] slot first reached from another body"
                                " (first write at line %d, count %zu)\n",
                        r->line, g_ownerMismatch);
        }
    }
    /* One slot serves one node, and a node is one kind of thing: a result slot written
     * as a `Stmt`'s and then read as an `Expr`'s means the id was mixed up (or the same
     * address was reused for a different node -- the pointer-key hazard results.h warns
     * about). Abort loudly in debug builds; a release build keeps the old behaviour so
     * that a false positive cannot break a release. */
    EXTC_DBG_ASSERT_MSGF(r->kind == kind,
        "result slot used as %s but first written as %s (first write at line %d)",
        kindName(kind), kindName(r->kind), r->line);
    return r;
}

size_t resultsCount(void) { return g_len; }
