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

static const void  **g_nodes;     /* id -> node (the id is the index) */
static NodeResults  *g_results;   /* id -> result slot */
static size_t        g_len;       /* ids handed out so far */
static size_t        g_cap;

static const void **h_keys;       /* pointer -> id hash */
static NodeId      *h_vals;
static size_t       h_cap, h_len;

static size_t hashPtr(const void *p) {
    size_t h = (size_t)p >> 4;              /* nodes are aligned: low bits carry nothing */
    h *= 0x9E3779B97F4A7C15ull;
    return h;
}

static void growIds(void) {
    size_t ncap = g_cap ? g_cap * 2 : 256;
    const void **nn = realloc((void *)g_nodes, ncap * sizeof *nn);
    NodeResults *nr = realloc(g_results, ncap * sizeof *nr);
    if (!nn || !nr) abort();
    g_nodes = nn;
    g_results = nr;
    for (size_t i = g_cap; i < ncap; i++) {
        g_nodes[i] = NULL;
        g_results[i] = (NodeResults){0};
    }
    g_cap = ncap;
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
    g_nodes[id] = node;
    g_len++;
    h_keys[j] = node;
    h_vals[j] = id;
    h_len++;
    return id;
}

NodeResults *resultsById(NodeId id) {
    if (id == 0 || id > g_len) return NULL;
    return &g_results[id];
}

NodeResults *resultsOf(const void *node, bool create) {
    NodeId id = nodeIdOf(node, create);
    return id ? &g_results[id] : NULL;
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
        return r;
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
