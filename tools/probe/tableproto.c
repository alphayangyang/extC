/* tableproto.c -- hash table storage models over one workload.
 *
 *   cur    : extC hashMap today, dynamic. 17 B of index columns per bucket
 *            (keys/tag/slot/bucketOfDense) plus 20 B of dense value and slot
 *            metadata per element, pow2 capacity, whole-table rebuild.
 *   curpre : same layout pre-sized with withCap(n) (bench h1 does this).
 *   flat2  : packed 24 B buckets (tag,key,value), pow2 capacity, load 0.7,
 *            growth 2x, full rehash.
 *   flat2pre: same layout, pre-sized.
 *   inc    : packed buckets, fastrange index, growth 1.5x, incremental
 *            migration in batches of 8 buckets per operation, drained old
 *            pages returned with MADV_DONTNEED as the cursor passes.  The
 *            old table stays readable only while every bucket after the
 *            cursor is untouched, so a run that stops mid-migration must
 *            finish draining before any lookup.
 *   eh     : extendible hashing on a reserved slab: one directory entry per
 *            page of EH_SLOTS 16 B slots (key, value; key 0 is the empty
 *            sentinel), split at EH_SPLIT_AT load.  A page splits one at a
 *            time into two pages resolved by one more hash bit, so there is
 *            no whole-table rehash: the peak is the live bytes plus a page
 *            and the longest step is one page of re-insertions plus one
 *            directory doubling.  -DEH_PAGE / -DEH_SLOTS / -DEH_SPLIT_AT
 *            are the shape knobs (default 4096 / 255 / 75% load).
 *
 * Build: cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -o tableproto tableproto.c
 * Run:   taskset -c 6 ./tableproto <mode> <n>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#define EMPTY 0
#define FULL  1
#define BATCH 8

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long vmhwm_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; long kb = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmHWM:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; }
    fclose(f); return kb;
}

static uint64_t sm_state = 0x2545F4914F6CDD1Dull;
static uint64_t sm_next(void)
{
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline uint64_t hk(int64_t k)
{
    uint64_t z = (uint64_t)k * 0x9E3779B97F4A7C15ull;
    z ^= z >> 29; z *= 0xBF58476D1CE4E5B9ull; z ^= z >> 32; return z;
}
static void *anew(size_t bytes) { return calloc(1, bytes); }

/* ---------------- cur ---------------- */
typedef struct {
    int64_t *keys; uint8_t *tag; int32_t *slot; int32_t *bod; int64_t cap;
    int64_t *vals; int32_t *rev; int64_t *ent; int64_t n; double worst;
} Cur;

static void cur_arrays(Cur *m, int64_t cap)
{
    m->keys = anew((size_t)cap * 8); m->tag = anew((size_t)cap);
    m->slot = anew((size_t)cap * 4); m->bod = anew((size_t)cap * 4);
    m->vals = anew((size_t)cap * 8); m->rev = anew((size_t)cap * 4);
    m->ent = anew((size_t)cap * 8); m->cap = cap;
}
static void cur_rebuild(Cur *m, int64_t ncap)
{
    double t0 = now_ms();
    Cur t; memset(&t, 0, sizeof t); cur_arrays(&t, ncap);
    for (int64_t i = 0; i < m->n; i++) { t.vals[i] = m->vals[i]; t.ent[i] = m->ent[i]; }
    for (int64_t h = 0; h < m->cap; h++) {
        if (m->tag[h] != FULL) continue;
        int64_t k = m->keys[h], d = m->slot[h];
        uint64_t p = hk(k) & (uint64_t)(ncap - 1);
        while (t.tag[p] == FULL) p = (p + 1) & (uint64_t)(ncap - 1);
        t.tag[p] = FULL; t.keys[p] = k; t.slot[p] = (int32_t)d; t.bod[d] = (int32_t)p; t.rev[d] = (int32_t)p;
    }
    t.n = m->n;
    free(m->keys); free(m->tag); free(m->slot); free(m->bod);
    free(m->vals); free(m->rev); free(m->ent);
    *m = t;
    double dt = now_ms() - t0; if (dt > m->worst) m->worst = dt;
}
static void cur_init(Cur *m, int64_t cap) { memset(m, 0, sizeof *m); cur_arrays(m, cap); }
static void cur_put(Cur *m, int64_t k, int64_t v)
{
    if ((m->n + 1) * 10 >= m->cap * 7) cur_rebuild(m, m->cap * 2);
    uint64_t h = hk(k) & (uint64_t)(m->cap - 1);
    while (m->tag[h] == FULL) {
        if (m->keys[h] == k) { m->vals[m->slot[h]] = v; return; }
        h = (h + 1) & (uint64_t)(m->cap - 1);
    }
    int64_t d = m->n++;
    m->tag[h] = FULL; m->keys[h] = k; m->slot[h] = (int32_t)d; m->bod[d] = (int32_t)h;
    m->vals[d] = v; m->rev[d] = (int32_t)h; m->ent[d] = (uint64_t)(d + 1);
}
static int cur_get(Cur *m, int64_t k, int64_t *out)
{
    uint64_t h = hk(k) & (uint64_t)(m->cap - 1);
    while (m->tag[h] != EMPTY) {
        if (m->tag[h] == FULL && m->keys[h] == k) { *out = m->vals[m->slot[h]]; return 1; }
        h = (h + 1) & (uint64_t)(m->cap - 1);
    }
    return 0;
}

/* ---------------- packed buckets ---------------- */
typedef struct { int64_t k, v; uint8_t tag; } Bucket;  /* 24 B aligned */

typedef struct {
    Bucket *b; int64_t cap, n;
    Bucket *ob; int64_t ocap, opos;
    int pow2;
    double worst;
} Tab;

static int64_t tab_index(Tab *t, uint64_t h, int64_t cap)
{
    if (t->pow2) return (int64_t)(h & (uint64_t)(cap - 1));
    return (int64_t)(((__uint128_t)h * (uint64_t)cap) >> 64);
}
static Bucket *tab_arr(int64_t cap) { return (Bucket *)anew((size_t)cap * sizeof(Bucket)); }
static void tab_alloc(Tab *t, int64_t cap)
{
    t->b = tab_arr(cap); t->cap = cap; t->n = 0;
}
static inline __attribute__((always_inline)) void tab_insert(Tab *t, int64_t k, int64_t v)
{
    int64_t h = tab_index(t, hk(k), t->cap);
    for (;;) {
        Bucket *b = &t->b[h];
        if (b->tag != FULL) { b->tag = FULL; b->k = k; b->v = v; t->n++; return; }
        if (b->k == k) { b->v = v; return; }
        if (++h == t->cap) h = 0;
    }
}
static inline __attribute__((always_inline)) int tab_find(Tab *t, int64_t k, int64_t *v)
{
    if (t->b) {
        int64_t h = tab_index(t, hk(k), t->cap);
        while (t->b[h].tag != EMPTY) {
            if (t->b[h].tag == FULL && t->b[h].k == k) { *v = t->b[h].v; return 1; }
            if (++h == t->cap) h = 0;
        }
    }
    if (t->ob) {
        int64_t h = tab_index(t, hk(k), t->ocap);
        while (t->ob[h].tag != EMPTY) {
            if (t->ob[h].tag == FULL && t->ob[h].k == k) { *v = t->ob[h].v; return 1; }
            if (++h == t->ocap) h = 0;
        }
    }
    return 0;
}
static int64_t tab_grow_cap(int64_t cap) { return cap + cap / 2; }

static void tab_put_flat(Tab *t, int64_t k, int64_t v)
{
    if ((t->n + 1) * 10 >= t->cap * 7) {
        double t0 = now_ms();
        int64_t ncap = t->cap * 2;
        Bucket *nb = tab_arr(ncap);
        Tab nt; memset(&nt, 0, sizeof nt); nt.b = nb; nt.cap = ncap; nt.pow2 = t->pow2;
        for (int64_t i = 0; i < t->cap; i++)
            if (t->b[i].tag == FULL) tab_insert(&nt, t->b[i].k, t->b[i].v);
        free(t->b); t->b = nb; t->cap = ncap; t->n = nt.n;
        double dt = now_ms() - t0; if (dt > t->worst) t->worst = dt;
    }
    tab_insert(t, k, v);
}

static void tab_drop_old_prefix(Tab *t, int64_t upto)
{
    long pg = sysconf(_SC_PAGESIZE);
    int64_t bytes = upto * (int64_t)sizeof(Bucket);
    bytes = (bytes / pg) * pg;
    if (bytes > 0) madvise(t->ob, (size_t)bytes, MADV_DONTNEED);
}

static void tab_put_inc(Tab *t, int64_t k, int64_t v)
{
    if (!t->ob && (t->n + 1) * 10 >= t->cap * 7) {
        int64_t oc = t->cap; Bucket *ob = t->b;
        t->ob = ob; t->ocap = oc; t->opos = 0;
        tab_alloc(t, tab_grow_cap(oc));
    }
    if (t->ob) {
        double t0 = now_ms();
        int moved = 0;
        while (t->opos < t->ocap && moved < BATCH) {
            int64_t i = t->opos++;
            if (t->ob[i].tag == FULL) { tab_insert(t, t->ob[i].k, t->ob[i].v); t->ob[i].tag = EMPTY; moved++; }
            if (i > 0 && (i & 4095) == 0) tab_drop_old_prefix(t, i);
        }
        if (t->opos >= t->ocap) {
            free(t->ob); t->ob = NULL; t->ocap = 0; t->opos = 0;
        }
        double dt = now_ms() - t0; if (dt > t->worst) t->worst = dt;
    }
    tab_insert(t, k, v);
}

/* ---------------- extendible hashing on a reserved slab --------------- */
#ifndef EH_PAGE
#define EH_PAGE 4096              /* one page of the table */
#endif
#ifndef EH_SLOTS
#define EH_SLOTS 255              /* 255 * 16 B + 8 B header = 4088 B, one page */
#endif
#ifndef EH_SPLIT_AT
#define EH_SPLIT_AT ((EH_SLOTS * 3) / 4)   /* split a page above 75% load */
#endif
#define EH_PEND 4096              /* worklist of entries waiting for a home */
typedef struct { int64_t k, v; } EB;      /* k == 0 is the empty sentinel */
typedef struct { int32_t pd, n; EB b[EH_SLOTS]; } EPage;
_Static_assert(sizeof(EPage) <= EH_PAGE, "an EH page must fit in one page");
typedef struct {
    EPage *base; int32_t *dir; int d; int64_t npages, splits;
    EB pend[EH_PEND]; int64_t npend;
    double worst, spanms;
} EH;

static inline EPage *eh_page(EH *m, int64_t i) { return (EPage *)((char *)m->base + i * EH_PAGE); }
static inline int64_t eh_slot_h(uint64_t h) { return (int64_t)(((h >> 32) * (uint64_t)EH_SLOTS) >> 32); }
static inline int64_t eh_di_h(EH *m, uint64_t h) { return (int64_t)(h & (((uint64_t)1 << m->d) - 1)); }
static inline void eh_defer(EH *m, int64_t k, int64_t v)
{
    m->pend[m->npend].k = k; m->pend[m->npend].v = v; m->npend++;
}

static int64_t eh_newpage(EH *m, int64_t pd)
{
    int64_t i = m->npages++;
    EPage *p = eh_page(m, i);
    memset(p, 0, EH_PAGE);
    p->pd = (int32_t)pd;
    return i;
}

static int eh_find(EH *m, int64_t k, int64_t *v)
{
    uint64_t h = hk(k);
    int64_t pi = m->dir[eh_di_h(m, h)];
    __builtin_prefetch((char *)m->base + pi * EH_PAGE);
    EPage *p = eh_page(m, pi);
    int64_t s = eh_slot_h(h);
    for (int64_t i = 0; i < EH_SLOTS; i++) {
        EB *e = &p->b[s];
        if (e->k == 0) return 0;
        if (e->k == k) { *v = e->v; return 1; }
        if (++s == EH_SLOTS) s = 0;
    }
    return 0;
}

/* Split the page that holds key k. The page aliases 2^(d - pd) directory
   entries; only the aliases that take the new bit move to the new page, so a
   split costs one page of entries plus O(aliases) directory writes. */
static void eh_split(EH *m, int64_t k)
{
    double t0 = now_ms();
    uint64_t h = hk(k);
    int64_t di = eh_di_h(m, h);
    EPage *p = eh_page(m, m->dir[di]);
    if (p->pd == m->d) {
        int64_t half = (int64_t)1 << m->d;
        int32_t *ndir = (int32_t *)anew((size_t)(half * 2) * sizeof(int32_t));
        for (int64_t i = 0; i < half; i++) { ndir[i] = m->dir[i]; ndir[i + half] = m->dir[i]; }
        free(m->dir); m->dir = ndir; m->d++;
        di = eh_di_h(m, h);
    }
    int64_t pd = p->pd;
    int64_t bit = (int64_t)1 << pd;
    int64_t cnt = (int64_t)1 << (m->d - pd);
    int64_t base = di & (bit - 1);
    int64_t npi = eh_newpage(m, pd + 1);
    p->pd = (int32_t)(pd + 1);
    for (int64_t j = 0; j < cnt; j++) {
        int64_t idx = base + (j << pd);
        if (idx & bit) m->dir[idx] = (int32_t)npi;
    }
    for (int64_t i = 0; i < EH_SLOTS; i++) {
        if (p->b[i].k != 0) { eh_defer(m, p->b[i].k, p->b[i].v); p->b[i].k = 0; }
    }
    p->n = 0;
    m->splits++;
    double dt = now_ms() - t0; if (dt > m->worst) m->worst = dt; m->spanms += dt;
}
static void eh_put(EH *m, int64_t k, int64_t v)
{
    eh_defer(m, k, v);
    while (m->npend > 0) {
        int64_t kk = m->pend[m->npend - 1].k, vv = m->pend[m->npend - 1].v;
        m->npend--;
        uint64_t h = hk(kk);
        int64_t di = eh_di_h(m, h);
        __builtin_prefetch((char *)m->base + (int64_t)m->dir[di] * EH_PAGE);
        EPage *p = eh_page(m, m->dir[di]);
        int64_t s = eh_slot_h(h);
        int done = 0;
        for (int64_t i = 0; i < EH_SLOTS; i++) {
            EB *e = &p->b[s];
            if (e->k == kk) { e->v = vv; done = 1; break; }
            if (e->k == 0) {
                if (p->n >= EH_SPLIT_AT) break;
                e->k = kk; e->v = vv; p->n++; done = 1; break;
            }
            if (++s == EH_SLOTS) s = 0;
        }
        if (!done) { eh_defer(m, kk, vv); eh_split(m, kk); }
    }
}

static void eh_init(EH *m, int64_t reserve)
{
    memset(m, 0, sizeof *m);
    m->base = (EPage *)mmap(NULL, (size_t)reserve, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m->base == MAP_FAILED) { fprintf(stderr, "mmap failed"); exit(1); }
    m->d = 4;
    m->dir = (int32_t *)anew((size_t)16 * sizeof(int32_t));
    for (int i = 0; i < 16; i++) m->dir[i] = (int32_t)eh_newpage(m, 4);
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "cur";
    int64_t n = argc > 2 ? atoll(argv[2]) : 4000000;
    int64_t q = n, sum = 0, hits = 0;
    int64_t *keys = malloc((size_t)n * 8);
    for (int64_t i = 0; i < n; i++) keys[i] = (int64_t)(sm_next() % 1000000007ull) + 1;
    double t0, t1; long peak; double worst; int64_t cap_end = 0;

    if (strcmp(mode, "cur") == 0 || strcmp(mode, "curpre") == 0) {
        Cur m; int64_t cap0 = 1024;
        if (strcmp(mode, "curpre") == 0) { while (cap0 < n * 2) cap0 *= 2; }
        cur_init(&m, cap0);
        t0 = now_ms();
        for (int64_t i = 0; i < n; i++) cur_put(&m, keys[i], keys[i] * 3 + 1);
        double tmid = now_ms();
        for (int64_t i = 0; i < q; i++) { int64_t v; if (cur_get(&m, keys[i], &v)) { sum += v; hits++; } }
        t1 = now_ms(); printf("  ins=%.1f ms get=%.1f ms\n", tmid - t0, t1 - tmid);
        peak = vmhwm_kb(); worst = m.worst; cap_end = m.cap;
    } else if (strcmp(mode, "eh") == 0) {
        EH m; eh_init(&m, (int64_t)8 << 30);
        t0 = now_ms();
        for (int64_t i = 0; i < n; i++) eh_put(&m, keys[i], keys[i] * 3 + 1);
        double tmid = now_ms();
        for (int64_t i = 0; i < q; i++) { int64_t v; if (eh_find(&m, keys[i], &v)) { sum += v; hits++; } }
        t1 = now_ms(); printf("  ins=%.1f ms get=%.1f ms splits=%lld pages=%lld span=%.1f\n", tmid - t0, t1 - tmid, (long long)m.splits, (long long)m.npages, m.spanms);
        peak = vmhwm_kb(); worst = m.worst; cap_end = m.npages * EH_SLOTS;
    } else {
        Tab t; memset(&t, 0, sizeof t);
        t.pow2 = (strcmp(mode, "flat2") == 0 || strcmp(mode, "flat2pre") == 0);
        if (strcmp(mode, "flat2pre") == 0) { int64_t c = 1024; while (c * 7 < n * 10) c *= 2; tab_alloc(&t, c); }
        else tab_alloc(&t, 1024);
        t0 = now_ms();
        for (int64_t i = 0; i < n; i++) {
            if (t.pow2) tab_put_flat(&t, keys[i], keys[i] * 3 + 1);
            else tab_put_inc(&t, keys[i], keys[i] * 3 + 1);
        }
        while (t.ob) tab_put_inc(&t, keys[0], keys[0] * 3 + 1);
        double tmid = now_ms();
        for (int64_t i = 0; i < q; i++) { int64_t v; if (tab_find(&t, keys[i], &v)) { sum += v; hits++; } }
        t1 = now_ms(); printf("  ins=%.1f ms get=%.1f ms\n", tmid - t0, t1 - tmid);
        peak = vmhwm_kb(); worst = t.worst; cap_end = t.cap;
    }
    printf("mode=%-6s n=%lld wall=%7.1f ms peak=%7ld kB hits=%lld sum=%lld worst_struct=%8.3f ms cap=%lld\n",
           mode, (long long)n, t1 - t0, peak, (long long)hits, (long long)sum, worst, (long long)cap_end);
    free(keys);
    return 0;
}