/* converged-model.c -- one shape, two memory models, measured side by side.
 *
 * The shape (mixed, intended to be comprehensive for a memory model):
 *   - 50k persistent 32 B nodes in region A, chained, reachable from a root
 *   - 200k churn nodes per round in region B, each holding one cross-region ref into A,
 *     all dropped immediately (garbage that must be swept and whose out-edges must be unlinked)
 *   - 10k cycles per round in region A (garbage that only a sweep can free)
 *   - one growable i64 array, 1M -> 4M elements (8 -> 32 MiB) at 1.1x per step, summed after growth
 *   - 20 x 512 KiB sparse blobs per round with 65,536 ref slots and 512 written groups,
 *     allocated, cross-region refs written, dropped
 *
 * MODEL cur  = what the compiler emits today: 512 KiB slabs with occ/mark/rsbit bitmaps and a
 *              freelist, big blocks as separate 512 KiB-aligned mappings, growth by
 *              allocate-new + memcpy (the old block becomes garbage), and capacity returned
 *              only when (cap - bytes >= 8 MiB) AND (cap <= 3 x bytes)  -- the two gates that
 *              keep grow11 at 3.8x live.
 * MODEL new  = the converged model: one unit (64 KiB chunk), one owner (region), three policies
 *              (T0 whole-region free / T1 count / T2 sweep -- both runs here are T2), and one
 *              contract (chunk ledger, step <= one chunk of work).  Growth appends chunks, so
 *              no element is ever copied and no dead generation is ever created; capacity is
 *              returned by a pure watermark (cap - live > 1 MiB), with no grace and no ratio gate.
 *
 * Deliberate simplifications (documented, not hidden): one object shape for small nodes, one
 * blob shape, one root array, single-threaded, no size classes, no compaction, no write barrier.
 * Everything else -- slab/chunk headers, bitmaps, freelists, mark stack, unlink on free,
 * cross-region counters, pause instrumentation, trigger law -- follows the emitted runtime.
 *
 * build: cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -o /tmp/cm/converged-model tools/probe/converged-model.c
 * run:   taskset -c 6 ./converged-model cur 3 ; taskset -c 6 ./converged-model new 3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------- platform ---------- */
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }
static double cpu_ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }
/* a step is charged the smaller of wall and thread-cpu time: preemption on a shared
 * host inflates wall, and the contract is about work done, not about the scheduler */
static double step_us_of(double w0, double c0) {
    double w = (now_ns() - w0) / 1000.0, c = (cpu_ns() - c0) / 1000.0;
    return (c < w) ? c : w;
}
static long rss_now_kb(void) {
    FILE *f = fopen("/proc/self/status", "r"); char l[256]; long kb = -1;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { kb = atol(l + 6); break; }
    fclose(f); return kb;
}
static long rss_peak_kb(void) {
    FILE *f = fopen("/proc/self/status", "r"); char l[256]; long kb = -1;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, "VmHWM:", 6)) { kb = atol(l + 6); break; }
    fclose(f); return kb;
}
static void *map_aligned(size_t bytes, size_t align) {
    size_t total = bytes + align;
    char *raw = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); exit(1); }
    uintptr_t base = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
    size_t head = base - (uintptr_t)raw, tail = total - head - bytes;
    if (head) munmap(raw, head);
    if (tail) munmap((void *)(base + bytes), tail);
    return (void *)base;
}

/* ---------- geometry ---------- */
#define MB (1024u * 1024u)
#define SLAB_CUR   (512u * 1024u)   /* current model: one slab */
#define CHUNK_NEW  (64u * 1024u)    /* converged model: one chunk */
#define HDR        512u             /* block header + inline bitmaps, payload follows */
#define SS         32u              /* small object size */
#define CHUNK_ELEMS 8128u           /* (65536 - 512) / 8 */
#define BLK_MAGIC  0x424c4b31u
#define MODEL_CUR  0
#define MODEL_NEW  1

typedef struct Model Model;
typedef struct Region Region;
typedef struct Block Block;
typedef struct Blob Blob;
typedef struct Grow Grow;
typedef struct Node Node;

struct Node { Node *next; int64_t v; Node *ref; };   /* 24 B payload, rounded up to SS */

struct Block {
    uint32_t magic, kind;      /* kind 1 = small object block */
    Region *reg;
    Block *next, *prev;
    uint32_t ss, nslots, nwords, bump;
    int32_t freelist;
    uint32_t nLive, nMark;
    int64_t bytes;             /* occupied bytes in this block */
    uint64_t *occ, *mark, *rsbit;
    uint32_t *rscnt, *nxt;     /* rscnt = incoming cross-region edges, nxt = free list link */
    size_t mapBytes;
    void *payload;
};

struct Blob {
    Blob *next;
    Region *reg;
    void *map;
    void *payload;
    size_t mapBytes;
    size_t bytes;
    uint32_t nref;
    uint64_t *mask;            /* 1 bit per 64 B written group */
    uint32_t maskWords;
    int pinned;                /* live by construction (a container buffer) */
    size_t dropped;            /* bytes already given back with MADV_DONTNEED */
    int dead;
};

struct Region {
    Model *m;
    int id;
    Block *blocks, *bump;
    Blob *blobs;
    int64_t bytes;             /* occupied bytes (garbage included until swept) */
    int64_t capBytes;          /* mapped bytes */
    uint32_t nblocks;
};

struct Model {
    int kind;
    Region *reg[2];
    void **roots; int nroots, rootCap;
    void **mstack; size_t msp, mcap;
    int marking;
    int64_t bytes, capBytes;
    int64_t thr;
    size_t ncollections, nsteps;
    double gc_ns;
    double *step_us; size_t nstep, stepCap;
    uint64_t rsIncN, rsDecN;
    int64_t copied;
    int64_t peakLive;
    int64_t liveAfter;
    double mxMark, mxSweep, mxReclaim;
    double growFactor;
    uint32_t sweepWord;
};

/* ---------- small helpers ---------- */
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n ? n : 1, s ? s : 1); if (!p) { fprintf(stderr, "oom\n"); exit(1); } return p; }
static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static void step_note2(Model *m, double us, int kind);
static void step_note(Model *m, double us) { step_note2(m, us, 0); }
static void step_note2(Model *m, double us, int kind) {
    if (m->nstep == m->stepCap) {
        m->stepCap = m->stepCap ? m->stepCap * 2 : 4096;
        m->step_us = (double *)realloc(m->step_us, m->stepCap * sizeof(double));
        if (!m->step_us) { fprintf(stderr, "oom\n"); exit(1); }
    }
    m->step_us[m->nstep++] = us;
    if (kind == 0 && us > m->mxMark) m->mxMark = us;
    else if (kind == 1 && us > m->mxSweep) m->mxSweep = us;
    else if (kind == 2 && us > m->mxReclaim) m->mxReclaim = us;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x < y) ? -1 : (x > y); }

static Block *objBlock(Model *m, void *p) {
    uintptr_t mask = (m->kind == MODEL_CUR) ? (uintptr_t)(SLAB_CUR - 1) : (uintptr_t)(CHUNK_NEW - 1);
    Block *b = (Block *)((uintptr_t)p & ~mask);
    return (b->magic == BLK_MAGIC) ? b : NULL;
}

/* ---------- regions ---------- */
static Region *region_new(Model *m, int id) {
    Region *r = (Region *)xcalloc(1, sizeof(Region));
    r->m = m; r->id = id; r->blobs = NULL; r->blocks = NULL; r->bump = NULL;
    return r;
}

static void blob_free(Blob *b);

static Block *block_new(Model *m, Region *r) {
    size_t chunk = (m->kind == MODEL_CUR) ? SLAB_CUR : CHUNK_NEW;
    char *map = (char *)map_aligned(chunk, chunk);
    Block *b = (Block *)map;
    memset(b, 0, sizeof *b);
    b->magic = BLK_MAGIC; b->kind = 1; b->reg = r;
    b->ss = SS;
    b->payload = map + HDR;
    b->nslots = (uint32_t)((chunk - HDR) / SS);
    b->nwords = (b->nslots + 63) / 64;
    b->occ = (uint64_t *)xcalloc(b->nwords, 8);
    b->mark = (uint64_t *)xcalloc(b->nwords, 8);
    b->rsbit = (uint64_t *)xcalloc(b->nwords, 8);
    b->rscnt = (uint32_t *)xcalloc(b->nslots, 4);
    b->nxt = (uint32_t *)xcalloc(b->nslots, 4);
    b->freelist = -1; b->bump = 0;
    b->mapBytes = chunk;
    b->next = r->blocks; if (r->blocks) r->blocks->prev = b; r->blocks = b;
    r->nblocks++; r->capBytes += (int64_t)chunk; m->capBytes += (int64_t)chunk;
    return b;
}

static void block_free(Model *m, Region *r, Block *b) {
    uint64_t *occ = b->occ, *mark = b->mark, *rsbit = b->rsbit;
    uint32_t *rscnt = b->rscnt, *nxt = b->nxt;
    size_t mb = b->mapBytes;
    if (b->prev) b->prev->next = b->next; else r->blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    if (r->bump == b) r->bump = NULL;
    r->nblocks--; r->capBytes -= (int64_t)mb; m->capBytes -= (int64_t)mb;
    munmap((void *)b, mb);
    free(occ); free(mark); free(rsbit); free(rscnt); free(nxt);
}

/* ---------- cross-region accounting ---------- */
static void rs_inc(Model *m, Node *target) {
    Block *tb = objBlock(m, target);
    uint32_t i = (uint32_t)(((char *)target - (char *)tb->payload) / tb->ss);
    if (tb->rscnt[i]++ == 0) tb->rsbit[i >> 6] |= 1ull << (i & 63);
    m->rsIncN++;
}
static void rs_dec(Model *m, Node *target) {
    Block *tb = objBlock(m, target);
    uint32_t i = (uint32_t)(((char *)target - (char *)tb->payload) / tb->ss);
    if (tb->rscnt[i] > 0 && --tb->rscnt[i] == 0) tb->rsbit[i >> 6] &= ~(1ull << (i & 63));
    m->rsDecN++;
}

/* ---------- allocation ---------- */
static Node *m_alloc_node(Model *m, int rid) {
    Region *r = m->reg[rid];
    Block *b = r->bump;
    if (!b || (b->freelist < 0 && b->bump >= b->nslots)) {
        b = NULL;
        for (Block *x = r->blocks; x; x = x->next)
            if (x->freelist >= 0 || x->bump < x->nslots) { b = x; break; }
        if (!b) b = block_new(m, r);
        r->bump = b;
    }
    uint32_t idx;
    if (b->freelist >= 0) { idx = (uint32_t)b->freelist; b->freelist = (b->nxt[idx] == 0xFFFFFFFFu) ? -1 : (int32_t)b->nxt[idx]; }
    else idx = b->bump++;
    b->occ[idx >> 6] |= 1ull << (idx & 63);
    b->nLive++; b->bytes += b->ss;
    r->bytes += b->ss; m->bytes += b->ss;
    Node *n = (Node *)((char *)b->payload + (size_t)idx * b->ss);
    memset(n, 0, b->ss);
    return n;
}

static void blob_drop(Blob *b) { b->dead = 1; b->pinned = 0; }

static Blob *blob_new(Model *m, int rid, size_t bytes, uint32_t nref) {
    Region *r = m->reg[rid];
    size_t align = (m->kind == MODEL_CUR) ? SLAB_CUR : CHUNK_NEW;
    size_t mapBytes = align_up(bytes + HDR, align);
    char *map = (char *)map_aligned(mapBytes, align);
    Block *b = (Block *)map;
    memset(b, 0, sizeof *b);
    b->magic = BLK_MAGIC; b->kind = 2; b->reg = r;
    b->mapBytes = mapBytes; b->ss = (uint32_t)bytes; b->nslots = 1;
    b->payload = map + HDR;
    Blob *bl = (Blob *)xcalloc(1, sizeof(Blob));
    bl->reg = r; bl->map = map; bl->payload = b->payload; bl->mapBytes = mapBytes; bl->bytes = bytes;
    bl->nref = nref;
    if (bytes >= 4096) { bl->maskWords = (uint32_t)((bytes / 64 + 63) / 64); bl->mask = (uint64_t *)xcalloc(bl->maskWords, 8); }
    bl->pinned = 1;
    bl->next = r->blobs; r->blobs = bl;
    r->bytes += (int64_t)bytes; m->bytes += (int64_t)bytes;
    r->capBytes += (int64_t)mapBytes; m->capBytes += (int64_t)mapBytes;
    return bl;
}

static void blob_store(Model *m, Blob *b, uint32_t idx, Node *target) {
    ((Node **)b->payload)[idx] = target;
    Block *tb = objBlock(m, target);
    if (tb->reg != b->reg) {
        rs_inc(m, target);
        if (b->mask) { uint32_t g = idx / 8; b->mask[g >> 6] |= 1ull << (g & 63); }
    }
}

static void blob_free(Blob *b) {
    Model *m = b->reg->m;
    if (b->mask) {
        uint32_t groups = (uint32_t)(b->bytes / 64);
        for (uint32_t g = 0; g < groups; g++) {
            if (!(b->mask[g >> 6] & (1ull << (g & 63)))) continue;
            Node **slot = (Node **)((char *)b->payload + (size_t)g * 64);
            for (int j = 0; j < 8; j++) if (slot[j]) rs_dec(m, slot[j]);
        }
        free(b->mask);
    }
    munmap(b->map, b->mapBytes);
    b->reg->bytes -= (int64_t)b->bytes; m->bytes -= (int64_t)b->bytes;
    b->reg->capBytes -= (int64_t)b->mapBytes; m->capBytes -= (int64_t)b->mapBytes;
    free(b);
}

/* ---------- marking ---------- */
static void mark_push(Model *m, void *p);

static void mark_pop(Model *m) {
    void *p = m->mstack[--m->msp];
    Node *n = (Node *)p;
    if (n->next) mark_push(m, n->next);
    if (n->ref) mark_push(m, n->ref);
}

static void mark_push(Model *m, void *p) {
    Block *b = objBlock(m, p);
    if (!b || b->kind != 1) return;
    uint32_t i = (uint32_t)(((char *)p - (char *)b->payload) / b->ss);
    if (b->mark[i >> 6] & (1ull << (i & 63))) return;
    b->mark[i >> 6] |= 1ull << (i & 63);
    b->nMark++;
    if (m->msp == m->mcap) {
        m->mcap = m->mcap ? m->mcap * 2 : 4096;
        m->mstack = (void **)realloc(m->mstack, m->mcap * sizeof(void *));
        if (!m->mstack) { fprintf(stderr, "oom\n"); exit(1); }
    }
    m->mstack[m->msp++] = p;
}

/* ---------- sweeping ---------- */
static void sweep_block_reset(Model *m, Block *b) {
    memset(b->occ, 0, b->nwords * 8);
    memset(b->mark, 0, b->nwords * 8);
    memset(b->rsbit, 0, b->nwords * 8);
    memset(b->rscnt, 0, (size_t)b->nslots * 4);
    b->freelist = -1; b->bump = 0; b->nLive = 0; b->nMark = 0;
    b->reg->bytes -= b->bytes; m->bytes -= b->bytes; b->bytes = 0;
}

static void free_slot(Model *m, Block *b, uint32_t i) {
    if (m->rsIncN) {
        Node *n = (Node *)((char *)b->payload + (size_t)i * b->ss);
        if (n->next) rs_dec(m, n->next);
        if (n->ref) rs_dec(m, n->ref);
    }
    b->occ[i >> 6] &= ~(1ull << (i & 63));
    b->nxt[i] = (b->freelist < 0) ? 0xFFFFFFFFu : (uint32_t)b->freelist;
    b->freelist = (int32_t)i;
    b->nLive--; b->bytes -= b->ss;
    b->reg->bytes -= b->ss; m->bytes -= b->ss;
}

/* one sweep step per region: one block per step (a 512 KiB slab for the current model, a
 * 64 KiB chunk for the converged one), so a step is one chunk of work by construction. */
static int sweep_step(Model *m, Region *r) {
    for (Block *b = r->blocks; b; b = b->next) {
        double w0 = now_ns(), c0 = cpu_ns();
        if (b->nMark == 0 && m->rsIncN == 0) {
            sweep_block_reset(m, b);
            step_note2(m, step_us_of(w0, c0), 1);
            m->nsteps++;
            continue;
        }
        for (uint32_t w = 0; w < b->nwords; w++) {
            uint64_t dead = b->occ[w] & ~b->mark[w];
            if (dead) {
                uint32_t base = w * 64;
                for (uint32_t bit = 0; bit < 64 && base + bit < b->nslots; bit++)
                    if (dead & (1ull << bit)) free_slot(m, b, base + bit);
            }
            b->mark[w] = 0;
        }
        b->nMark = 0;
        step_note2(m, step_us_of(w0, c0), 1);
        m->nsteps++;
    }
    return 1;
}

/* ---------- reclaim ---------- */
/* Free at most `budget` units of work and return how many were done.  A unit is one
 * block unmap, one dead blob, or one 512 KiB page-drop of a large dead blob -- the last
 * one is what keeps a 32 MiB unmap from becoming a 1.8 ms step. */
static int reclaim_step(Model *m, int budget) {
    int work = 0;
    for (int ri = 0; ri < 2 && work < budget; ri++) {
        Region *r = m->reg[ri];
        if (m->kind == MODEL_CUR) {
            if (m->capBytes - m->bytes < (int64_t)(8 * MB)) continue;
            if (m->capBytes > 3 * m->bytes) continue;
        }
        Blob **pl = &r->blobs;
        while (*pl && work < budget) {
            Blob *b = *pl;
            if (!b->dead) { pl = &b->next; continue; }
            if (m->kind == MODEL_NEW && m->capBytes - m->bytes <= (int64_t)(1 * MB)) break;
            if (b->bytes > (size_t)(512 * 1024) && b->dropped < b->bytes) {
                size_t step = 512 * 1024;
                if (step > b->bytes - b->dropped) step = b->bytes - b->dropped;
                madvise((char *)b->map + b->dropped, step, MADV_DONTNEED);
                b->dropped += step;
                work++;
                continue;
            }
            *pl = b->next;
            blob_free(b);
            work++;
        }
        Block *bl = r->blocks;
        while (bl && work < budget) {
            Block *nx = bl->next;
            if (bl->nLive == 0) { block_free(m, r, bl); work++; }
            bl = nx;
            if (m->kind == MODEL_CUR && m->capBytes > 3 * m->bytes) break;
        }
    }
    return work;
}

static void collect(Model *m) {
    double t0 = now_ns();
    m->ncollections++;
    m->msp = 0;
    for (int i = 0; i < m->nroots; i++) if (m->roots[i]) mark_push(m, m->roots[i]);
    while (m->msp) {
        double w0 = now_ns(), c0 = cpu_ns();
        uint32_t n = 0;
        while (m->msp && n < 512) { mark_pop(m); n++; }
        step_note(m, step_us_of(w0, c0));
        m->nsteps++;
    }
    for (int ri = 0; ri < 2; ri++) {
        double s0 = now_ns();
        sweep_step(m, m->reg[ri]);
        step_note(m, (now_ns() - s0) / 1000.0);
        m->nsteps++;
    }
    for (;;) {
        double w0 = now_ns(), c0 = cpu_ns();
        int w = reclaim_step(m, 4);
        step_note2(m, step_us_of(w0, c0), 2);
        m->nsteps++;
        if (!w) break;
    }
    {
        int64_t lv = m->bytes;
        for (int ri = 0; ri < 2; ri++)
            for (Blob *b = m->reg[ri]->blobs; b; b = b->next)
                if (b->dead) lv -= (int64_t)b->bytes;
        m->liveAfter = lv;
        if (lv > m->peakLive) m->peakLive = lv;
    }
    int64_t floor = (int64_t)(1 * MB);
    m->thr = 3 * (m->bytes > floor ? m->bytes : floor);
    m->gc_ns += now_ns() - t0;
}

static void maybe_collect(Model *m) { if (!m->marking && m->bytes > m->thr) collect(m); }

/* ---------- roots ---------- */
static void root_set(Model *m, int i, void *p) {
    if (i >= m->rootCap) {
        int nc = m->rootCap ? m->rootCap * 2 : 64;
        while (nc <= i) nc *= 2;
        m->roots = (void **)realloc(m->roots, (size_t)nc * sizeof(void *));
        if (!m->roots) { fprintf(stderr, "oom\n"); exit(1); }
        m->rootCap = nc;
    }
    m->roots[i] = p;
    if (i >= m->nroots) { m->nroots = i + 1; for (int k = 0; k < i; k++) if (m->roots[k] == NULL) m->roots[k] = NULL; }
}

/* ---------- growable array ---------- */
struct Grow { Blob *blob; int64_t *p; size_t cap, n; Blob **chunks; size_t nchunks; size_t chunksCap; };

static Grow *grow_new(Model *m) {
    Grow *g = (Grow *)xcalloc(1, sizeof(Grow));
    size_t cap = (m->kind == MODEL_CUR) ? (size_t)(1u << 20) : CHUNK_ELEMS;
    if (m->kind == MODEL_CUR) {
        g->blob = blob_new(m, 0, cap * 8, 0);
        g->p = (int64_t *)g->blob->payload;
        g->cap = cap;
    }
    return g;
}

static void grow_push(Model *m, Grow *g, int64_t v) {
    if (m->kind == MODEL_CUR) {
        if (g->n == g->cap) {
            size_t ncap = (size_t)((double)g->cap * m->growFactor);
            if (ncap <= g->cap) ncap = g->cap + 1;
            Blob *nb = blob_new(m, 0, ncap * 8, 0);
            memcpy(nb->payload, g->p, g->n * 8);
            m->copied += (int64_t)g->n * 8;
            blob_drop(g->blob);
            g->blob = nb; g->p = (int64_t *)nb->payload; g->cap = ncap;
        }
        g->p[g->n++] = v;
    } else {
        if ((g->n % CHUNK_ELEMS) == 0) {
            if (g->nchunks == g->chunksCap) {
                g->chunksCap = g->chunksCap ? g->chunksCap * 2 : 256;
                g->chunks = (Blob **)realloc(g->chunks, g->chunksCap * sizeof(Blob *));
                if (!g->chunks) { fprintf(stderr, "oom\n"); exit(1); }
            }
            Blob *nb = blob_new(m, 0, CHUNK_ELEMS * 8, 0);
            g->chunks[g->nchunks++] = nb;
        }
        ((int64_t *)g->chunks[g->nchunks - 1]->payload)[g->n % CHUNK_ELEMS] = v;
        g->n++;
    }
    if ((g->n & 0xFFFF) == 0) maybe_collect(m);
}

static int64_t grow_sum(Model *m, Grow *g) {
    int64_t s = 0;
    if (m->kind == MODEL_CUR) {
        for (size_t i = 0; i < g->n; i++) s += g->p[i];
    } else {
        for (size_t c = 0; c < g->nchunks; c++) {
            size_t lim = (c + 1 == g->nchunks) ? (g->n - c * CHUNK_ELEMS) : CHUNK_ELEMS;
            int64_t *p = (int64_t *)g->chunks[c]->payload;
            for (size_t i = 0; i < lim; i++) s += p[i];
        }
    }
    return s;
}

/* ---------- model ---------- */
static Model *model_new(int kind) {
    Model *m = (Model *)xcalloc(1, sizeof(Model));
    m->kind = kind;
    m->reg[0] = region_new(m, 0);
    m->reg[1] = region_new(m, 1);
    m->thr = 3 * (int64_t)(1 * MB);
    m->growFactor = 1.1;
    return m;
}

/* ---------- the shape ---------- */
#define N_PERS    50000
#define N_CHURN   200000
#define N_CYC     20000
#define BLOB_BYTES (512u * 1024u)
#define BLOB_NREF  65536u
#define BLOB_WRITES 512
#define BLOB_ITERS 20
#define GROW_END   (4u << 20)

static Node *chain[N_PERS];

static void node_store_ref(Model *m, Node *owner, Node *target) {
    owner->ref = target;
    Block *ob = objBlock(m, owner), *tb = objBlock(m, target);
    if (ob->reg != tb->reg) rs_inc(m, target);
}

static void run_workload(Model *m, int rounds) {
    double t0 = now_ns();
    /* 1. persistent chain in region A: the live set that never dies */
    for (int i = 0; i < N_PERS; i++) {
        Node *n = m_alloc_node(m, 0);
        n->v = i;
        n->next = (i ? chain[i - 1] : NULL);
        chain[i] = n;
        root_set(m, 0, n);
        if ((i & 0x3FF) == 0) maybe_collect(m);
    }
    Grow *g = grow_new(m);
    int64_t acc = 0;
    for (int rd = 0; rd < rounds; rd++) {
        double r0 = now_ns();
        /* 2. churn in region B, each node holding one cross-region ref into A */
        for (int i = 0; i < N_CHURN; i++) {
            Node *n = m_alloc_node(m, 1);
            n->v = i;
            node_store_ref(m, n, chain[(size_t)i % N_PERS]);
            root_set(m, 1, n);
            root_set(m, 1, NULL);
            if ((i & 0x1FFF) == 0) maybe_collect(m);
        }
        /* 3. cycles in region A: unreachable but only a sweep can free them */
        for (int i = 0; i < N_CYC / 2; i++) {
            Node *a = m_alloc_node(m, 0);
            Node *b = m_alloc_node(m, 0);
            a->next = b; b->next = a;
            root_set(m, 2, a);
            root_set(m, 2, NULL);
            if ((i & 0xFFF) == 0) maybe_collect(m);
        }
        /* 4. the growable array: 1M -> 4M i64 at 1.1x per step, then scanned */
        size_t target = (size_t)1 << 20;
        while (g->n < target) grow_push(m, g, (int64_t)g->n);
        while (target < GROW_END) {
            size_t next = (size_t)((double)target * 1.1);
            if (next <= target) next = target + 1;
            if (next > GROW_END) next = GROW_END;
            while (g->n < next) grow_push(m, g, (int64_t)g->n);
            target = next;
        }
        acc += grow_sum(m, g);
        maybe_collect(m);
        /* 5. sparse 512 KiB blobs, cross-region refs written, dropped at once */
        for (int it = 0; it < BLOB_ITERS; it++) {
            Blob *b = blob_new(m, 1, BLOB_BYTES, BLOB_NREF);
            for (int k = 0; k < BLOB_WRITES; k++)
                blob_store(m, b, (uint32_t)((k * 127) % (int)BLOB_NREF), chain[(size_t)k % N_PERS]);
            blob_drop(b);
            if ((it & 3) == 0) maybe_collect(m);
        }
        collect(m);
        printf("    round %d: wall %7.1f ms  rss_peak %7ld kB  live %7.2f MB  cap %7.2f MB  ratio %5.2f\n",
               rd + 1, (now_ns() - r0) / 1e6, rss_peak_kb(), (double)m->bytes / (double)MB,
               (double)m->capBytes / (double)MB, (double)m->capBytes / (double)m->bytes);
        fflush(stdout);
    }
    (void)t0;
    printf("    acc=%lld\n", (long long)acc);
}

/* ---------- report ---------- */
static void report(Model *m, double wall_ms) {
    qsort(m->step_us, m->nstep, sizeof(double), cmp_d);
    double med = m->nstep ? m->step_us[m->nstep / 2] : 0.0;
    double p99 = m->nstep ? m->step_us[(size_t)((double)m->nstep * 0.99)] : 0.0;
    double p999 = m->nstep ? m->step_us[(size_t)((double)m->nstep * 0.999)] : 0.0;
    double p9999 = m->nstep ? m->step_us[(size_t)((double)m->nstep * 0.9999)] : 0.0;
    double mx = m->nstep ? m->step_us[m->nstep - 1] : 0.0;
    double live = (double)m->peakLive / (double)MB;
    long peak = rss_peak_kb();
    printf("[%s] wall %8.1f ms | gc %7.1f ms (%4.1f%%) | collects %4zu steps %6zu | step med %6.2f p99 %6.2f p99.9 %7.2f p99.99 %8.2f max %9.2f us | peak %7ld kB | live %6.2f MB | peak/live %5.2f | rss_now %7ld kB | copied %6.1f MB | rs_inc %llu rs_dec %llu | max mark/sweep/reclaim %.2f/%.2f/%.2f us\n",
           m->kind == MODEL_CUR ? "cur" : "new", wall_ms, m->gc_ns / 1e6, 100.0 * m->gc_ns / (wall_ms * 1e6),
           m->ncollections, m->nstep, med, p99, p999, p9999, mx, peak, live, (double)peak / 1024.0 / (live > 0 ? live : 1), rss_now_kb(),
           (double)m->copied / (double)MB, (unsigned long long)m->rsIncN, (unsigned long long)m->rsDecN, m->mxMark, m->mxSweep, m->mxReclaim);
}

int main(int argc, char **argv) {
    int kind = (argc > 1 && !strcmp(argv[1], "new")) ? MODEL_NEW : MODEL_CUR;
    int rounds = (argc > 2) ? atoi(argv[2]) : 3;
    if (rounds < 1) rounds = 3;
    printf("model=%s rounds=%d  (baseline rss %ld kB)\n", kind == MODEL_NEW ? "new" : "cur", rounds, rss_peak_kb());
    Model *m = model_new(kind);
    if (argc > 3) m->growFactor = atof(argv[3]);
    printf("grow factor %.2f\n", m->growFactor);
    double t0 = now_ns();
    run_workload(m, rounds);
    double wall = (now_ns() - t0) / 1e6;
    report(m, wall);
    return 0;
}
