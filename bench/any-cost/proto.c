/* Prototype v3 for `any`: descriptor carries a dense integer id, so
 *   - a downcast is one integer compare,
 *   - an N-way type match can be a jump table instead of a compare chain,
 *   - printing / copying / equality / marking stay one shared data-driven
 *     routine (no vtables, no function pointers, no per-type code).
 *
 * Every dynamic result is checked against the hand-written static code on the
 * same data before its time is reported.
 */
#define _POSIX_C_SOURCE 199309L
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------- descriptor */

enum { K_NONE, K_I64, K_F64, K_TEXT, K_OBJ, K_ANY };
enum { F_HAS_REF = 1u };

typedef struct Desc Desc;

typedef struct {
    const char *name;
    int32_t off;
    int32_t kind;      /* K_* of the field, flattened */
    const Desc *desc;  /* for K_OBJ / K_ANY nesting */
} Field;

typedef struct {
    uint32_t litOff;   /* byte offset into the type's literal blob */
    uint32_t litLen;
    int32_t off;
    int32_t kind;
    const struct Desc *desc;  /* for K_OBJ: the nested type */
} Part;

struct Desc {
    uint32_t id;       /* dense: a match becomes a jump table */
    uint32_t size;
    uint32_t nfield;
    uint32_t flags;
    int32_t kind;
    const char *name;
    const Field *fields;
    const uint64_t *words;   /* (off << 8) | kind, one load per field */
    const Part *parts;       /* literal chunk + slot, precompiled */
    uint32_t npart;
    const char *lits;
    int32_t homog;    /* K_* when every field has that kind, else K_NONE */
};

typedef struct {
    const void *p;
    const Desc *d;
} Any;

#define ANY_OF(T, ptr) ((Any){ (const void *)(ptr), &T##_desc })

/* --------------------------------------------------------------- payload */

typedef struct {
    int64_t seq;
    double score;
} item;

typedef struct {
    int64_t id;
    item it;
    const char *label;
    Any next;
} rec;

/* four small shapes with an `area` method, for the method-call benchmark */
typedef struct { int64_t w, h; } rect;
typedef struct { int64_t r; } circle;
typedef struct { int64_t a, b, c; } tri;
typedef struct { int64_t n; } line;

typedef struct {
    int64_t a, b, c, d;
} quad;

/* ----------------------------------------------------------- descriptors */

static const Desc desc_i64, desc_f64, desc_text, desc_any, item_desc, rec_desc, quad_desc;

static const Field quad_fields[] = {
    { "a", (int32_t)offsetof(quad, a), K_I64, &desc_i64 },
    { "b", (int32_t)offsetof(quad, b), K_I64, &desc_i64 },
    { "c", (int32_t)offsetof(quad, c), K_I64, &desc_i64 },
    { "d", (int32_t)offsetof(quad, d), K_I64, &desc_i64 },
};
static const uint64_t quad_words[] = {
    ((uint64_t)offsetof(quad, a) << 8) | (uint64_t)K_I64,
    ((uint64_t)offsetof(quad, b) << 8) | (uint64_t)K_I64,
    ((uint64_t)offsetof(quad, c) << 8) | (uint64_t)K_I64,
    ((uint64_t)offsetof(quad, d) << 8) | (uint64_t)K_I64,
};
static const Desc quad_desc = { 6, sizeof(quad), 4, 0, K_OBJ, "quad",
                                quad_fields, quad_words, NULL, 0, NULL, K_I64 };

static const Field item_fields[] = {
    { "seq", (int32_t)offsetof(item, seq), K_I64, &desc_i64 },
    { "score", (int32_t)offsetof(item, score), K_F64, &desc_f64 },
};

static const Field rec_fields[] = {
    { "id", (int32_t)offsetof(rec, id), K_I64, &desc_i64 },
    { "it", (int32_t)offsetof(rec, it), K_OBJ, &item_desc },
    { "label", (int32_t)offsetof(rec, label), K_TEXT, &desc_text },
    { "next", (int32_t)offsetof(rec, next), K_ANY, &desc_any },
};

static const Desc desc_i64 = { 0, sizeof(int64_t), 0, 0, K_I64, "i64", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc desc_f64 = { 1, sizeof(double), 0, 0, K_F64, "f64", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc desc_text = { 2, sizeof(const char *), 0, 0, K_TEXT, "text", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc desc_any = { 3, sizeof(Any), 0, F_HAS_REF, K_ANY, "any", NULL, NULL, NULL, 0, NULL, K_NONE };

static const uint64_t item_words[] = {
    (0ull << 8) | (uint64_t)K_I64,
    ((uint64_t)offsetof(item, score) << 8) | (uint64_t)K_F64,
};
static const uint64_t rec_words[] = {
    ((uint64_t)offsetof(rec, id) << 8) | (uint64_t)K_I64,
    ((uint64_t)offsetof(rec, it) << 8) | (uint64_t)K_OBJ,
    ((uint64_t)offsetof(rec, label) << 8) | (uint64_t)K_TEXT,
    ((uint64_t)offsetof(rec, next) << 8) | (uint64_t)K_ANY,
};

/* print templates: the blob holds the literal chunks back to back; each part
 * copies its chunk and then formats one slot. */
static const char item_lits[] = "{ }";
static const char rec_lits[] = "{   }";

static const Part item_parts[] = {
    { 0, 1, (int32_t)offsetof(item, seq), K_I64, NULL },
    { 1, 1, (int32_t)offsetof(item, score), K_F64, NULL },
    { 2, 1, 0, K_NONE, NULL },
};

static const Part rec_parts[] = {
    { 0, 1, (int32_t)offsetof(rec, id), K_I64, NULL },
    { 1, 1, (int32_t)offsetof(rec, it), K_OBJ, &item_desc },
    { 2, 1, (int32_t)offsetof(rec, label), K_TEXT, NULL },
    { 3, 1, (int32_t)offsetof(rec, next), K_ANY, &desc_any },
    { 4, 1, 0, K_NONE, NULL },
};

static const Desc item_desc = { 4, sizeof(item), 2, 0, K_OBJ, "item",
                                item_fields, item_words, item_parts, 3, item_lits, K_NONE };
static const Desc rec_desc = { 5, sizeof(rec), 4, F_HAS_REF, K_OBJ, "rec",
                               rec_fields, rec_words, rec_parts, 5, rec_lits, K_NONE };

static const Desc the_rect = { 11, sizeof(rect), 0, 0, K_OBJ, "rect", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc the_circle = { 12, sizeof(circle), 0, 0, K_OBJ, "circle", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc the_tri = { 13, sizeof(tri), 0, 0, K_OBJ, "tri", NULL, NULL, NULL, 0, NULL, K_NONE };
static const Desc the_line = { 14, sizeof(line), 0, 0, K_OBJ, "line", NULL, NULL, NULL, 0, NULL, K_NONE };

/* ------------------------------------------------------------ formatting */

static const char DIGITS[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

static inline uint32_t div100(uint64_t u) {
    return (uint32_t)((u * 1374389535ull) >> 37);   /* floor(u / 100), u < 2^63 */
}

static char *fmt_i64(char *o, int64_t v) {
    char tmp[20];
    char *b = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    while (u >= 100) {
        unsigned r = (unsigned)(u - (uint64_t)div100(u) * 100u);
        u = div100(u);
        b -= 2;
        memcpy(b, DIGITS + r * 2, 2);
    }
    if (u < 10) *--b = (char)('0' + (int)u);
    else {
        b -= 2;
        memcpy(b, DIGITS + u * 2, 2);
    }
    if (v < 0) *--b = '-';
    size_t len = (size_t)(tmp + sizeof tmp - b);
    memcpy(o, b, len);
    return o + len;
}

static char *fmt_f64_3(char *o, double v) {
    uint64_t scaled = (uint64_t)(v * 1000.0 + 0.5);
    uint64_t whole = div100(div100(scaled) * 100ull + (scaled - (scaled / 100) * 100)) / 10;
    whole = scaled / 1000;
    unsigned frac = (unsigned)(scaled - whole * 1000);
    o = fmt_i64(o, (int64_t)whole);
    *o++ = '.';
    *o++ = (char)('0' + (int)(frac / 100));
    *o++ = (char)('0' + (int)((frac / 10) % 10));
    *o++ = (char)('0' + (int)(frac % 10));
    return o;
}

static char *fmt_text(char *o, const char *s) {
    while (*s) *o++ = *s++;
    return o;
}

/* ------------------------------------------------------ static reference */

static char *static_put(char *o, const rec *r) {
    *o++ = '{';
    o = fmt_i64(o, r->id);
    *o++ = ' ';
    *o++ = '{';
    o = fmt_i64(o, r->it.seq);
    *o++ = ' ';
    o = fmt_f64_3(o, r->it.score);
    *o++ = '}';
    *o++ = ' ';
    o = fmt_text(o, r->label);
    *o++ = ' ';
    if (r->next.p) o = static_put(o, (const rec *)r->next.p);
    else *o++ = '-';
    *o++ = '}';
    return o;
}

static int static_eq(const rec *a, const rec *b) {
    if (a->id != b->id || a->it.seq != b->it.seq || a->it.score != b->it.score) return 0;
    if (strcmp(a->label, b->label) != 0) return 0;
    if (!a->next.p || !b->next.p) return a->next.p == b->next.p;
    return static_eq((const rec *)a->next.p, (const rec *)b->next.p);
}

static int static_mark(const rec *r) {
    int n = 1;
    for (const rec *p = r; p->next.p; p = (const rec *)p->next.p) n++;
    return n;
}

/* ------------------------------------------------------- dynamic walkers */

static char *slot_put(char *o, const char *base, int32_t off, int32_t kind) {
    const char *p = base + off;
    switch (kind) {
    case K_I64: return fmt_i64(o, *(const int64_t *)p);
    case K_F64: return fmt_f64_3(o, *(const double *)p);
    case K_TEXT: return fmt_text(o, *(const char *const *)p);
    default: return o;
    }
}

static char *any_put_table(char *o, Any a) {
    if (!a.d) { *o++ = '-'; return o; }
    switch (a.d->kind) {
    case K_I64: return fmt_i64(o, *(const int64_t *)a.p);
    case K_F64: return fmt_f64_3(o, *(const double *)a.p);
    case K_TEXT: return fmt_text(o, *(const char *const *)a.p);
    case K_ANY: return any_put_table(o, *(const Any *)a.p);
    case K_OBJ: {
        const char *base = (const char *)a.p;
        *o++ = '{';
        for (uint32_t i = 0; i < a.d->nfield; i++) {
            const Field *f = &a.d->fields[i];
            if (i) *o++ = ' ';
            if (f->kind == K_OBJ) o = any_put_table(o, (Any){ base + f->off, f->desc });
            else if (f->kind == K_ANY) {
                Any sub = *(const Any *)(base + f->off);
                if (!sub.d) *o++ = '-';
                else o = any_put_table(o, sub);
            } else o = slot_put(o, base, f->off, f->kind);
        }
        *o++ = '}';
        return o;
    }
    }
    return o;
}

static char *any_put_words(char *o, Any a) {
    if (!a.d) { *o++ = '-'; return o; }
    switch (a.d->kind) {
    case K_I64: return fmt_i64(o, *(const int64_t *)a.p);
    case K_F64: return fmt_f64_3(o, *(const double *)a.p);
    case K_TEXT: return fmt_text(o, *(const char *const *)a.p);
    case K_ANY: return any_put_words(o, *(const Any *)a.p);
    case K_OBJ: {
        const char *base = (const char *)a.p;
        const uint64_t *w = a.d->words;
        *o++ = '{';
        if (a.d->homog == K_I64) {          /* every field the same kind: no switch */
            for (uint32_t i = 0; i < a.d->nfield; i++) {
                if (i) *o++ = ' ';
                o = fmt_i64(o, *(const int64_t *)(base + (w[i] >> 8)));
            }
            *o++ = '}';
            return o;
        }
        if (a.d->homog == K_F64) {
            for (uint32_t i = 0; i < a.d->nfield; i++) {
                if (i) *o++ = ' ';
                o = fmt_f64_3(o, *(const double *)(base + (w[i] >> 8)));
            }
            *o++ = '}';
            return o;
        }
        for (uint32_t i = 0; i < a.d->nfield; i++) {
            uint64_t word = w[i];
            int32_t off = (int32_t)(word >> 8);
            int32_t kind = (int32_t)(word & 0xffu);
            if (i) *o++ = ' ';
            if (kind == K_OBJ) o = any_put_words(o, (Any){ base + off, a.d->fields[i].desc });
            else if (kind == K_ANY) {
                Any sub = *(const Any *)(base + off);
                if (!sub.d) *o++ = '-';
                else o = any_put_words(o, sub);
            } else o = slot_put(o, base, off, kind);
        }
        *o++ = '}';
        return o;
    }
    }
    return o;
}

static char *any_put_tmpl(char *o, Any a) {
    if (!a.d) { *o++ = '-'; return o; }
    switch (a.d->kind) {
    case K_I64: return fmt_i64(o, *(const int64_t *)a.p);
    case K_F64: return fmt_f64_3(o, *(const double *)a.p);
    case K_TEXT: return fmt_text(o, *(const char *const *)a.p);
    case K_ANY: return any_put_tmpl(o, *(const Any *)a.p);
    case K_OBJ: {
        const char *base = (const char *)a.p;
        for (uint32_t i = 0; i < a.d->npart; i++) {
            const Part *t = &a.d->parts[i];
            if (t->litLen) {
                const char *src = a.d->lits + t->litOff;
                if (t->litLen == 1) *o++ = *src;
                else if (t->litLen == 2) { o[0] = src[0]; o[1] = src[1]; o += 2; }
                else { memcpy(o, src, t->litLen); o += t->litLen; }
            }
            switch (t->kind) {
            case K_NONE: break;
            case K_I64: o = fmt_i64(o, *(const int64_t *)(base + t->off)); break;
            case K_F64: o = fmt_f64_3(o, *(const double *)(base + t->off)); break;
            case K_TEXT: o = fmt_text(o, *(const char *const *)(base + t->off)); break;
            case K_OBJ: o = any_put_tmpl(o, (Any){ base + t->off, t->desc }); break;
            case K_ANY: {
                Any sub = *(const Any *)(base + t->off);
                if (!sub.d) *o++ = '-';
                else o = any_put_tmpl(o, sub);
                break;
            }
            }
        }
        return o;
    }
    }
    return o;
}

static int any_eq(Any a, Any b) {
    if (!a.d || !b.d) return a.d == b.d;
    if (a.d != b.d) return 0;
    switch (a.d->kind) {
    case K_I64: return *(const int64_t *)a.p == *(const int64_t *)b.p;
    case K_F64: return *(const double *)a.p == *(const double *)b.p;
    case K_TEXT: return strcmp(*(const char *const *)a.p, *(const char *const *)b.p) == 0;
    case K_ANY: return any_eq(*(const Any *)a.p, *(const Any *)b.p);
    case K_OBJ:
        for (uint32_t i = 0; i < a.d->nfield; i++) {
            const Field *f = &a.d->fields[i];
            if (f->kind == K_OBJ || f->kind == K_ANY) {
                if (!any_eq((Any){ (const char *)a.p + f->off, f->desc },
                            (Any){ (const char *)b.p + f->off, f->desc }))
                    return 0;
            } else if (f->kind == K_I64) {
                if (*(const int64_t *)((const char *)a.p + f->off) !=
                    *(const int64_t *)((const char *)b.p + f->off)) return 0;
            } else if (f->kind == K_F64) {
                if (*(const double *)((const char *)a.p + f->off) !=
                    *(const double *)((const char *)b.p + f->off)) return 0;
            } else if (f->kind == K_TEXT) {
                if (strcmp(*(const char *const *)((const char *)a.p + f->off),
                           *(const char *const *)((const char *)b.p + f->off)) != 0)
                    return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int any_mark(Any a) {
    int n = 1;
    if (a.d->kind != K_OBJ) return n;
    for (uint32_t i = 0; i < a.d->nfield; i++) {
        const Field *f = &a.d->fields[i];
        if (f->kind != K_ANY) continue;
        Any sub = *(const Any *)((const char *)a.p + f->off);
        if (sub.p) n += any_mark(sub);
    }
    return n;
}

/* Deep copy needs somewhere to put the nested objects; in extC that is the
 * frame's region (the same arena `new` uses).  A bump pointer is the honest
 * stand-in: one pointer add per copied object. */
static char g_arena[1u << 22];
static size_t g_arena_used;

static void *arena_alloc(size_t n, size_t align) {
    size_t at = (g_arena_used + (align - 1)) & ~(align - 1);
    g_arena_used = at + n;
    return g_arena + at;
}

static void any_copy(void *dst, Any src) {
    if (!src.d) { *(Any *)dst = (Any){ NULL, NULL }; return; }
    switch (src.d->kind) {
    case K_I64: memcpy(dst, src.p, sizeof(int64_t)); return;
    case K_F64: memcpy(dst, src.p, sizeof(double)); return;
    case K_TEXT: memcpy(dst, src.p, sizeof(const char *)); return;
    case K_ANY: {
        Any sub = *(const Any *)src.p;
        if (!sub.p) { *(Any *)dst = sub; return; }
        void *mem = arena_alloc(sub.d->size, 16);
        any_copy(mem, sub);
        *(Any *)dst = (Any){ mem, sub.d };
        return;
    }
    case K_OBJ:
        memcpy(dst, src.p, src.d->size);
        if (!(src.d->flags & F_HAS_REF)) return;
        for (uint32_t i = 0; i < src.d->nfield; i++) {
            const Field *f = &src.d->fields[i];
            if (f->kind != K_ANY) continue;
            Any sub = *(Any *)((char *)dst + f->off);
            if (sub.p) {
                void *mem = arena_alloc(sub.d->size, 16);
                any_copy(mem, sub);
                *(Any *)((char *)dst + f->off) = (Any){ mem, sub.d };
            }
        }
        return;
    }
}

/* -------------------------------------------------- calling a method ---------
 * Same call site, three lowerings:
 *   vtable  — the descriptor carries a function pointer (what `dyn` is)
 *   switch  — closed world: the compiler emits a direct call per type
 *   known   — the site downcasts first, then calls static code
 */

static int64_t area_rect(const void *p) { const rect *q = p; return q->w * q->h; }
static int64_t area_circle(const void *p) { const circle *q = p; return 3 * q->r * q->r; }
static int64_t area_tri(const void *p) { const tri *q = p; return (q->a + q->b + q->c) / 2; }
static int64_t area_line(const void *p) { const line *q = p; return q->n; }

typedef int64_t (*AreaFn)(const void *);

static const Desc the_rect, the_circle, the_tri, the_line;


/* vtable in the descriptor: one pointer per dynamic method */
typedef struct {
    const Desc *d;
    AreaFn area;
} DynDesc;

static const DynDesc dd_rect = { &the_rect, area_rect };
static const DynDesc dd_circle = { &the_circle, area_circle };
static const DynDesc dd_tri = { &the_tri, area_tri };
static const DynDesc dd_line = { &the_line, area_line };

/* closed world: the compiler saw every implementation, so the call site becomes
 * a switch over ids with a direct call per arm (no pointer stored anywhere) */
static __attribute__((noinline, noipa)) int64_t area_switch(const DynDesc *dd, uint32_t id, const void *p) {
    switch (id) {
    case 11: return area_rect(p);
    case 12: return area_circle(p);
    case 13: return area_tri(p);
    case 14: return area_line(p);
    default: return -1;
    }
    (void)dd;
}

/* ------------------------------------------------------------- dispatch */

/* N synthetic types with ids 0..N-1: the site has to pick a static path from
 * a dynamic value.  Four honest models of how a language could do it. */
#define MAXT 256
static Desc g_descs[MAXT];
static Any g_dyn[MAXT];
static int64_t g_pay[MAXT];
static uint32_t g_sid[MAXT];
static uint8_t g_slot[MAXT];      /* jump table: id -> arm */

static int64_t take(const void *p) { return *(const int64_t *)p; }

/* a `match` written as a compare chain, which is what a naive lowering gives */
static __attribute__((noinline, noipa)) int64_t dispatch_linear(const Any *a, int n, int nt) {
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = a[i].d->id;
        int slot = nt - 1;
        for (int j = 0; j < nt; j++) {
            if (g_sid[j] == id) { slot = j; break; }
        }
        acc += take(a[i].p) + slot;
    }
    return acc;
}

/* ids are dense, so a jump table: one indexed load, no compares */
static __attribute__((noinline, noipa)) int64_t dispatch_jump(const Any *a, int n) {
    int64_t acc = 0;
    for (int i = 0; i < n; i++) acc += take(a[i].p) + g_slot[a[i].d->id];
    return acc;
}

/* sparse ids: binary search over the sorted id table */
static __attribute__((noinline, noipa)) int64_t dispatch_bsearch(const Any *a, int n, int nt) {
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = a[i].d->id;
        int lo = 0, hi = nt - 1, slot = 0;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            uint32_t v = g_sid[mid];
            if (v == id) { slot = mid; break; }
            if (v < id) lo = mid + 1; else hi = mid - 1;
        }
        acc += take(a[i].p) + slot;
    }
    return acc;
}

/* station cache: the site remembers the last descriptor it saw (this is what a
 * monomorphic call site degenerates to after the first iteration) */
static __attribute__((noinline, noipa)) int64_t dispatch_cache(const Any *a, int n) {
    int64_t acc = 0;
    const Desc *last = a[0].d;
    uint8_t bias = g_slot[last->id];
    for (int i = 0; i < n; i++) {
        const Desc *d = a[i].d;
        if (d != last) { last = d; bias = g_slot[d->id]; }
        acc += take(a[i].p) + bias;
    }
    return acc;
}

/* ------------------------------------------------------------- timing etc */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double best_of(int rounds, const double *t) {
    double b = t[0];
    for (int i = 1; i < rounds; i++) if (t[i] < b) b = t[i];
    return b;
}

#define N 4096
static rec g_data[N];
static rec g_tail[N];
static Any g_mixed[N];
static const char *g_labels[3] = { "alpha", "beta", "" };

static void build(void) {
    for (int i = 0; i < N; i++) {
        g_data[i].id = i;
        g_data[i].it.seq = i * 7;
        g_data[i].it.score = (double)(i % 1000) + 0.5;
        g_data[i].label = g_labels[i % 3];
        g_data[i].next.p = NULL;
        g_data[i].next.d = NULL;
        g_tail[i].id = 100000 + i;
        g_tail[i].it.seq = -i;
        g_tail[i].it.score = (double)(i % 100) + 0.25;
        g_tail[i].label = "tail";
        g_tail[i].next.p = NULL;
        g_tail[i].next.d = NULL;
    }
    for (int i = 0; i < N; i++) {
        switch (i % 3) {
        case 0: g_mixed[i] = ANY_OF(rec, &g_data[i]); break;
        case 1: g_mixed[i] = ANY_OF(item, &g_data[i].it); break;
        default: g_mixed[i].p = &g_labels[i % 3]; g_mixed[i].d = &desc_text; break;
        }
    }
    for (int i = 0; i < N; i += 8) g_data[i].next = ANY_OF(rec, &g_tail[i]);
}

static __attribute__((noinline, noipa)) int is_rec_ptr(Any w) { return w.d == &rec_desc; }
static __attribute__((noinline, noipa)) int is_rec_id(Any w) { return w.d->id == 5u; }

static Any __attribute__((noinline)) opaque(Any a) {
    __asm__ volatile("" : "+r"(a.p), "+r"(a.d));
    return a;
}

int main(void) {
    build();
    /* A mode for instruction counting: run exactly one print loop, so the
     * difference between two runs is that loop's cost. */
    const char *mode = getenv("EXTC_BENCH_MODE");
    if (mode) {
        const int M = 200000;
        static char mbuf[1024];
        size_t ms = 0;
        if (strcmp(mode, "none") == 0) { printf("mode none\n"); return 0; }
        if (strcmp(mode, "static") == 0) {
            for (int i = 0; i < M; i++) ms += (size_t)(static_put(mbuf, &g_data[i & (N - 1)]) - mbuf);
        } else if (strcmp(mode, "words") == 0) {
            for (int i = 0; i < M; i++)
                ms += (size_t)(any_put_words(mbuf, opaque(ANY_OF(rec, &g_data[i & (N - 1)]))) - mbuf);
        } else if (strcmp(mode, "table") == 0) {
            for (int i = 0; i < M; i++)
                ms += (size_t)(any_put_table(mbuf, opaque(ANY_OF(rec, &g_data[i & (N - 1)]))) - mbuf);
        } else if (strcmp(mode, "method-vt") == 0 || strcmp(mode, "method-sw") == 0) {
            static const DynDesc *dds[4];
            static void *objs[4];
            rect r0 = { 3, 4 };
            circle c0 = { 5 };
            tri t0 = { 3, 4, 5 };
            line l0 = { 7 };
            dds[0] = &dd_rect; objs[0] = &r0;
            dds[1] = &dd_circle; objs[1] = &c0;
            dds[2] = &dd_tri; objs[2] = &t0;
            dds[3] = &dd_line; objs[3] = &l0;
            int64_t a = 0;
            const int M = 200000;
            if (mode[7] == 'v') {
                for (int i = 0; i < M; i++) a += dds[i & 3]->area(objs[i & 3]);
            } else {
                for (int i = 0; i < M; i++) a += area_switch(dds[i & 3], dds[i & 3]->d->id, objs[i & 3]);
            }
            ms += (size_t)a;
        } else if (strcmp(mode, "dispatch") == 0) {
            for (int i = 0; i < MAXT; i++) { g_descs[i] = desc_i64; g_descs[i].id = (uint32_t)i;
                g_pay[i] = i; g_slot[i] = (uint8_t)i; g_sid[i] = (uint32_t)i;
                g_dyn[i].p = &g_pay[i]; g_dyn[i].d = &g_descs[i]; }
            int64_t a = 0;
            for (int r = 0; r < 2000; r++) a += dispatch_jump(g_dyn, MAXT);
            ms += (size_t)a;
        } else if (strcmp(mode, "chain") == 0) {
            for (int i = 0; i < MAXT; i++) { g_descs[i] = desc_i64; g_descs[i].id = (uint32_t)i;
                g_pay[i] = i; g_slot[i] = (uint8_t)i; g_sid[i] = (uint32_t)i;
                g_dyn[i].p = &g_pay[i]; g_dyn[i].d = &g_descs[i]; }
            int64_t a = 0;
            for (int r = 0; r < 200; r++) a += dispatch_linear(g_dyn, MAXT, MAXT);
            ms += (size_t)a;
        }
        printf("mode %s ms=%zu\n", mode, ms);
        return 0;
    }

    /* ---------------- parity ---------------- */
    static char a[512], b[512];
    for (int i = 0; i < N; i++) {
        char *be = static_put(b, &g_data[i]);
        char *t1 = any_put_table(a, ANY_OF(rec, &g_data[i]));
        if ((size_t)(be - b) != (size_t)(t1 - a) || memcmp(a, b, (size_t)(t1 - a)))
            { printf("FAIL table at %d\n", i); return 1; }
        char *t2 = any_put_words(a, ANY_OF(rec, &g_data[i]));
        if ((size_t)(be - b) != (size_t)(t2 - a) || memcmp(a, b, (size_t)(t2 - a)))
            { printf("FAIL words at %d\n", i); return 1; }
        char *t3 = any_put_tmpl(a, ANY_OF(rec, &g_data[i]));
        if ((size_t)(be - b) != (size_t)(t3 - a) || memcmp(a, b, (size_t)(t3 - a)))
            { printf("FAIL tmpl at %d\n", i); return 1; }
        if (any_mark(ANY_OF(rec, &g_data[i])) != static_mark(&g_data[i]))
            { printf("FAIL mark at %d\n", i); return 1; }
        if (any_eq(ANY_OF(rec, &g_data[i]), ANY_OF(rec, &g_tail[i])) !=
            static_eq(&g_data[i], &g_tail[i]))
            { printf("FAIL eq at %d\n", i); return 1; }
        rec cp;
        any_copy(&cp, ANY_OF(rec, &g_data[i]));
        if (cp.id != g_data[i].id || cp.it.seq != g_data[i].it.seq ||
            cp.label != g_data[i].label)
            { printf("FAIL copy at %d\n", i); return 1; }
    }
    printf("parity: table / words / template / mark / eq / copy agree on %d records\n", N);

    /* ---------------- print cost ---------------- */
    {
        const int PR = 2000000;
        static char buf[1024];
        size_t sink = 0;
        double t[3];
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR; i++) sink += (size_t)(static_put(buf, &g_data[i & (N - 1)]) - buf);
            t[r] = (now_ns() - t0) / PR;
        }
        double st = best_of(3, t);
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR; i++)
                sink += (size_t)(any_put_table(buf, opaque(ANY_OF(rec, &g_data[i & (N - 1)]))) - buf);
            t[r] = (now_ns() - t0) / PR;
        }
        double tw = best_of(3, t);
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR; i++)
                sink += (size_t)(any_put_words(buf, opaque(ANY_OF(rec, &g_data[i & (N - 1)]))) - buf);
            t[r] = (now_ns() - t0) / PR;
        }
        double tt = best_of(3, t);
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR; i++)
                sink += (size_t)(any_put_tmpl(buf, opaque(ANY_OF(rec, &g_data[i & (N - 1)]))) - buf);
            t[r] = (now_ns() - t0) / PR;
        }
        double tm = best_of(3, t);
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR; i++)
                sink += (size_t)(any_put_tmpl(buf, g_mixed[i & (N - 1)]) - buf);
            t[r] = (now_ns() - t0) / PR;
        }
        double tmh = best_of(3, t);
        printf("print ns: static=%.1f table=%.1f words=%.1f template=%.1f template-hetero=%.1f (sink=%zu)\n",
               st, tw, tt, tm, tmh, sink);
    }

    /* ---------------- small shapes: per-field overhead ---------------- */
    {
        const int PR2 = 2000000;
        static char sbuf[1024];
        static quad qs[256];
        for (int i = 0; i < 256; i++) { qs[i].a = i; qs[i].b = i * 2; qs[i].c = i * 3; qs[i].d = i * 4; }
        size_t sink2 = 0;
        double best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR2; i++) {
                const quad *q = &qs[i & 255];
                char *o = sbuf;
                *o++ = '{';
                o = fmt_i64(o, q->a); *o++ = ' ';
                o = fmt_i64(o, q->b); *o++ = ' ';
                o = fmt_i64(o, q->c); *o++ = ' ';
                o = fmt_i64(o, q->d);
                *o++ = '}';
                sink2 += (size_t)(o - sbuf);
            }
            double x = (now_ns() - t0) / PR2;
            if (x < best) best = x;
        }
        double q_static = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR2; i++)
                sink2 += (size_t)(any_put_words(sbuf, opaque(ANY_OF(quad, &qs[i & 255]))) - sbuf);
            double x = (now_ns() - t0) / PR2;
            if (x < best) best = x;
        }
        double q_dyn = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR2; i++)
                sink2 += (size_t)(any_put_words(sbuf, opaque(ANY_OF(item, &g_data[i & (N - 1)].it))) - sbuf);
            double x = (now_ns() - t0) / PR2;
            if (x < best) best = x;
        }
        double i_dyn = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR2; i++) {
                const item *p = &g_data[i & (N - 1)].it;
                char *o = sbuf;
                *o++ = '{';
                o = fmt_i64(o, p->seq); *o++ = ' ';
                o = fmt_f64_3(o, p->score);
                *o++ = '}';
                sink2 += (size_t)(o - sbuf);
            }
            double x = (now_ns() - t0) / PR2;
            if (x < best) best = x;
        }
        double i_static = best;
        /* ceiling: the site checks one pointer, then runs static code */
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < PR2; i++) {
                Any w = opaque(ANY_OF(rec, &g_data[i & (N - 1)]));
                if (w.d == &rec_desc) sink2 += (size_t)(static_put(sbuf, (const rec *)w.p) - sbuf);
                else sink2 += (size_t)(any_put_words(sbuf, w) - sbuf);
            }
            double x = (now_ns() - t0) / PR2;
            if (x < best) best = x;
        }
        double ceil = best;
        printf("print ns: quad(4 x i64) static=%.1f dyn=%.1f | item(2 fields) static=%.1f dyn=%.1f | rec site-known=%.1f (sink=%zu)\n",
               q_static, q_dyn, i_static, i_dyn, ceil, sink2);
    }

    /* ---------------- deep copy cost ---------------- */
    {
        const int CR = 2000000;
        rec dst;
        double best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < CR; i++) dst = g_data[i & (N - 1)];
            double x = (now_ns() - t0) / CR;
            if (x < best) best = x;
        }
        double st = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            g_arena_used = 0;
            double t0 = now_ns();
            for (int i = 0; i < CR; i++) {
                g_arena_used = 0;
                any_copy(&dst, opaque(ANY_OF(rec, &g_data[i & (N - 1)])));
            }
            double x = (now_ns() - t0) / CR;
            if (x < best) best = x;
        }
        printf("copy ns: static-assign=%.2f  any-deep-copy=%.2f  (bytes/obj=%zu)\n",
               st, best, sizeof(rec));
    }

    /* ---------------- downcast: pointer compare vs id compare ---------------- */
    {
        const int DC = 40000000;
        volatile int pick = 1;
        double best;
        int64_t acc = 0;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < DC; i++) {
                Any w = ANY_OF(rec, &g_data[i & (N - 1)]);
                acc += is_rec_ptr(w);
            }
            double x = (now_ns() - t0) / DC;
            if (x < best) best = x;
        }
        double byptr = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < DC; i++) {
                Any w = ANY_OF(rec, &g_data[i & (N - 1)]);
                acc += is_rec_id(w);
            }
            double x = (now_ns() - t0) / DC;
            if (x < best) best = x;
        }
        double byid = best;
        printf("downcast ns: desc-pointer=%.2f  desc-id=%.2f (pick=%d acc=%lld)\n",
               byptr, byid, pick, (long long)(acc & 1));
    }

    /* ---------------- eq / mark / small copy ---------------- */
    {
        const int ER = 2000000;
        double best = 1e9;
        int acc = 0;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++) acc += static_eq(&g_data[i & (N - 1)], &g_data[i & (N - 1)]);
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double st_eq = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++)
                acc += any_eq(opaque(ANY_OF(rec, &g_data[i & (N - 1)])),
                              opaque(ANY_OF(rec, &g_data[i & (N - 1)])));
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double dy_eq = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++) acc += static_mark(&g_data[i & (N - 1)]);
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double st_mk = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++) acc += any_mark(opaque(ANY_OF(rec, &g_data[i & (N - 1)])));
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double dy_mk = best;
        quad qs2[64];
        for (int i = 0; i < 64; i++) { qs2[i].a = i; qs2[i].b = i + 1; qs2[i].c = i + 2; qs2[i].d = i + 3; }
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++) { quad t = qs2[i & 63]; qs2[i & 63] = t; }
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double q_sc = best;
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (int i = 0; i < ER; i++) {
                g_arena_used = 0;
                quad t;
                any_copy(&t, opaque(ANY_OF(quad, &qs2[i & 63])));
                qs2[i & 63] = t;
            }
            double x = (now_ns() - t0) / ER;
            if (x < best) best = x;
        }
        double q_dc = best;
        printf("eq/mark ns: eq static=%.2f dyn=%.2f | mark static=%.2f dyn=%.2f | copy quad(32B,no refs) assign=%.2f desc=%.2f  (acc=%d)\n",
               st_eq, dy_eq, st_mk, dy_mk, q_sc, q_dc, acc & 1);
    }

    /* ---------------- method call cost ---------------- */
    {
        const int MC = 2000000;
        static const DynDesc *dds[4];
        static void *objs[4];
        rect r0 = { 3, 4 };
        circle c0 = { 5 };
        tri t0 = { 3, 4, 5 };
        line l0 = { 7 };
        dds[0] = &dd_rect; objs[0] = &r0;
        dds[1] = &dd_circle; objs[1] = &c0;
        dds[2] = &dd_tri; objs[2] = &t0;
        dds[3] = &dd_line; objs[3] = &l0;
        int64_t acc = 0;
        double best;

        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0n = now_ns();
            for (int i = 0; i < MC; i++) {
                const DynDesc *dd = dds[i & 3];
                acc += dd->area(objs[i & 3]);
            }
            double x = (now_ns() - t0n) / MC;
            if (x < best) best = x;
        }
        double vt = best;

        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0n = now_ns();
            for (int i = 0; i < MC; i++) {
                const DynDesc *dd = dds[i & 3];
                acc += area_switch(dd, dd->d->id, objs[i & 3]);
            }
            double x = (now_ns() - t0n) / MC;
            if (x < best) best = x;
        }
        double sw = best;

        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0n = now_ns();
            for (int i = 0; i < MC; i++) {
                const DynDesc *dd = dds[i & 3];
                if (dd->d == &the_rect) acc += area_rect(objs[i & 3]);
                else if (dd->d == &the_circle) acc += area_circle(objs[i & 3]);
                else if (dd->d == &the_tri) acc += area_tri(objs[i & 3]);
                else acc += area_line(objs[i & 3]);
            }
            double x = (now_ns() - t0n) / MC;
            if (x < best) best = x;
        }
        double kn = best;

        /* one concrete type only: the monomorphic case */
        best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t0n = now_ns();
            for (int i = 0; i < MC; i++) acc += area_rect(&r0);
            double x = (now_ns() - t0n) / MC;
            if (x < best) best = x;
        }
        double mono = best;
        printf("method ns: vtable=%.2f closed-switch=%.2f site-known-chain=%.2f direct-static=%.2f (acc=%d)\n",
               vt, sw, kn, mono, (int)(acc & 1));
    }

    /* ---------------- dispatch cost ---------------- */
    for (int i = 0; i < MAXT; i++) {
        g_descs[i] = desc_i64;
        g_descs[i].id = (uint32_t)i;
        g_pay[i] = (int64_t)i * 3;
        g_slot[i] = (uint8_t)(i & 0x7f);
        g_sid[i] = (uint32_t)i;
    }
    static const int sizes[] = { 2, 4, 16, 64, 256 };
    printf("dispatch ns/op:");
    int64_t acc = 0;
    for (int k = 0; k < 5; k++) {
        int nt = sizes[k];
        for (int i = 0; i < MAXT; i++) {
            g_dyn[i].p = &g_pay[i];
            g_dyn[i].d = &g_descs[i % nt];
        }
        volatile int perf = 2000000;
        const int per = perf;
        double t0 = now_ns();
        for (int r = 0; r < per; r++) acc += dispatch_linear(g_dyn, nt, nt);
        double li = (now_ns() - t0) / ((double)per * nt);
        t0 = now_ns();
        for (int r = 0; r < per; r++) acc += dispatch_jump(g_dyn, nt);
        double ju = (now_ns() - t0) / ((double)per * nt);
        t0 = now_ns();
        for (int r = 0; r < per; r++) acc += dispatch_bsearch(g_dyn, nt, nt);
        double bs = (now_ns() - t0) / ((double)per * nt);
        t0 = now_ns();
        for (int r = 0; r < per; r++) acc += dispatch_cache(g_dyn, nt);
        double ca = (now_ns() - t0) / ((double)per * nt);
        printf("  %d types: chain=%.2f jump=%.2f bsearch=%.2f cache=%.2f", nt, li, ju, bs, ca);
    }
    printf("\n");
    (void)acc;
    return 0;
}