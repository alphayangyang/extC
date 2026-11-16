/* ------------------------------------------------- unreferenced definitions
 * A definition nobody names is dead weight: the C compiler warns about it, and
 * the reader of the generated C has to skip it. Which definitions those are is
 * decided here, on the finished text, because a use may be generated long after
 * the definition (a body, or the descriptor table).
 */

/**
 * @brief Can a byte continue a C identifier?
 *
 * Used to tell the name `e` from the `e` inside `self` or `true`: single-letter locals are counted by
 * name, and a plain substring search would hit everywhere, which only ever keeps a definition alive
 * too long, never drops one.
 *
 * @param c  The byte to test.
 * @return   true for a letter, a digit, `_` or `$`.
 */
static bool identByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* How many times does the name `needle` occur in the finished output, as a whole
 * name? A mention inside a string literal still counts, which only ever keeps a
 * definition alive. */
/* Which text scan is actually running, and over how many bytes. The leaf profile says 53% of a
 * big build sits in `strstr`, but its caller chains have been wrong three times in this session
 * (strcmp to the parser, vecAt to rwStmt, strstr to dropUnusedLocals -- all disproved by the
 * phase timers). These counters answer it directly under EXTC_DBG_TIME=1. */
static long g_scanCalls, g_scanBytes;
static const char *g_scanWho;
/**
 * @brief Record the scan point and byte count of one text scan; accumulated only when timing is on.
 *
 * @param who  The scan point's name, recorded overwriting the previous one as the source of the
 *             latest scan.
 * @param n    This scan's byte count, added to the total.
 */
static void strAccount(const char *who, size_t n) {
    if (!cgTimeOn()) return;
    g_scanCalls++; g_scanBytes += (long)n; g_scanWho = who;
}
/**
 * @brief Report the accumulated scan call count, byte count and latest scan point on stderr.
 */
static void strReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[scan] calls=%ld bytes=%lld (last site: %s)\n",
            g_scanCalls, (long long)g_scanBytes, g_scanWho ? g_scanWho : "-");
}

/**
 * @brief Count how many times `needle` occurs in `hay` as a whole identifier.
 *
 * Occurrences inside string literals count too, which only keeps a definition alive a little longer.
 *
 * @param hay     The text to scan.
 * @param needle  The name to count.
 * @return        The number of occurrences of `needle` as a whole name.
 */
static size_t countMentions(const char *hay, const char *needle) {
    strAccount("countMentions", strlen(hay));
    size_t n = 0, len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) {
        if (p > hay && identByte(p[-1])) continue;
        if (identByte(p[len])) continue;
        n++;
    }
    return n;
}

/**
 * @brief Pair a generated function definition with the declaration of the same C name and record its
 *        offset and length in the body buffer.
 *
 * The key is the C name rather than the `FuncDef` pointer: the instance methods of one generic
 * function are generated once per instance, so pointers are not unique, whereas two generated
 * definitions with the same name cannot compile, so names are unique within this batch. When no
 * declaration of the same name is found the function returns and the definition stays unpaired, so
 * later stages do not delete it: leaving an uncalled function costs one compiler warning and does not
 * affect correctness.
 *
 * @param g    The generation context; `deadFuncs` is the pairing table and the entry with the same
 *             name as `f` is the partner.
 * @param f    The function definition taking part in the pairing.
 * @param off  The definition's starting offset in the body buffer, computed by the call site from the
 *             current output buffer's end.
 * @param len  The definition's byte length.
 */
static void deadFuncBody(CG *g, FuncDef *f, size_t off, size_t len) {
    const char *name = cFuncName(g, f);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (df->body || df->len || strcmp(df->name, name) != 0) continue;
        /* Only the offset is recorded, the text is not cut here: mid-body the output may temporarily point
         * at another buffer, the piece cut at this moment is not contiguous in the final unit, and it
         * can never be found by content afterwards. The text is taken from `g.body` once that is
         * complete. */
        df->off = off;
        df->len = len;
        df->namedByDynTable = df->namedByDynTable || planDynTable(f);   /* see the field */
        return;
    }
}

static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle);
static char  *funcDefStart(char *text, DeadFunc *df, size_t *lenOut);

/**
 * @brief Record a generated local declaration so that it can later be judged for reads.
 *
 * The text runs from `before` to the current output buffer's end, and the call site must guarantee
 * that this is exactly the rest of the declaration. The owning function is `g->curFuncName`.
 *
 * @param g       The generation context; the record is appended to `g->deadLocals`.
 * @param before  The starting offset in `g->out` of the line holding this declaration.
 * @param name    The local variable's C name, pointing at storage that outlives the buffer.
 * @param own     Where it is declared: 1 inside the function body, 0 in the prologue before the body
 *                (`__extc_home` is one of those).
 */
static void localDef(CG *g, size_t before, const char *name, size_t own) {
    Buf l;
    bufInit(&l, g->arena);
    bufPutn(&l, g->out->data + before, g->out->len - before);
    DeadLocal *d = arenaAllocZero(g->arena, sizeof *d);
    d->name     = name;
    d->text     = bufCstr(&l);
    d->funcName = g->curFuncName;
    d->own      = own;
    *(DeadLocal **)vecPush(&g->deadLocals) = d;
}

/**
 * @brief Is a line a `#define`, and if so where does the macro name start in the line?
 *
 * `#define X` and `#  define X` are the same directive (the prelude pads a space between `#` and
 * `define` for alignment), so both spellings must be recognized; at least one space or tab is
 * required between `define` and the macro name.
 *
 * @param ln  The start of a line; no terminating NUL is required.
 * @param ll  The byte length of the line.
 * @return    A pointer to the macro name inside the line; `NULL` when it is not a `#define` or the
 *            line has no macro name.
 */
static const char *macroDefineName(const char *ln, size_t ll) {
    size_t i = 0;
    if (i >= ll || ln[i] != '#') return NULL;
    i++;
    while (i < ll && (ln[i] == ' ' || ln[i] == '\t')) i++;
    if (i + 6 > ll || strncmp(ln + i, "define", 6) != 0) return NULL;
    i += 6;
    if (i >= ll || (ln[i] != ' ' && ln[i] != '\t')) return NULL;
    while (i < ll && (ln[i] == ' ' || ln[i] == '\t')) i++;
    return (i < ll) ? ln + i : NULL;
}

/* Drop local declarations that nothing reads. The stage is the whole function body: a name
 * that occurs there exactly once is the declaration itself. */
/* A statement to remove, or to shorten to its right-hand side. */
typedef struct {
    size_t      start, end;   /* the span in the output text */
    const char *repl;         /* what takes its place, or NULL to drop it entirely */
} LocalCut;

/**
 * @brief Does this text contain a call (looking only for `(`)?
 *
 * An assignment whose right-hand side contains a call cannot be deleted wholesale: the call is the
 * reason it exists, and it matters even when nobody reads the return value (`uint64_t skip =
 * pcg32_next(&r)` advances the generator). A parenthesis is not a call, so a `(` inside a literal or
 * a comment counts as well; that only keeps statements, it never changes semantics.
 *
 * @param s  The text fragment to check.
 * @param n  The fragment's byte count; no terminating NUL is required.
 * @return   `true` when a `(` occurs.
 */
static bool textHasCall(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == '(') return true;
    return false;
}

/**
 * @brief Count how many times `name` is read in this text.
 *
 * String and character literals and comments are skipped whole; an occurrence directly followed by a
 * single `=` (and not by `==`) is a write and does not count as a read, so a compound assignment such
 * as `name += 1` counts as a read. The name must be bounded by non-identifier bytes on both sides.
 *
 * @param hay   The text fragment to scan.
 * @param n     The fragment's byte count; no terminating NUL is required.
 * @param name  The variable name, NUL-terminated.
 * @return      The read count; 0 when it is only written or does not occur.
 */
static size_t countReads(const char *hay, size_t n, const char *name) {
    size_t len = strlen(name), reads = 0;
    for (size_t i = 0; i < n; ) {
        char c = hay[i];
        if (c == '"' || c == '\'') { char q = c; i++;
            while (i < n) { if (hay[i] == '\\' && i + 1 < n) { i += 2; continue; }
                            if (hay[i] == q) { i++; break; } i++; } continue; }
        if (c == '/' && i + 1 < n && hay[i + 1] == '/') { while (i < n && hay[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && hay[i + 1] == '*') { i += 2;
            while (i + 1 < n && !(hay[i] == '*' && hay[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n; continue; }
        if (i + len <= n && strncmp(hay + i, name, len) == 0 &&
            (i == 0 || !identByte(hay[i - 1])) && (i + len >= n || !identByte(hay[i + len]))) {
            size_t j = i + len;
            while (j < n && hay[j] == ' ') j++;
            if (!(j < n && hay[j] == '=' && (j + 1 >= n || hay[j + 1] != '='))) reads++;
            i = j;
            continue;
        }
        i++;
    }
    return reads;
}

/* name -> body for the per-local lookup below. That lookup is a linear scan over `deadFuncs` for
 * every local -- 3N locals against N functions is the quadratic this pass still has (cg-locals is
 * 41% of the build at N=3200 and grows x4 per doubling). The map reproduces the scan exactly: the
 * **first** entry with a non-NULL body, stale pointer and all, because that pointer is what the
 * later strstr(text, body) is applied to. */
typedef struct { const char *name; const char *body; } NameBody;
typedef struct { NameBody *slot; size_t mask; } BodyMap;
/**
 * @brief Compute the FNV-1a hash of a NUL-terminated string for the open addressing of `BodyMap` and
 *        `BpMap`.
 *
 * @param p  A NUL-terminated C string.
 * @return   The hash value; the caller masks it to pick a slot.
 */
static size_t bodyHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211u; }
    return h;
}
/**
 * @brief Initialize `BodyMap`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * The slots are zero-allocated from the arena and an empty slot is recognized by `name == NULL`; the
 * capacity must be a power of two for slot selection by `mask = cap - 1` to work. The table never
 * grows.
 *
 * @param m  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of entries; the call site passes `g->deadFuncs.len + 1`.
 */
static void bodyMapInit(BodyMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief Put one `name -> body` entry into `BodyMap`; a duplicate name keeps only the first entry.
 *
 * First come, first served: the call site inserts in `deadFuncs` order, and the first one is the one
 * to keep. When the name already exists the function returns without overwriting.
 *
 * @param m     The target table.
 * @param name  The C name, NUL-terminated; the table stores only the pointer, so it must outlive the
 *              table.
 * @param body  The pointer to the definition body for that name.
 */
static void bodyMapPut(BodyMap *m, const char *name, const char *body) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        NameBody *s = &m->slot[i];
        if (!s->name) { s->name = name; s->body = body; return; }
        if (strcmp(s->name, name) == 0) return;                 /* first one wins: so does the scan */
    }
}
/* Content-search results, cached per function name. `DeadFunc.body` is a pointer from emission
 * time and the text has been rewritten since (a definition removed by an earlier phase moves
 * everything after it), so it is usually
 * stale and the lookup falls back to strstr(text, body) -- for **every local** of that function.
 * callgrind put that site at 71.67% of all instructions (117,643 calls, 9.33 G instructions).
 * All locals of one function resolve the same body, so one search per name is enough: the cached
 * value is exactly what the search would return while the text is unchanged. */
typedef struct { const char *name; char *bp; size_t bl; int filled; } BpEntry;
typedef struct { BpEntry *slot; size_t mask; } BpMap;
/**
 * @brief Initialize `BpMap`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * Isomorphic to `BodyMap`: the slots are zero-allocated from the arena, an empty slot is recognized
 * by `name == NULL`, and the table never grows.
 *
 * @param m  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of function names.
 */
static void bpMapInit(BpMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief Get (or create) the cache entry of `name` in `BpMap`.
 *
 * A new entry has `filled == 0` and `bp == NULL`; the caller fills in the search result and then sets
 * `filled` to 1 itself. The table never grows, so the returned entry's address is stable for the
 * table's lifetime.
 *
 * @param m     The target table.
 * @param name  The function's C name, NUL-terminated; the table stores only the pointer, so it must
 *              outlive the table.
 * @return      A pointer to the entry inside the table, which lives as long as the table.
 */
static BpEntry *bpMapAt(BpMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        BpEntry *s = &m->slot[i];
        if (!s->name) { s->name = name; return s; }
        if (strcmp(s->name, name) == 0) return s;
    }
}

/**
 * @brief Look up the definition body of `name` in `BodyMap`.
 *
 * What is returned is the pointer recorded at emission time, and once the text is rewritten it may no
 * longer point at the current content: the caller must confirm with `strstr` that it is still inside
 * the current text and fall back to a whole-text search when it is not.
 *
 * @param m     The target table.
 * @param name  The C name, NUL-terminated.
 * @return      The body pointer; `NULL` when the table has no such name.
 */
static const char *bodyMapGet(BodyMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        NameBody *s = &m->slot[i];
        if (!s->name) return NULL;
        if (strcmp(s->name, name) == 0) return s->body;
    }
}

/**
 * @brief Delete, in place, local declarations that nothing reads, together with their assignments.
 *
 * The judgment works per whole function body: when a name occurs only once in that body, that
 * occurrence is the declaration itself. A declaration with `own == 0` sits in the prologue (before the
 * first function body) and belongs to no body, so it is looked for inside the prologue's range
 * instead.
 *
 * One site is deleted per round and the text is rescanned from the start afterwards: a rewrite
 * invalidates every earlier offset. When the right-hand side of a deleted statement contains a call,
 * the call must stay and its value be discarded explicitly, or `-Werror` fails on `unused-value`.
 *
 * @param g      The generation context; reads `deadLocals`, `deadFuncs` and `mainBody`.
 * @param out    The buffer holding the text; its contents are rebuilt for every deletion.
 * @param textp  On entry the text's start (`out->data`); on return the start after the rewrite.
 * @param lenp   On entry the text's byte count; on return the byte count after the rewrite.
 */
static void dropUnusedLocals(CG *g, Buf *out, char **textp, size_t *lenp) {
    char  *text = *textp;
    size_t len  = *lenp;
    /* A declaration with `own == 0` sits in the prologue (before the first function body), and
     * firstBody is that upper bound; `DeadFunc.body` also points into the current text only while
     * the text has not been rewritten. */
    size_t firstBody = len;
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->body && df->body >= text && df->body < text + len) {
            size_t o = (size_t)(df->body - text);
            if (o < firstBody) firstBody = o;
        }
    }
    BpMap bpCache;
    bpMapInit(&bpCache, g->arena, g->deadFuncs.len + 1);
    BodyMap bodyMap;
    bodyMapInit(&bodyMap, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->body) bodyMapPut(&bodyMap, df->name, df->body);
    }
    int textIsOriginal = 1;
    for (;;) {
        bool cut = false;
        for (size_t i = 0; i < g->deadLocals.len && !cut; i++) {
            /* This pass costs memory latency: the real stall is chasing a pointer into a new DeadLocal; the
             * base pointer is still re-read through `vecAt`, so a growth triggered by vecPush is never
             * missed. */
            if (i + 8 < g->deadLocals.len)
                __builtin_prefetch(*(DeadLocal **)vecAt(&g->deadLocals, i + 8), 0, 0);
            DeadLocal *d = *(DeadLocal **)vecAt(&g->deadLocals, i);
            if (!d->text || !d->funcName) continue;
            const char *body = NULL;
            if (textIsOriginal) body = bodyMapGet(&bodyMap, d->funcName);
            if (!body) {
                for (size_t k = 0; k < g->deadFuncs.len; k++) {
                    DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
                    if (df->body && strcmp(df->name, d->funcName) == 0) { body = df->body; break; }
                }
            }
            if (!body && g->mainBody && strcmp(d->funcName, g->mainFuncName) == 0)
                body = g->mainBody;                    /* main has no DeadFunc entry */
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s func=%-12s mainFunc=%-12s body=%s\n", d->name,
                        d->funcName ? d->funcName : "(null)",
                        g->mainFuncName ? g->mainFuncName : "(null)", body ? "yes" : "no");
            if (!body) continue;                       /* its function is gone already */
            g_locN++;
            char  *bp = NULL;
            size_t bl = 0;
            if (textIsOriginal) {
                BpEntry *e = bpMapAt(&bpCache, d->funcName);
                if (!e->filled) { e->bp = strstr(text, body); e->bl = strlen(body); e->filled = 1; }
                bp = e->bp; bl = e->bl;
            } else {
                bp = strstr(text, body);           /* the text has been rewritten: search by content again each round */
                bl = strlen(body);
            }
            /* `g->mainBody` is a pre-taken copy of `main`, and every deletion rebuilds the text: after a
             * deletion the copy cannot be found by content, so the search falls back to the
             * whole unit. A whole-unit search is safe here -- a local variable name is only valid
             * inside its own function's scope, so widening the range only keeps one more local
             * alive. */
            if (!bp) { bp = (char *)text; bl = len; }
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s bp=%s cnt=%zu own=%zu\n", d->name,
                        bp ? "hit" : "miss", bp ? countMentionsIn(g, bp, bl, d->name) : 0, d->own);
            /* The declaration's own mention is not a read of the variable, so the body is
             * examined without that line. Any other read keeps everything. */
            double t_bm = cgNow();
            Buf bm;
            bufInit(&bm, g->arena);
            size_t declAt = 0;
            bool   declInside = false;
            g_locCopy += (long long)bl;
            if (d->own == 1) {
                char *dl = strstr(bp, d->text);
                if (!dl) continue;
                declAt = (size_t)(dl - bp);
                declInside = true;
                bufPutn(&bm, bp, declAt);
                bufPutn(&bm, dl + strlen(d->text), bl - declAt - strlen(d->text));
            } else {
                bufPutn(&bm, bp, bl);
            }
            cgAcc("cg-loc-bm", cgNow() - t_bm);
            g_locRead += (long long)bm.len;
            if (countReads(bufCstr(&bm), bm.len, d->name) != 0) continue;   /* it is read */
            (void)declInside;
            (void)declAt;
            /* collect the pieces to delete: the declaration itself and every assignment to it (when the
     * right-hand side contains a call, only the call is kept) */
            Vec cuts;
            vecInit(&cuts, g->arena, sizeof(LocalCut));
            char *dl;
            if (d->own == 0) {
                dl = NULL;
                size_t dtl = strlen(d->text);
                if (textIsOriginal) {
                    for (char *p2 = text; (size_t)(p2 - text) + dtl <= firstBody; p2++)
                        if (memcmp(p2, d->text, dtl) == 0) { dl = p2; break; }
                } else {
                    dl = strstr(text, d->text);
                }
            } else {
                dl = strstr(bp, d->text);
            }
            if (!dl || (d->own != 0 && dl >= bp + bl)) continue;
            {
                size_t ls = (size_t)(dl - text);
                while (ls > 0 && text[ls - 1] != '\n') ls--;
                size_t le = (size_t)(dl - text);
                while (le < len && text[le] != '\n') le++;
                if (le < len) le++;
                char *assign = NULL;
                for (size_t k = (size_t)(dl - text); k < le; k++)
                    if (text[k] == '=' && (k + 1 >= le || text[k + 1] != '=')) { assign = text + k; break; }
                LocalCut c;
                c.start = ls; c.end = le; c.repl = NULL;
                if (assign) {
                    size_t ws = 0;
                    while (ls + ws < (size_t)(assign - text) &&
                           (text[ls + ws] == ' ' || text[ls + ws] == '\t')) ws++;
                    size_t rhs = (size_t)(assign - text) + 1;
                    while (rhs < le && (text[rhs] == ' ' || text[rhs] == '\t')) rhs++;
                    if (textHasCall(text + rhs, le - rhs)) {
                        /* Keep the call, but its value must be discarded explicitly: the right-hand side is usually an
                         * unwrapping (`var f = fs::openWrite(p)!`), and the bare payload expression left
                         * behind is judged `unused-value` by gcc, which fails outright under
                         * `-Werror`. */
                        size_t re = le;
                        while (re > rhs && (text[re - 1] == ';' || text[re - 1] == '\n' ||
                                            text[re - 1] == ' ' || text[re - 1] == '\t')) re--;
                        Buf r;
                        bufInit(&r, g->arena);
                        bufPutn(&r, text + ls, ws);
                        bufPuts(&r, "(void)(");
                        bufPutn(&r, text + rhs, re - rhs);
                        bufPuts(&r, ");\n");
                        c.repl = bufCstr(&r);
                    }
                }
                *(LocalCut *)vecPush(&cuts) = c;
            }
            g_locLine += (long long)bl;
            for (char *p = bp; p < bp + bl; ) {
                char  *nl2 = memchr(p, '\n', (size_t)(bp + bl - p));
                size_t ll2 = nl2 ? (size_t)(nl2 - p) : (size_t)(bp + bl - p);
                size_t k = 0;
                while (k < ll2 && (p[k] == ' ' || p[k] == '\t')) k++;
                size_t nlen2 = strlen(d->name);
                if (ll2 > k + nlen2 && strncmp(p + k, d->name, nlen2) == 0) {
                    char *q = p + k + nlen2;
                    while (q < p + ll2 && *q == ' ') q++;
                    if (q < p + ll2 && *q == '=' && (q + 1 >= p + ll2 || q[1] != '=')) {
                        size_t ls = (size_t)(p - text), le = ls + ll2 + (nl2 ? 1 : 0);
                        size_t rhs = (size_t)(q - text) + 1;
                        while (rhs < ls + ll2 && (text[rhs] == ' ' || text[rhs] == '\t')) rhs++;
                        LocalCut c;
                        c.start = ls; c.end = le; c.repl = NULL;
                        if (textHasCall(text + rhs, le - rhs)) {
                            Buf r;
                            bufInit(&r, g->arena);
                            bufPutn(&r, text + ls, k);          /* the indentation */
                            bufPutn(&r, text + rhs, le - rhs);  /* the call, its `;` and the newline */
                            c.repl = bufCstr(&r);
                        }
                        *(LocalCut *)vecPush(&cuts) = c;
                    }
                }
                if (!nl2) break;
                p = nl2 + 1;
            }
            /* One assembly, in order: the spans are disjoint and ascending, and the result is
             * never longer than what it replaces, so it fits in the same buffer. */
            g_cuts++;
            double t_rw = cgNow();
            Buf nb;
            bufInit(&nb, g->arena);
            size_t prev = 0;
            for (size_t ci = 0; ci < cuts.len; ci++) {
                LocalCut *c = (LocalCut *)vecAt(&cuts, ci);
                if (c->start < prev) continue;                 /* overlapping: skip, stay safe */
                bufPutn(&nb, text + prev, c->start - prev);
                if (c->repl) bufPuts(&nb, c->repl);
                prev = c->end;
            }
            bufPutn(&nb, text + prev, len - prev);
            memcpy(out->data, bufCstr(&nb), nb.len);
            out->data[nb.len] = 0;
            out->len = nb.len;
            len = nb.len;
            text = out->data;
            textIsOriginal = 0;            /* positions are stale from here on: search again */
            cgAcc("cg-rewrite", cgNow() - t_rw);
            d->text = NULL;
            cut = true;
        }
        if (!cut) break;
    }
    *textp = text;
    *lenp = len;
}

/**
 * @brief Count how many times `name` occurs as a whole name in the generated code.
 *
 * String and character literals and comments are skipped whole: single-letter local names occur
 * inside literals too (a string containing `v = `, for instance), and that is not a use. The name
 * must be bounded by non-identifier bytes on both sides, so `e` does not hit the `e` inside `self`.
 *
 * @param hay   The text fragment to scan.
 * @param n     The fragment's byte count; no terminating NUL is required.
 * @param name  The name to count, NUL-terminated.
 * @return      The occurrence count.
 */
static size_t countCodeMentions(const char *hay, size_t n, const char *name) {
    strAccount("countCodeMentions", n);
    size_t len = strlen(name), count = 0;
    for (size_t i = 0; i < n; ) {
        char c = hay[i];
        if (c == '"' || c == '\'') {                 /* skip the literal */
            char q = c;
            i++;
            while (i < n) {
                if (hay[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (hay[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && hay[i + 1] == '/') {
            while (i < n && hay[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && hay[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(hay[i] == '*' && hay[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (i + len <= n && strncmp(hay + i, name, len) == 0 &&
            (i == 0 || !identByte(hay[i - 1])) && (i + len >= n || !identByte(hay[i + len]))) {
            count++;
            i += len;
            continue;
        }
        i++;
    }
    return count;
}

/**
 * @brief Count `name`'s occurrences in a text that is not NUL-terminated, counting code only.
 *
 * Literals and comments are skipped and the name must be bounded by non-identifier bytes; the name is
 * a byte range rather than a C string, so a body buffer can be sliced into it directly.
 *
 * @param g       The generation context; this function does not use it.
 * @param hay     The text fragment to scan.
 * @param n       The fragment's byte count; no terminating NUL is required.
 * @param needle  The name to count, NUL-terminated.
 * @return        The occurrence count.
 */
static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle) {
    strAccount("countMentionsIn", n);
    (void)g;
    return countCodeMentions(hay, n, needle);
}

/**
 * @brief Count how many times `name` occurs outside these spans.
 *
 * One `strstr` pass that skips the given spans is cheaper than copying the whole unit, cutting the
 * fragments out and counting in the copy. The spans are half-open, `[starts[k], ends[k])`; the name
 * must be whole, and mentions inside literals and comments count too -- that only keeps definitions
 * alive, which is the safe direction.
 *
 * @param hay     The whole NUL-terminated text.
 * @param name    The name to count, NUL-terminated.
 * @param starts  The span start array.
 * @param ends    The span end array, half-open.
 * @param nspans  The number of spans.
 * @return        The number of whole-name occurrences outside every span.
 */
static size_t countMentionsOutside(const char *hay, const char *name,
                                   const size_t *starts, const size_t *ends, size_t nspans) {
    size_t len = strlen(name), count = 0;
    /* `strstr` jumps from one occurrence to the next (vectorized); matching byte by byte with
     * `strncmp` would be much more expensive. Mentions inside literals or comments count too -- that
     * only keeps definitions alive, which is the safe direction. */
    for (const char *p = hay; (p = strstr(p, name)) != NULL; p += len) {
        size_t i = (size_t)(p - hay);
        if (i > 0 && identByte(hay[i - 1])) continue;
        if (identByte(p[len])) continue;
        bool inside = false;
        for (size_t k = 0; k < nspans; k++)
            if (i >= starts[k] && i < ends[k]) { inside = true; break; }
        if (!inside) count++;
    }
    return count;
}

/* Drop every recorded definition whose name occurs exactly once in the finished
 * output - that one occurrence being the definition itself.
 *
 * The text is searched for the definition rather than for a byte offset: the
 * unit is assembled by splicing several buffers together (prototypes, then the
 * descriptor pool, then the bodies), so an offset taken at emission time does
 * not survive. The definition text does.
 *
 * A name that occurs twice or more is kept, whoever mentions it - including
 * mentions that sit inside code that is itself unreachable. That keeps the
 * decision on the safe side: the worst case is a definition that is still
 * emitted, never a name that nothing defines. */
/* Occurrence counts of the names this phase decides on. `countMentions(text, name)` runs once per
 * candidate over the whole unit -- the scan counters put that at 11.4 GB of scanning for N=3200
 * (x3.85 per doubling, the largest single cost left). Same numbers, maintained instead of
 * re-scanned: one pass fills them and every removal subtracts the runs inside the text it deletes,
 * so the decisions -- and the emitted C -- are byte for byte the same. Only the names asked about
 * live in the table, so a scan never inserts and the table cannot fill up. */
typedef struct { const char *name; size_t len; long count; } NameCount;
typedef struct { NameCount *slot; size_t mask; } CountTable;
/**
 * @brief Compute the FNV-1a hash of a byte range for the open addressing of `CountTable`.
 *
 * The length is given by `n` and no terminating NUL is required, so a name can be a slice of the text.
 *
 * @param p  The start of the bytes.
 * @param n  The byte count.
 * @return   The hash value; the caller masks it to pick a slot.
 */
static size_t countHash(const char *p, size_t n) {
    size_t h = 1469598103934665603u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211u; }
    return h;
}
/**
 * @brief Initialize `CountTable`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * The slots are zero-allocated from the arena and an empty slot is recognized by `name == NULL`. The
 * table only holds the names the caller registered first and no insertion happens while counting
 * afterwards, so it cannot fill up.
 *
 * @param t  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of registered names.
 */
static void countInit(CountTable *t, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    t->slot = arenaAllocZero(a, cap * sizeof *t->slot);
    t->mask = cap - 1;
}
/**
 * @brief Register a name in the name counter table, starting its count at 0.
 *
 * The table is sized once in `countInit` from the number of candidate names, so this function only
 * probes empty slots and can never fill up; an entry stores the caller's name pointer without copying
 * the text, so that pointer must outlive the table.
 *
 * @param t  The name counter table; must have been initialized by `countInit`.
 * @param p  The name text, `n` bytes long, not necessarily NUL-terminated.
 * @param n  The name's byte count.
 */
static void countPut(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) { s->name = p; s->len = n; s->count = 0; return; }
        if (s->len == n && memcmp(s->name, p, n) == 0) return;
    }
}
/**
 * @brief Add `delta` to one name's count.
 *
 * A name not in the table returns immediately: the table only holds names that have been registered,
 * and the scan never inserts a new entry.
 *
 * @param t      The name counter table.
 * @param p      The name text, `n` bytes long.
 * @param n      The name's byte count.
 * @param delta  The count increment, possibly negative (subtracting the occurrences from a text that
 *               is about to be deleted).
 */
static void countBump(CountTable *t, const char *p, size_t n, long delta) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return;
        if (s->len == n && memcmp(s->name, p, n) == 0) { s->count += delta; return; }
    }
}
/**
 * @brief Get a name's current count.
 *
 * @param t  The name counter table.
 * @param p  The name text, `n` bytes long.
 * @param n  The name's byte count.
 * @return   The count in the table; 0 when the name is not there.
 */
static long countGet(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return 0;
        if (s->len == n && memcmp(s->name, p, n) == 0) return s->count;
    }
}
/**
 * @brief Adjust every name occurring in a text by `delta`.
 *
 * Only identifiers are split: a name cut at either end of the range does not count, it belongs to the
 * identifier outside. Used to subtract a piece of text that is about to be deleted from the counts.
 *
 * @param t      The name counter table.
 * @param text   The whole text, covering at least `[start, start + len)`.
 * @param start  The range's starting offset.
 * @param len    The range's byte count.
 * @param delta  The count increment, possibly negative.
 */
static void countSpan(CountTable *t, const char *text, size_t start, size_t len, long delta) {
    size_t i = 0;
    while (i < len) {
        if (!identByte(text[start + i])) { i++; continue; }
        size_t s0 = i;
        while (i < len && identByte(text[start + i])) i++;
        if (s0 == 0 && start > 0 && identByte(text[start - 1])) continue;
        if (i == len && identByte(text[start + len])) continue;
        countBump(t, text + start + s0, i - s0, delta);
    }
}

/**
 * @brief Delete definitions, prototypes and local declarations that nothing references from the
 *        assembled generated unit.
 *
 * A name that occurs in the finished text only at its own definition is unused. The judgment is a
 * fixed point: in one round a definition may be mentioned only by another definition that is itself
 * about to be deleted, so one round is not enough. Offsets move with every deletion, so the text is
 * located again on each pass. The judgment errs on the conservative side: any other mention keeps the
 * definition, even when that piece of code is dead itself.
 *
 * @param g    The code generation state; the records in `deadDefs` / `deadFuncs` / `deadLocals` are
 *             the evidence.
 * @param out  The generated C unit; deleted in place, with `out->len` shrinking accordingly.
 */
static void dropUnreferenced(CG *g, Buf *out) {
    char  *text = bufCstr(out);              /* terminate: the searches below are C strings */
    size_t len  = out->len;
    /* Definitions inside a body first, from the end backwards, so that removing one
    * leaves the offsets of the ones still to come untouched. What can name such a
    * definition is the enclosing piece of code and nothing else - a payload binding
    * is read by its arm - so the count happens inside that scope: `e` and `n` are
    * common names, and counting them across the whole unit would always find more
    * than one. */
    for (size_t i = g->deadDefs.len; i-- > 0; ) {
        DeadDef *d = *(DeadDef **)vecAt(&g->deadDefs, i);
        if (!d->scoped) continue;                                      /* top-level: later */
        if (countMentionsIn(g, g->body.data + d->scopeA, d->scopeB - d->scopeA, d->name)) continue;
        size_t tl = strlen(d->text);
        size_t at = g->bodyOff + d->off;
        if (at + tl > len || memcmp(text + at, d->text, tl) != 0) continue;   /* be sure */
        memmove(text + at, text + at + tl, len - at - tl + 1);
        if (dbgOn("EXTC_DBG_DCE")) fprintf(stderr, "[dce] scoped %s\n", d->name);
        len -= tl;
        out->len = len;
    }
    /* Local declarations that nothing reads: after the offset-based phase, before the
     * phases that remove whole functions (a local of a function that is gone needs no
     * decision). */
    CGACC("cg-locals", dropUnusedLocals(g, out, &text, &len));
    /* Functions nobody calls. Both halves are located by their own text, so what is
    * removed is exactly what was captured - never a piece of a function. The
    * definition sits after the declaration, so it goes first and the declaration's
    * position stays valid.
    *
    * A use is a mention *outside* the declaration and the definition, so a function
    * whose only other mention is a call to itself is not used. A mutually recursive
    * pair with no entry point has no such mention either way and is kept. */
    CountTable counts;
    countInit(&counts, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        if (k + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, k + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->name) countPut(&counts, df->name, strlen(df->name));
    }
    countSpan(&counts, text, 0, len, 1);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        if (i + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, i + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;                                   /* no definition emitted */
        if (df->namedByDynTable) continue;   /* named by a `dyn` table, which is spliced in later */
        size_t nlen = strlen(df->name);
        long   total = countGet(&counts, df->name, nlen);
        if (total < 2) continue;                                   /* no declaration + definition */
        char  *pt = strstr(text, df->proto);
        /* The definition is located by its signature line: the passes above rewrite the inside of
        * bodies, so the copy taken at generation time can stop matching the text. */
        size_t blen = 0;
        char  *bd = funcDefStart(text, df, &blen);
        if (!pt || !bd || bd < pt || !blen) continue;               /* not both, in order */
        size_t bl = blen, pl = strlen(df->proto);
        /* `total == 2` is the fast path: declaration and definition, nothing else names it. A
         * larger count only reaches the decision when the extra mentions are all inside the two
         * pieces - that is the self-call case above. */
        if (total != 2) {
            size_t inside = countMentionsIn(g, bd, bl, df->name)
                          + countMentionsIn(g, pt, pl, df->name);
            if ((size_t)total != inside) continue;                 /* someone else names it */
        }
        countSpan(&counts, text, (size_t)(bd - text), bl, -1);
        countSpan(&counts, text, (size_t)(pt - text), pl, -1);
        memmove(bd, bd + bl, len - (size_t)(bd - text) - bl + 1);    /* definition first */
        len -= bl;
        memmove(pt, pt + pl, len - (size_t)(pt - text) - pl + 1);    /* then declaration */
        len -= pl;
        if (g->bodyOff > (size_t)(pt - text)) g->bodyOff -= pl;
        out->len = len;
        if (dbgOn("EXTC_DBG_DCE")) fprintf(stderr, "[dce] func %s\n", df->name);
        df->body = NULL;
    }
    /* The runtime primitive block is not decided here: it is assembled beside the unit and
    * decided from the registry (`primSplice`, called from generateC once the rest of the
    * pipeline has settled). Nothing below this line scans for a definition. */
    /* Top-level definitions, one name at a time.
    *
    * The test is on a scratch copy of the unit: every piece registered under the name is cut
    * out of the copy, and if the name is not mentioned anywhere else, the pieces are the only
    * places it appears - so nothing uses it and they can go.
    *
    * Everything the name appears in is its own pieces: nothing uses it. */
    bool dropped = true;
    while (dropped) {
        dropped = false;
        for (size_t i = 0; i < g->deadDefs.len; i++) {
            if (i + 8 < g->deadDefs.len) __builtin_prefetch(*(DeadDef **)vecAt(&g->deadDefs, i + 8), 0, 0);
            DeadDef *d = *(DeadDef **)vecAt(&g->deadDefs, i);
            if (d->scoped || !d->text) continue;            /* inside a body, or already gone */
            size_t starts[8], ends[8], pieces = 0;
            bool   allThere = true;
            for (size_t k = 0; k < g->deadDefs.len; k++) {
                if (k + 8 < g->deadDefs.len) __builtin_prefetch(*(DeadDef **)vecAt(&g->deadDefs, k + 8), 0, 0);
                DeadDef *o = *(DeadDef **)vecAt(&g->deadDefs, k);
                if (o->scoped || !o->text || strcmp(o->name, d->name) != 0) continue;
                char  *p = strstr(text, o->text);
                if (!p) { allThere = false; break; }        /* a piece is not in the unit */
                if (pieces < 8) {                            /* more than eight: leave it alone */
                    starts[pieces] = (size_t)(p - text);
                    ends[pieces]   = starts[pieces] + strlen(o->text);
                }
                pieces++;
            }
            if (!allThere || pieces == 0 || pieces > 8) continue;
            /* Everything the name appears in is its own pieces: nothing uses it. */
            if (countMentionsOutside(text, d->name, starts, ends, pieces) != 0) continue;
            for (size_t k = 0; k < g->deadDefs.len; k++) {  /* remove them for real */
                DeadDef *o = *(DeadDef **)vecAt(&g->deadDefs, k);
                if (o->scoped || !o->text || strcmp(o->name, d->name) != 0) continue;
                size_t ol = strlen(o->text);
                char  *at = strstr(text, o->text);
                if (at) {
                    memmove(at, at + ol, len - (size_t)(at - text) - ol + 1);
                    len -= ol;
                    out->len = len;
                    text = bufCstr(out);
                }
                o->text = NULL;
            }
            dropped = true;
            break;                                          /* the text moved: start over */
        }
    }
    out->data[len] = '\0';
}

/**
 * @brief Make sure the `EXTC_UNUSED` attribute macro is still defined in the buffer the attribute is
 *        inserted into.
 *
 * The attribute macro is pruned before this runs, and with no use in sight it deletes the definition,
 * after which the inserted attribute would point at a macro that is gone. The definition is
 * inserted back into the `#if defined(__GNUC__)` block that holds it; when even the anchor is gone the
 * function returns false and the caller gives up the insertion instead of emitting an attribute the
 * compiler does not know.
 *
 * @param g   The code generation state, which provides the arena for the temporary buffer.
 * @param nb  The unit text the attribute is inserted into; the definition is inserted in place.
 * @return    true when the definition is already in the text or was inserted by this call; false when
 *            none of the three anchors is found.
 */
static bool ensureUnusedMacro(CG *g, Buf *nb) {
    if (strstr(nb->data, "#define EXTC_UNUSED")) return true;
    const char *anchors[3];
    anchors[0] = "#define EXTC_INLINE";          /* preferred: right next to its sibling */
    anchors[1] = "#if defined(__GNUC__)";        /* the block it belongs to */
    anchors[2] = NULL;
    for (int i = 0; anchors[i]; i++) {
        char *a = strstr(nb->data, anchors[i]);
        if (!a) continue;
        char *nl = strchr(a, '\n');
        if (!nl) continue;
        size_t ins = (size_t)(nl - nb->data) + 1;
        Buf t;
        bufInit(&t, g->arena);
        bufPutn(&t, nb->data, ins);
        bufPuts(&t, "#define EXTC_UNUSED __attribute__((unused))\n");
        bufPutn(&t, nb->data + ins, nb->len - ins);
        nb->len = 0;
        bufPutn(nb, t.data, t.len);
        return true;
    }
    return false;
}

/**
 * @brief Locate a function definition's start and end in the generated text.
 *
 * The recorded function text is matched as a whole first; when an earlier rewrite pass has moved it,
 * the search falls back to the signature line. The definition ends at the `}` in column zero -- every
 * generated definition closes that way. When it cannot be located the function returns NULL, so the
 * caller skips that function instead of deleting the wrong thing.
 *
 * @param text    The assembled generated unit's text, writable.
 * @param df      The function record; returns NULL when `df->body` is NULL (the definition was not
 *                emitted).
 * @param lenOut  Receives the definition text's byte count, from the start to the closing `}`.
 * @return        The definition's start; NULL when it is not found.
 */
static char *funcDefStart(char *text, DeadFunc *df, size_t *lenOut) {
    if (!df->body) return NULL;
    char *at = strstr(text, df->body);              /* untouched: the whole text is the key */
    if (!at) {
        /* Touched by the passes above, so only the signature line can be relied on. */
        const char *nl = strchr(df->body, '\n');
        size_t sigLen = nl ? (size_t)(nl - df->body) : strlen(df->body);
        if (!sigLen) return NULL;
        for (char *p = text; *p; p++) {
            if (p != text && p[-1] != '\n') continue;
            if (strncmp(p, df->body, sigLen) == 0) { at = p; break; }
        }
    }
    if (!at) return NULL;
    char *e = at;
    for (char *p = at; *p; p++)   /* `p != at`: `p[-1]` below must not read before the buffer */
        if (p != at && p[0] == '}' && p[-1] == '\n') { e = p + 1; break; }
    *lenOut = (size_t)(e - at);
    return at;
}

/**
 * @brief Order insertion points from the end of the unit to its start.
 *
 * One inserted attribute moves every later offset, so insertion has to run backwards. The points are
 * collected function by function, which is not their order in the text (a prototype emitted early
 * can describe a body emitted late).
 *
 * @param a  A `size_t` offset.
 * @param b  Another `size_t` offset.
 * @return   Negative when `a` comes later in the text.
 */
static int cmpInsertDesc(const void *a, const void *b) {
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;
    return x < y ? 1 : (x > y ? -1 : 0);
}

/**
 * @brief Add `EXTC_UNUSED` to the declaration of a function that has no call site at all.
 *
 * The attribute asserts "nobody calls this", and on a function that is called clang reports
 * `-Wused-but-marked-unused`, once per call site. The condition holds when the name occurs in the
 * finished text only in its own declaration and definition. All insertion points are collected first
 * and applied from the end backwards, with the insertion itself done last, because inserting one moves
 * every later offset.
 *
 * @param g    The code generation state; `deadFuncs` is the source of candidates.
 * @param out  The generated C unit; the attributes are inserted in place.
 */
static void markUncalledFunctions(CG *g, Buf *out) {
    char  *text = out->data;
    size_t len  = out->len;
    Vec    at;
    vecInit(&at, g->arena, sizeof(size_t));
    /* One pass over the finished text fills the counts the loop below asks for. Counting by
     * scanning per function is O(N^2): the strstr site histogram put 23.4 GB of scanning on the
     * per-name query here at N=1600, and the per-function instruction diff showed the whole run is
     * 75% inside strstr. The text does not change while the loop runs -- it only collects offsets
     * -- so the counts are the same numbers, byte for byte. */
    CountTable counts;
    countInit(&counts, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        if (k + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, k + 8), 0, 0);
        DeadFunc *dfk = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (dfk->name) countPut(&counts, dfk->name, strlen(dfk->name));
    }
    countSpan(&counts, text, 0, len, 1);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        if (i + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, i + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;
        size_t blen = 0;
        char *bd = funcDefStart(text, df, &blen);
        if (!bd) continue;
        char *pt = strstr(text, df->proto);
        size_t own = 1;                                  /* the definition always mentions it */
        if (pt && pt < bd) own++;
        if (bd) (void)blen;
        if (((size_t)countGet(&counts, df->name, strlen(df->name))) != own) continue;   /* something calls it: leave it alone */
        /* The attribute goes at the start of the declaration. Looking for the literal `static `
        * would miss every `@inline` function: those are declared through the `EXTC_INLINE` macro,
        * so no `static` is spelled out. */
        char *decl = pt ? pt : bd;
        while (*decl == ' ') decl++;
        if (decl >= text + len) continue;
        *(size_t *)vecPush(&at) = (size_t)(decl - text);
    }
    if (!at.len) return;

    if (!strstr(text, "#define EXTC_UNUSED") && !strstr(text, "#define EXTC_INLINE") &&
        !strstr(text, "#if defined(__GNUC__)")) return;
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, text, len);
    qsort(at.data, at.len, sizeof(size_t), cmpInsertDesc);
    for (size_t k = 0; k < at.len; k++) {
        size_t off = *(size_t *)vecAt(&at, k);
        Buf tmp;
        bufInit(&tmp, g->arena);
        bufPutn(&tmp, nb.data, off);
        bufPuts(&tmp, "EXTC_UNUSED ");
        bufPutn(&tmp, nb.data + off, nb.len - off);
        nb.len = 0;
        bufPutn(&nb, tmp.data, tmp.len);
    }
    ensureUnusedMacro(g, &nb);
    out->data = bufCstr(&nb);
    out->len  = nb.len;
    out->cap  = nb.len + 1;
}

/**
 * @brief Add `EXTC_UNUSED` to a parameter the function body never reads.
 *
 * A parameter cannot be deleted, it is part of the signature, and the attribute is the only way to say
 * "this parameter is deliberately unused". The parameter list's range is the first `(` of the
 * definition up to its matching `)`, split at top-level commas, and each name is counted only inside
 * **this** function's body -- counting to the end of the file would take another function's parameter
 * of the same name as a use. The attribute is written at the parameter's start rather than before the
 * name: after a preceding type it would attach to a pointer type and gcc would still report the
 * parameter as unused. Insertion points are collected first and applied from the end backwards, with
 * the insertion itself done last.
 *
 * @param g    The code generation state; every body in `deadFuncs` is a text to check.
 * @param out  The generated C unit; the attributes are inserted in place.
 */
static void markUnusedParams(CG *g, Buf *out) {
    char  *text = out->data;
    size_t len  = out->len;
    Vec    at;
    vecInit(&at, g->arena, sizeof(size_t));
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;
        size_t bl = 0;
        char  *fn = funcDefStart(text, df, &bl);
        if (!fn) continue;
        char  *op = memchr(fn, '(', bl);
        if (!op) continue;
        int    depth = 0;
        char  *cl = NULL;
        for (char *p = op; p < fn + bl; p++) {
            if (*p == '(') depth++;
            else if (*p == ')' && --depth == 0) { cl = p; break; }
        }
        if (!cl) continue;
        char *ps = op + 1;
        int   d2 = 0;
        for (char *p = ps; p <= cl; p++) {
            if (*p == '(' || *p == '[') d2++;
            else if (*p == ')' || *p == ']') d2--;
            if (!((p == cl) || (*p == ',' && d2 == 0))) continue;
            char *ns = p;                                   /* the parameter is [ps, p) */
            while (ns > ps && ns[-1] == ' ') ns--;
            char *ne = ns;
            while (ne > ps && identByte(ne[-1])) ne--;
            if (ne < ns && (size_t)(ns - ne) < 64) {
                char nm[64];
                memcpy(nm, ne, (size_t)(ns - ne));
                nm[ns - ne] = 0;
                /* `f(void)` has no parameters, and `void` is not one: marking it makes clang say
                 * the attribute cannot be applied to a `void` parameter. */
                if (strcmp(nm, "void") == 0) { ps = p + 1; continue; }
                /* Counted inside *this* function's body, not to the end of the file: another
                 * function's own `__extc_home` parameter would otherwise count as a use here. */
                if (countCodeMentions(cl + 1, (size_t)((fn + bl) - (cl + 1)), nm) == 0) {
                    /* At the *start* of the parameter, not before the name: written as
                     * `int32_t * __attribute__((unused)) b` the attribute lands on the pointer
                     * type and gcc still reports the parameter. */
                    char *pstart = ps;
                    while (pstart < p && *pstart == ' ') pstart++;
                    *(size_t *)vecPush(&at) = (size_t)(pstart - text);
                }
            }
            ps = p + 1;
        }
    }
    if (!at.len) return;

    if (!strstr(text, "#define EXTC_UNUSED") && !strstr(text, "#define EXTC_INLINE") &&
        !strstr(text, "#if defined(__GNUC__)")) return;
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, text, len);
    qsort(at.data, at.len, sizeof(size_t), cmpInsertDesc);
    for (size_t k = 0; k < at.len; k++) {
        size_t off = *(size_t *)vecAt(&at, k);
        Buf tmp;
        bufInit(&tmp, g->arena);
        bufPutn(&tmp, nb.data, off);
        bufPuts(&tmp, "EXTC_UNUSED ");
        bufPutn(&tmp, nb.data + off, nb.len - off);
        nb.len = 0;
        bufPutn(&nb, tmp.data, tmp.len);
    }
    ensureUnusedMacro(g, &nb);
    out->data = bufCstr(&nb);
    out->len  = nb.len;
    out->cap  = nb.len + 1;
}

/* Is this method an implementation named by a static method table?
 *
 * Such a function is a **root**: nothing in the program calls it, yet the table refers to it, so
 * it has to be emitted and must not be pruned as unreferenced. The reachability filter below only
 * keeps `used` functions (plus `main`), which is why the first tables emitted nothing that
 * resolved -- `'box_zeta' undeclared` in the generated C was how that showed up. Each of the two
 * seeding loops (methods, free functions) asks this same question, so it lives here once.
 *
 * The owner is compared first, which keeps a same-named method of another type out; the name
 * comparison is there because codegen can hold its own copy of a declaration (instances,
 * substituted signatures). */
/**
 * @brief A type's method set (the struct the method table hangs off).
 *
 * When the type has no function body of its own, `Type.mholder` is used -- that covers the built-in
 * scalars (`impl i64 { ... }`) and generic **instances** (`impl slice<u8> { ... }`); otherwise the
 * declared `sdef` is used. Every pass that asks "does this type have this method" must go through
 * here, and overlooking `mholder` misses those two classes of methods.
 *
 * @param t  The type; may be NULL.
 * @return   The method set's struct definition; NULL when `t` is NULL or both are empty.
 */
static StructDef *methodSetOf(Type *t) {
    if (!t) return NULL;
    return t->mholder ? t->mholder : t->sdef;
}

/**
 * @brief Is a function pointed to by some trait impl's method table?
 *
 * Such a function is a root of the reachability analysis: the program has no call site for it, but a
 * table references it, so it must be emitted and cannot be pruned as unused. The method set is
 * compared first (`methodSetOf`), which keeps same-named methods of other types out; then the name is
 * compared again, because codegen may hold a copy of the declaration (an instance, a substituted
 * signature). The function matches when both the method set and the name match.
 *
 * @param m  The module holding the trait impls; the method tables come from `m->impls`.
 * @param f  The candidate function; returns false immediately when `f->owner` is NULL (a free
 *           function).
 * @return   true when a method table points at `f`.
 */
static bool inTraitTable(Module *m, FuncDef *f) {
    if (!f->owner) return false;
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        Type *bt = ttBase(planImplTarget(im));
        if (!bt || methodSetOf(bt) != f->owner) continue;
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            if (mth == f) return true;
            if (mth->name && f->name && strcmp(mth->name, f->name) == 0) return true;
        }
    }
    return false;
}


static const PrimPiece PRIM_BLOCK[] = {
    { PRIM_SCAFFOLD, NULL, "/* Every path that ends the process on purpose goes through here. A trap path is\n * the only code that still runs when a program dies, so this is where anything the\n * process has to give back is given back. Today that is the terminal: a program\n * that traps in raw mode would otherwise leave the user's shell unable to echo\n * what they type. The hook is installed by `extc_raw_enter` (emitted only when the\n * program uses the raw-terminal primitives) and cleared before it runs, so it\n * cannot run twice. */\n" },
    { PRIM_VAR, "__extc_dying", "static int32_t (*__extc_dying)(void);\n" },
    { PRIM_SCAFFOLD, NULL, "#if defined(__GNUC__) || defined(__clang__)\n" },
    { PRIM_MACRO, NULL, "#  define EXTC_NORETURN __attribute__((noreturn))\n" },
    { PRIM_SCAFFOLD, NULL, "#else\n" },
    { PRIM_MACRO, NULL, "#  define EXTC_NORETURN _Noreturn\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_die", "EXTC_NORETURN static inline void extc_die(int code) {\n    if (__extc_dying) { int32_t (*f)(void) = __extc_dying; __extc_dying = 0; (void)f(); }\n    exit(code);\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Every primitive below is `static inline`, and that is not a style choice:\n * without inlining, gcc at -O1 cannot see the body of a check, so it can neither\n * eliminate the check nor turn `i % 7` into a multiply and shift. Measured: the\n * remainder operator was 4.6x slower and a matrix multiply 1.5x slower; with\n * inlining both match C. */\n/* Out-of-range trap; the position comes from the call site via `#line`. */\n" },
    { PRIM_FUNC, "extc_trap", "EXTC_NORETURN static inline void extc_trap(const char *file, int line, int64_t i, int64_t n) {\n    fprintf(stderr, \"%s:%d: trap: index %lld out of range (length %lld)\\n\",\n            file, line, (long long)i, (long long)n);\n    extc_die(1);\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Arithmetic failures must be loud: division by zero, the overflowing division, and\n * an over-wide shift are all undefined behaviour in C. Each one traps with a\n * source position instead of computing a wrong answer or leaving UB behind. */\n" },
    { PRIM_FUNC, "extc_trapMsg", "EXTC_NORETURN static inline void extc_trapMsg(const char *file, int line, const char *msg) {\n    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, msg);\n    extc_die(1);\n}" },
    /* The two `!` assertions. Each carries its comment **inside** its definition: a separate
     * comment row would be emitted unconditionally and change the generated C of every
     * program, including the ones that never write `!`. The comment names no primitive, so
     * it cannot keep one alive through the mention count. */
    { PRIM_FUNC, "extc_refAssert", "/* `!` on a nullable reference: the signature is checked at runtime, and a wrong\n * one traps with a source position. The pointer is returned, so the operand is\n * evaluated once. An `@unchecked` body never calls this at all. */\nstatic inline void *extc_refAssert(void *p, const char *file, int line) {\n    if (!p) extc_trapMsg(file, line, \"the nullable reference was asserted non-null but is null\");\n    return p;\n}" },
    { PRIM_FUNC, "extc_tagAssert", "/* `!` on an `option` / `result`: the tag is asserted before the payload is read,\n * so a `none` / failed arm traps with a position instead of handing back a zero\n * value. The tag is returned, so the operand is evaluated once. */\nstatic inline int extc_tagAssert(int tag, int want, const char *file, int line) {\n    if (tag != want) extc_trapMsg(file, line, \"the variant was asserted to hold a value but is none or failure\");\n    return tag;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n#ifndef EXTC_REC_LIMIT\n" },
    { PRIM_MACRO, NULL, "#define EXTC_REC_LIMIT 100000\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_VAR, "__extc_rec_depth", "static int64_t __extc_rec_depth = 0;\n" },
    { PRIM_FUNC, "extc_rec_enter", "static inline void extc_rec_enter(const char *f, int l) {\n    if (++__extc_rec_depth > EXTC_REC_LIMIT)\n        extc_trapMsg(f, l, \"recursion too deep (unbounded recursion?)\");\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_divI", "static inline int64_t extc_divI(int64_t a, int64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n    return a / b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_modI", "static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n    return a % b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_divU", "static inline uint64_t extc_divU(uint64_t a, uint64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    return a / b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_modU", "static inline uint64_t extc_modU(uint64_t a, uint64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    return a % b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Shifts: shifting by the width or more, or by a negative amount, is undefined\n * behaviour in C, so the count is checked and returned (evaluated once). */\n" },
    { PRIM_FUNC, "extc_shiftCount", "static inline int64_t extc_shiftCount(int64_t b, int64_t w, const char *f, int l) {\n    if (b < 0 || b >= w) extc_trapMsg(f, l, \"shift count out of range\");\n    return b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Checked index: the index is returned, so the caller evaluates it only once. */\n" },
    { PRIM_FUNC, "extc_checkedIndex", "static inline int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line) {\n    if (i < 0 || i >= n) extc_trap(file, line, i, n);\n    return i;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Checked slice: requires 0 <= lo <= hi <= n, and returns `lo`. */\n" },
    { PRIM_FUNC, "extc_checkedRange", "static inline int64_t extc_checkedRange(int64_t lo, int64_t hi, int64_t n,\n                          const char *file, int line) {\n    if (lo < 0 || hi < lo || hi > n) {\n        fprintf(stderr, \"%s:%d: trap: slice %lld..%lld is out of range (length %lld)\\n\",\n                file, line, (long long)lo, (long long)hi, (long long)n);\n        extc_die(1);\n    }\n    return lo;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n\n" },
    { PRIM_VAR, "extc_ablock", "typedef struct extc_ablock { struct extc_ablock *prev; int fresh; int64_t cap, used; char data[1]; } extc_ablock;\n" },
    { PRIM_VAR, "extc_arena", "typedef struct extc_arena { extc_ablock *top; extc_ablock *spare; } extc_arena;\n" },
    /* `any` is a by-value type in signatures (`fn log(v: any)`), so its typedef has to be in the
     * preamble, ahead of the prototypes; the struct itself is emitted with the rest of the `any`
     * runtime, before any body uses it. One row, mentioned by name, so a program without `any`
     * never carries it. */
    { PRIM_VAR, "ExtcAny", "typedef struct ExtcDesc ExtcDesc;\ntypedef struct ExtcAny { const struct ExtcDesc *desc; uint64_t u; } ExtcAny;\n" },
    { PRIM_SCAFFOLD, NULL, "#ifndef EXTC_ARENA_SPARE_MAX\n" },
    { PRIM_MACRO, NULL, "#define EXTC_ARENA_SPARE_MAX (1 << 20)\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_arena_init", "static inline void extc_arena_init(extc_arena *a) { a->top = NULL; a->spare = NULL; }\n" },
    { PRIM_FUNC, "extc_arena_release", "static inline void extc_arena_release(extc_arena *a) {\n    while (a->top) {\n        extc_ablock *p = a->top->prev;\n        /* Always keep one block back. Guarding this with \"the arena had more\n         * than one block\" would look like a saving and be the opposite: the\n         * block that pays off is precisely the single-block arena of a tight\n         * loop, where reusing one block avoids a malloc and a free per\n         * iteration. */\n        if (!a->spare && a->top->cap <= EXTC_ARENA_SPARE_MAX) {\n            a->spare = a->top; a->spare->prev = NULL; a->spare->fresh = 0;\n        } else {\n            free(a->top);\n        }\n        a->top = p;\n    }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_arena_destroy", "static inline void extc_arena_destroy(extc_arena *a) {\n    extc_arena_release(a);\n    if (a->spare) { free(a->spare); a->spare = NULL; }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Explicit conversions: narrowing, sign change, or float-to-integer. A value that\n * does not fit traps, reporting the source position. */\n" },
    { PRIM_FUNC, "extc_narrowI", "static inline int64_t extc_narrowI(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {\n    if (v < lo || v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n    return v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_narrowU", "static inline uint64_t extc_narrowU(uint64_t v, uint64_t hi, const char *f, int l) {\n    if (v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n    return v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_convFloat", "static inline int64_t extc_convFloat(double v, int64_t lo, int64_t hi, const char *f, int l) {\n    if (!(v >= (double)lo && v <= (double)hi)) extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n    return (int64_t)v;   /* truncation toward zero, as in C */\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* float -> u64: the range cannot be expressed with the int64_t bounds above\n * (UINT64_MAX does not fit in int64_t), so it is its own helper. 2^64 is\n * exactly representable as a double, which is why the upper test is `<`. */\n" },
    { PRIM_FUNC, "extc_convFloatU", "static inline uint64_t extc_convFloatU(double v, const char *f, int l) {\n    if (!(v >= 0.0 && v < 18446744073709551616.0)) extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n    return (uint64_t)v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n#if defined(__GNUC__) || defined(__clang__)\n" },
    { PRIM_MACRO, NULL, "#define EXTC_INLINE static inline __attribute__((always_inline))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_UNUSED __attribute__((unused))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_MALLOC __attribute__((malloc))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_NOINLINE __attribute__((noinline))\n" },
    { PRIM_SCAFFOLD, NULL, "#else\n" },
    { PRIM_MACRO, NULL, "#define EXTC_INLINE static inline\n" },
    { PRIM_MACRO, NULL, "#define EXTC_MALLOC\n" },
    { PRIM_MACRO, NULL, "#define EXTC_NOINLINE\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_arena_alloc", "static EXTC_MALLOC EXTC_NOINLINE void *extc_arena_alloc(extc_arena *a, int64_t n, const char *f, int l) {\n    if (n <= 0) n = 1;\n    n = (n + 7) & ~(int64_t)7;\n    /* Adopt the spare only when the arena is empty.\n     *\n     * This is one test on a pointer that is NULL in the common case, which\n     * matters: an allocation in a loop runs this code millions of times, and\n     * asking 'is the spare big enough for n' on every call costs extra work for\n     * no benefit -- the spare\n     * is a whole block, so `cap >= n` is already true whenever the arena is\n     * empty enough to want it. */\n    if (!a->top && a->spare) { a->top = a->spare; a->spare = NULL; a->top->used = 0; }\n    if (!a->top || a->top->cap - a->top->used < n) {\n        /* Block size: the request, grown by doubling, with a small floor.\n         *\n         * A flat floor costs that much per **task**: a coroutine's frame is ~100 bytes, so a\n         * flat 4096 floor costs 4 KB per task. Doubling keeps the malloc count of an allocation loop the same\n         * after a few blocks while a one-shot allocation (the common case in a coroutine)\n         * pays only for what it asks. */\n        int64_t cap = n > 64 ? n : 64;\n        if (a->top && a->top->cap < (INT64_C(1) << 20) && cap < a->top->cap * 2)\n            cap = a->top->cap * 2;\n        extc_ablock *b = (extc_ablock *)calloc(1, sizeof(extc_ablock) + (size_t)cap);\n        if (!b) { fprintf(stderr, \"%s:%d: trap: out of arena memory\"\n                        \" (this allocation wanted %lld bytes)\\n\",\n                        f, l, (long long)n); extc_die(1); }\n        b->prev = a->top; b->cap = cap; b->used = 0; b->fresh = 1;\n        /* `fresh` goes before `cap`/`used`: right before `data` it pushed the payload to\n         * offset 28, every allocation became 4-byte aligned, and storing an int64_t there\n         * is a misaligned store. */\n        a->top = b;\n    }\n    {\n        void *p = a->top->data + a->top->used;\n        a->top->used += n;\n        /* A block straight from `calloc` is zero **throughout**, and only the bytes not yet\n         * handed out are ever looked at again, so those need no clearing; a block that came\n         * back from the spare was written to in its previous life and does. The promise is\n         * unchanged either way: a byte read after its lifetime ends is an initialized byte\n         * (SPEC section 0.6). Skipping it matters for the one-shot large buffer: a 32 MiB\n         * `new` pays only for what it touches, since a fresh block is\n         * already zero and needs no `memset`. */\n        if (!a->top->fresh) memset(p, 0, (size_t)n);\n        return p;\n    }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n\n" },
};

/**
 * @brief Register the run-time primitive blocks as a table in emission order.
 *
 * `PRIM_BLOCK` is the block itself: scaffolding and definitions alternate. Each definition is
 * registered here with its name and kind, so the later dead-alive judgment reads the registration
 * instead of guessing from line shapes; a macro line's name is read out of the `#define` line itself.
 *
 * @param g  The code generation state; the registrations are written to `g->primEnts`.
 */
static void primEmit(CG *g) {
    for (size_t i = 0; i < sizeof PRIM_BLOCK / sizeof *PRIM_BLOCK; i++) {
        const PrimPiece *p = &PRIM_BLOCK[i];
        /* A macro's row carries no name: it is read from the `#define` line itself, which is
         * the one thing every macro row has. (Keeping it out of the table also keeps a bare
         * `EXTC_*` string literal out of the source: `tools/check_switches.py` reads any of
         * those as a compiler switch that has to be documented, and a macro of the *generated
         * C* is not one.) A function or variable row spells its name out. */
        const char *name = p->name;
        if (p->kind == PRIM_MACRO) {
            const char *mn = macroDefineName(p->text, strlen(p->text));
            size_t nl = 0;
            if (mn) while (identByte(mn[nl])) nl++;
            if (nl) {
                char *cp = arenaAlloc(g->arena, nl + 1);
                memcpy(cp, mn, nl);
                cp[nl] = '\0';
                name = cp;
            }
        }
        if (!name) continue;
        PrimEnt *e = arenaAllocZero(g->arena, sizeof *e);
        e->name = name;
        e->text = p->text;
        e->kind = p->kind;
        e->quiet = strcmp(name, "__extc_dying") == 0;
        *(PrimEnt **)vecPush(&g->primEnts) = e;
    }
}

/**
 * @brief Count how many times a name occurs in one piece of scaffolding text.
 *
 * A guard line (`#ifndef X` / `#ifdef X`) only asks whether a macro is defined, which is not a use, so
 * those lines are skipped whole when `skipGuards` is true: `#ifndef EXTC_REC_LIMIT` must not keep the
 * macro alive.
 *
 * @param g           The code generation state, passed through to `countMentionsIn`.
 * @param text        One piece of scaffolding text.
 * @param name        The identifier to count, matched as a whole identifier.
 * @param skipGuards  Whether to skip `#ifndef` / `#ifdef` lines.
 * @return            The occurrence count.
 */
static size_t primScaffoldMentions(CG *g, const char *text, const char *name, bool skipGuards) {
    size_t n = 0, len = strlen(text);
    for (size_t p = 0; p < len; ) {
        const char *ln = text + p;
        const char *nl = memchr(ln, '\n', len - p);
        size_t ll = nl ? (size_t)(nl - ln) : (len - p);
        bool guard = (ll > 8 && (strncmp(ln, "#ifndef ", 8) == 0 || strncmp(ln, "#ifdef ", 7) == 0));
        if (!(skipGuards && guard)) n += countMentionsIn(g, ln, ll, name);
        if (!nl) break;
        p += ll + 1;
    }
    return n;
}

/**
 * @brief Decide which definitions in the run-time primitive blocks this program needs, and splice the
 *        surviving text into the unit.
 *
 * The blocks are assembled outside the unit, so the decision happens **before** they become part of
 * the generated C: the unit, the blocks' scaffolding and the live definitions are searched against
 * each other to a fixed point. A definition whose name occurs only inside itself is dropped -- the
 * same criterion as the text-level pass, only asked of the registration table. The scaffolding
 * (comments, blank lines, the `#if/#else/#endif` skeleton) is always emitted, so the product is
 * byte-identical to "emit the whole block, then delete definitions".
 *
 * @param g    The code generation state; the `primEnts` filled in by `primEmit` are the evidence.
 * @param out  The generated C unit; the block is spliced at `g->primA`, with what precedes and
 *             follows it left as it is.
 */
static void primSplice(CG *g, Buf *out) {
    const size_t ntab = sizeof PRIM_BLOCK / sizeof *PRIM_BLOCK;
    const size_t np   = g->primEnts.len;
    if (!np) return;
    char *unit = bufCstr(out);                       /* the unit, without the block */
    bool *live = arenaAllocZero(g->arena, np * sizeof *live);
    for (size_t i = 0; i < np; i++) live[i] = true;
    for (;;) {
        bool changed = false;
        for (size_t i = 0; i < np; i++) {
            PrimEnt *e = *(PrimEnt **)vecAt(&g->primEnts, i);
            if (!live[i]) continue;
            size_t total = countMentions(unit, e->name);
            for (size_t p = 0; p < ntab; p++) {      /* the scaffolding is always there */
                if (PRIM_BLOCK[p].kind != PRIM_SCAFFOLD) continue;
                total += primScaffoldMentions(g, PRIM_BLOCK[p].text, e->name, e->kind == PRIM_MACRO);
            }
            for (size_t j = 0; j < np; j++) {
                if (!live[j]) continue;
                PrimEnt *o = *(PrimEnt **)vecAt(&g->primEnts, j);
                total += countMentionsIn(g, o->text, strlen(o->text), e->name);
            }
            size_t inside;
            if (e->kind == PRIM_MACRO) {
                /* What the macro consists of: every `#define` line of that name still standing
                 * (a macro may be defined once per branch of an `#if`). */
                inside = 0;
                for (size_t j = 0; j < np; j++) {
                    if (!live[j]) continue;
                    PrimEnt *o = *(PrimEnt **)vecAt(&g->primEnts, j);
                    if (o->kind != PRIM_MACRO || strcmp(o->name, e->name) != 0) continue;
                    inside += countMentionsIn(g, o->text, strlen(o->text), e->name);
                }
            } else {
                inside = countMentionsIn(g, e->text, strlen(e->text), e->name);
            }
            if (total != inside) continue;           /* something outside names it: keep */
            live[i] = false;
            changed = true;
            if (!e->quiet && dbgOn("EXTC_DBG_DCE"))
                fprintf(stderr, "[wl] prim %s\n", e->name);
        }
        if (!changed) break;
    }
    /* The block as it will be written: scaffolding plus the definitions that survived. */
    Buf blk;
    bufInit(&blk, g->arena);
    for (size_t p = 0, ei = 0; p < ntab; p++) {
        /* `kind` decides, not `name`: a macro row carries no name of its own (primEmit reads
         * it from the `#define` line), and reading `name == NULL` as scaffolding would emit
         * every macro unconditionally and shift every index after it. */
        if (PRIM_BLOCK[p].kind == PRIM_SCAFFOLD) { bufPuts(&blk, PRIM_BLOCK[p].text); continue; }
        if (live[ei++]) bufPuts(&blk, PRIM_BLOCK[p].text);
    }
    /* Splice it in at `primA`: the preamble before it, everything else after, so the block
    * lands in front of the coroutine runtime. */
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, out->data, g->primA);
    bufPuts(&nb, bufCstr(&blk));
    bufPutn(&nb, out->data + g->primA, out->len - g->primA);
    *out = nb;
}

/* One row per piece of the runtime primitive block, in emission order.
 * A row with `name` is a definition the worklist may leave out; a row without
 * one is scaffolding (comments, `#if/#else/#endif` skeletons) that is always
 * emitted, so a product whose primitive is unused looks exactly as it did when
 * the text-level pass removed the definition afterwards. Generated from the
 * primitive text; verified byte-for-byte. */

/**
 * @brief Emit the beginning of the generated file: feature macros, standard headers, and the few C
 *        types the prototypes need.
 *
 * Feature macros are honored only **before the first include**, so they must be written here, above
 * the header block, and only for programs that really need that POSIX/GNU interface: `g->needTime` /
 * `g->needDns` decide `_POSIX_C_SOURCE`, and `g->needHttpFile` decides `_GNU_SOURCE` (`clock_gettime`,
 * `nanosleep` and `getaddrinfo` are not visible under strict c11). The dyn and coroutine handle
 * typedefs are declared here too: prototypes and instance structs mention them first, and the runtime
 * is spliced in only at final assembly.
 *
 * @param g    The code generation state; `needTime`, `needDns`, `needHttpFile` and `needCoroHandle`
 *             are the emission switches.
 * @param out  The generated C unit, written from the beginning.
 * @param m    The module; `planUsesDyn` decides whether the dyn handle type is emitted.
 */
static void emitPreamble(CG *g, Buf *out, Module *m) {
    /* `-std=c11` is strict ISO, so the POSIX half of `<time.h>` (`clockid_t`, `CLOCK_MONOTONIC`,
     * `nanosleep`) is hidden. A feature-test macro only counts **before the first include** --
     * defining it later has no effect -- so it goes here, above the include block, and only for a
     * program that actually asks for the time (`needTime` is set by the declaration scan above,
     * which is why that scan has to run first). */
    /* `clock_gettime`/`nanosleep` are POSIX; `getaddrinfo`/`inet_ntop` are too (under strict c11
    * `struct addrinfo` is not even visible). The macro must come **before the first include**. */
    if (g->needTime || g->needDns)
        bufPuts(out, "#define _POSIX_C_SOURCE 200809L   /* clock_gettime/nanosleep/getaddrinfo */\n");
    /* The file-level HTTP date support uses `strptime`/`timegm`/`gmtime_r`: the first two are not
    * ISO C => `_GNU_SOURCE` is needed (`timegm` is not even POSIX). Enabled only when the program
    * really declares those shims. */
    if (g->needHttpFile)
        bufPuts(out, "#define _GNU_SOURCE 1   /* strptime/timegm/gmtime_r: the three HTTP date helpers */\n");
    bufPuts(out,
        "/* Generated by the extC compiler -- do not edit by hand.\n"
        " * The generated C is deliberately boring: no macro tricks, no clever optimizations.\n"
        " * `#line` directives map the C compiler's errors back to the .extc source lines.\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"      /* offsetof, needed by the descriptor tables */
        "#include <stdio.h>\n"
        "#include <stdarg.h>\n"   /* `extc_printf` (built-in printing and descriptor printing share the cout buffer) */
        "#include <string.h>\n"
        "#include <stdlib.h>\n\n");
    /* Which reclamation shape this product was built for, as one grep-able line in the product
     * head. The decision itself is made before the preamble: `scanUnitNeeds` reads the switches
     * and the all-acyclic upgrade (just above `emitPreamble`) may turn the sweep off and the
     * counting on. The marker is an **exhaustive** description of the runtime state and must not
     * contradict the `EXTC_RC_NOCOUNT` / `EXTC_RC_SWEEP_FREE` macros that follow:
     *   0 = no reclamation runtime at all (`needReclaim` false: the static analysis proved this
     *       unit reclaim-idle). **No line is emitted** for it: nothing may test a tier the product
     *       does not have, and a comment line in every product would churn all golden hashes.
     *       `EXTC_RECLAIM=0` removes the whole runtime, so it also lands here (no marker at all).
     *   1 = event tier: holder counting on (`EXTC_RC_NOCOUNT 0`), region sweeping off
     *       (`EXTC_RC_SWEEP_FREE 1`).
     *   2 = sweep tier: holder counting off (`EXTC_RC_NOCOUNT 1`), region sweeping on
     *       (`EXTC_RC_SWEEP_FREE 0`) -- the default.
     *   3 = counted+sweep tier: **both** on (`EXTC_RC_NOCOUNT 0`, `EXTC_RC_SWEEP_FREE 0`); reached
     *       when `EXTC_RC_NOCOUNT=0` forces counting on a cyclic unit, or `EXTC_RC_SWEEPFREE=0`
     *       forbids the event tier. */
    if (g->rc.reclaim) {
        int tier;
        if (g->rc.noCount == 0 && g->rc.sweepFree == 1)        tier = 1;
        else if (g->rc.noCount == 1 && g->rc.sweepFree != 1)   tier = 2;
        else if (g->rc.noCount == 0 && g->rc.sweepFree != 1)   tier = 3;
        else                                                 tier = 2;   /* not reachable; keep the sweep label */
        bufPrintf(out, "#define EXTC_RC_TIER %d   /* %s */\n", tier,
                  tier == 1 ? "event tier: holder counting on, region sweeping off"
                  : tier == 2 ? "sweep tier: holder counting off, region sweeping on"
                  : tier == 3 ? "counted+sweep tier: both on (NOCOUNT=0 forced)"
                              : "sweep tier: region sweeping on");
    }

    /* The dyn handle's C type is needed by **prototypes**: a `dyn Trait` parameter is a value of
    * this type, and prototypes are emitted before the runtime that defines it (the pool runtime is
    * spliced in during final assembly). Declaring it here closes that gap; C11 allows a typedef to
    * be repeated when it names the same type, so the runtime keeps its own copy. */
    {
        if (planUsesDyn(m))
            bufPuts(out,
                "#define EXTC_DYN_HANDLE_DEFINED 1\n"
                "struct ExtcDynHandleS { int64_t pid, slot, gen, pgen; };\n"
                "typedef struct ExtcDynHandleS ExtcDynHandle;\n\n");
        /* The coroutine handle, for the same reason as the dyn one: prototypes and the instance
         * structs (`vector<coroutine<T>>`) mention it before the runtime could define it. */
        if (g->needCoroHandle)
            bufPuts(out,
                "#define EXTC_CORO_HANDLE_DEFINED 1\n"
                "struct ExtcCoroS { void *frame; int64_t kind; int64_t task; };\n"
                "typedef struct ExtcCoroS extc_coro;\n\n");
    }
}

/**
 * @brief Record the splice point of the run-time block and emit the `@overwrite` cell type at the end
 *        of the unit.
 *
 * The primitive blocks (`primEmit`) and the coroutine runtime are assembled **right after** the unit,
 * and whether they enter the product is decided by `primSplice` later, so only the splice point
 * `g->primA` is recorded here: no pass deletes bytes before the block, so this offset is still
 * accurate at scan time. The `@overwrite` cell type contains `extc_arena *home` and must come after
 * the arena, so this function only computes `needOw` and the type itself is emitted in the arena
 * section.
 *
 * @param g    The code generation state.
 * @param out  The generated C unit; the cell type may be appended here.
 * @param m    The module.
 */
static void emitRuntimePrologue(CG *g, Buf *out, Module *m) {
    /* The reclamation runtime: the slab allocator, the holder counts and the region sweep.
     * It is emitted **before** any code that could name it -- its functions are `static` -- and
     * only when the program has a governed allocation point (`g->rc.reclaim`, narrowed by
     * `rcPlanDecide` from that criterion; `EXTC_RECLAIM=1` forces it on, `=0` off). */
    if (g->rc.reclaim) {
        /* The frame root chain (the [rt:chain] section in reclaim.c) has no emitter pushing into the chain
         * yet, so pushing code would be all dead => passing 0 emits nothing. The place to wire it up
         * is this 0: it becomes the corresponding codegen flag. */
        reclaimEmitRuntime(g->arena, out, g->rc.noCount, getenv("EXTC_RC_TIME") != NULL,
                           g->rc.sweepFree == 1, g->rcLocalStore == 1, 0);
        /* A region is claimed by **the arena's address**. The allocation point already knows which arena
         * it lands on (`arenaRefAt`), so the region's lifetime coincides exactly with that arena's --
         * including the case where the callee allocates with the caller's home arena (the home arena
         * is **one and the same address** handed all the way down), and including different frames of
         * the same level in recursion. The release point is emitted next to `extc_arena_destroy`
         * (`cgReclaimDropArena`): a block release does not touch the region, which lives until frame
         * destruction.
         *
         * When the table is full a new region is **not entered**: leaking a region is preferred over
         * freeing it early -- the same rule as "saturate and stick" for the count (obligation 3 in
         * `reclaim.c`).
         *
         * Placed after the runtime text and before the primitive block: it references only
         * `ExtcRcRegion` and `extc_rc_regionNew`/`extc_rc_regionFree`, all three provided by that text
         * above. The `unused` attribute is there because "the switch is on but the program has no
         * allocation point" is a legal input: neither function is called then, and the generated C must
         * still compile under `-Wall -Werror`. */
        bufPuts(out,
            "/* EXTC_RC_REG_FULL_POLICY 1: when the table is full, a new region is not entered and\n"
            " * is never dropped -- leaking a region beats freeing it early (see the note above).\n"
            " * `extc_rc_regWarned` makes the diagnostic one-shot. */\n"
            "#define EXTC_RC_REG_FULL_POLICY 1\n"
            "#define EXTC_RC_REG_MAX 512\n"
            "static void *extc_rc_regKey[EXTC_RC_REG_MAX];\n"
            "static int extc_rc_regWarned;\n"
            "static ExtcRcRegion *extc_rc_regVal[EXTC_RC_REG_MAX];\n"
            "static size_t extc_rc_regN;\n"
            "/* Every frame looks up its home arena once, in the prologue: the table usually holds\n"
            " * two or three entries, so a linear scan is enough, but the call/return itself is a third\n"
            " * of the cost, so the lookup is inlined into the prologue (`static inline` inlines at\n"
            " * -O2). */\n"
            "__attribute__((unused)) static inline ExtcRcRegion *extc_rc_regionOf(void *key, int32_t rid) {\n"
            "    for (size_t i = 0; i < extc_rc_regN; i++)\n"
            "        if (extc_rc_regKey[i] == key) return extc_rc_regVal[i];\n"
            "    ExtcRcRegion *r = extc_rc_regionNew(rid);\n"
            "    if (extc_rc_regN < EXTC_RC_REG_MAX) {\n"
            "        extc_rc_regKey[extc_rc_regN] = key;\n"
            "        extc_rc_regVal[extc_rc_regN] = r;\n"
            "        extc_rc_regN++;\n"
            "    } else if (!extc_rc_regWarned) {\n"
            "        /* The full-table path must not be silent: otherwise every later region leaks and nothing\n"
            "         * said so. Keep the leak (early free is worse), but say it once, under the same\n"
            "         * diagnostic switch as the rest of the timing story, so tests stay quiet. */\n"
            "        extc_rc_regWarned = 1;\n"
            "        /* Compiled in only when the product was built with EXTC_RC_TIME=1 (the                                   \n"
            "         * emitter's timing switch), so ordinary products and tests stay silent. */        \n"
            "#if EXTC_RC_TIME\n"
            "        fprintf(stderr, \"extc: rc region table full (EXTC_RC_REG_MAX=%d);\"                        \n"
            "                        \" later regions are never dropped\\n\", EXTC_RC_REG_MAX);                \n"
            "#endif\n"
            "    }\n"
            "    return r;\n"
            "}\n"
            "__attribute__((unused)) static void extc_rc_regionDrop(void *key) {\n"
            "    for (size_t i = 0; i < extc_rc_regN; i++) {\n"
            "        if (extc_rc_regKey[i] != key) continue;\n"
            "        extc_rc_regionFree(extc_rc_regVal[i]);\n"
            "        extc_rc_regN--;\n"
            "        extc_rc_regKey[i] = extc_rc_regKey[extc_rc_regN];\n"
            "        extc_rc_regVal[i] = extc_rc_regVal[extc_rc_regN];\n"
            "        return;\n"
            "    }\n"
            "}\n");
    }
    /* The dying hook goes between the includes and the trap paths that call it: `int32_t`
     * has to be known, and the definition has to precede every use. It is its own block
     * because the prologue around it is already at the 4095-byte limit C99 guarantees for
     * a string literal. */
    /* The dying hook is captured here, with the block, so the unreferenced-definition pass
    * can drop it together with the traps that call it: it must be part of the same block. */
    /* The @overwrite cell type is emitted only when it is really used: emitted
     * unconditionally it would add a line to the generated C of every program
     * and fill the golden files with noise. The type contains an
     * `extc_arena *home`, so it must come after `extc_arena`; therefore only
     * `needOw` is computed here and the typedef itself is emitted further down,
     * inside the arena section. */
    bool needOw = false;
    {
        for (size_t i = 0; i < m->funcs.len && !needOw; i++)
            if (planOwSites(*(FuncDef **)vecAt(&m->funcs, i)) > 0) needOw = true;
        for (size_t i = 0; i < m->structs.len && !needOw; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                if (planOwSites(*(FuncDef **)vecAt(&sd->methods, j)) > 0) { needOw = true; break; }
        }
        (void)needOw;   /* the typedef needs extc_arena, so it comes later */
    }
    /* The runtime primitive block: the dying hook first (every trap below calls it), then
     * the primitives themselves. It is assembled **beside** the unit, not into it: whether a
     * definition is used is decided from the registry (`primEmit`), just before the block is
     * spliced in, so a definition nothing uses never becomes part of the generated C. */
    g->primA = out->len;                 /* where the block will be spliced in */
    primEmit(g);


    g->coroDefPrinted = g->needCoroHandle;
    g->coroDefA = out->len;              /* the coroutine runtime goes after the block */


    if (needOw)
        bufPuts(out, "typedef struct extc_owcell { void *p; int64_t cap; extc_arena *home; } extc_owcell;\n");
}

/**
 * @brief Assemble the runtime text for the descriptor table, the generic printer and the structural
 *        equality comparison.
 *
 * The text goes into its own buffers (`g->rt`, `g->rtPrint`, `g->rtEq`) rather than straight into the
 * unit: only scalar descriptor lines are registered in `deadDefs` and unmentioned lines are deleted
 * later; `rtPrint` is emitted only when the program really calls printing; and an over-long string
 * literal is split into several `bufPuts` calls at C99's 4095-character limit.
 *
 * @param g  The code generation state, which owns the runtime buffers.
 */
static void buildRuntimeText(CG *g) {
    /* ==================================================================
    * The descriptor table and the single generic printer.
    *
    * One static descriptor per type, and the printing logic exists once per program:
    * this is not runtime reflection. The types, field offsets and variant names in the
    * descriptors are compile-time constants gcc can see through, so nothing is weakened
    * about a failure being loud when it cannot be proven impossible.
    * ================================================================== */
    bufPuts(&g->rt,
        "/* ---- Type descriptor table ----\n"
        /* Do **not** write the printing function's name inside this comment: in the runtime block text it is pruned by "delete when nobody mentions it", and that judgment counts **text** -- one mention in a comment is as good as a use. */
        " * One static entry per type; the printer exists once per program.\n"
        " * Meaning of `size`: for a scalar or struct it is sizeof(T); for an array or\n"
        " * view it is the element stride. A view always has the C layout\n"
        " * `{ T *data; int64_t len; }`.\n"
        " */\n"
        "enum {\n"
        "    EXTC_D_I8, EXTC_D_I16, EXTC_D_I32, EXTC_D_I64,\n"
        "    EXTC_D_U8, EXTC_D_U16, EXTC_D_U32, EXTC_D_U64,\n"
        "    EXTC_D_F32, EXTC_D_F64, EXTC_D_BOOL,\n"
        "    EXTC_D_ENUM,      /* table = const char *const[]; the tag sits at offset 0 */\n"
        "    EXTC_D_STRUCT,    /* table = ExtcField[] */\n"
        "    EXTC_D_ARRAY,     /* fixed-size array: `count` elements of `elem` */\n"
        "    EXTC_D_SLICE,     /* view: { T *data; int64_t len; } */\n"
        "    EXTC_D_TEXT,      /* slice<u8>: printed as text, unlike any other slice */\n"
        "    EXTC_D_REF        /* ref / ?ref: always printed as <ref> */\n"
        "};\n"
        "\n"
        "typedef struct ExtcDesc ExtcDesc;\n"
        "typedef struct { const char *name; size_t off; const ExtcDesc *desc; } ExtcField;\n"
        "\n"
        "struct ExtcDesc {\n"
        "    int             kind;\n"
        "    const char     *name;    /* display name of a struct or enum, for printing */\n"
        "    size_t          size;    /* scalar/struct = sizeof(T); array/view = element stride */\n"
        "    size_t          count;   /* number of fields, variants, or array elements */\n"
        "    const void     *table;   /* ExtcField[] or const char *const[] */\n"
        "    const ExtcDesc *elem;    /* element descriptor of an array or view */\n"
        "    /* Used by structural `==`; filled in for structs only, because a struct's `==`\n"
        "     * is user code (or library code) and can only be delegated to, never\n"
        "     * re-derived. codegen emits a one-line adapter for it. Every other kind is\n"
        "     * compared recursively by `extc_eq`, so this slot stays NULL. It sits last on\n"
        "     * purpose: an initializer that omits it still zero-fills it. */\n"
        "    bool          (*eq)(const void *a, const void *b);\n"
        "};\n"
        "\n"
        "/* Type-independent descriptors (reference, byte view, scalars), shared per program. */\n");
    /* Type-independent descriptors: one row per scalar kind, handed out by `descRef`
     * on demand. A program that never prints a `u64`, or never compares a `ref`, has
     * no use for that row - and a row nobody names is a warning in every unit that
     * carries it. Each row is emitted here and remembered: dropUnreferenced takes it
     * back out once the finished unit says nothing names it. */
    static const struct { const char *name; const char *row; } SCALAR_DESC[] = {
        { "extc_desc_ref", "static const ExtcDesc extc_desc_ref  = { EXTC_D_REF,  \"ref\",  sizeof(void *), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_text", "static const ExtcDesc extc_desc_text = { EXTC_D_TEXT, \"slice<u8>\", 1, 0, NULL, NULL, NULL };\n" },
        { "extc_desc_bool", "static const ExtcDesc extc_desc_bool = { EXTC_D_BOOL, \"bool\", sizeof(bool), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i8", "static const ExtcDesc extc_desc_i8  = { EXTC_D_I8,  \"i8\",  sizeof(int8_t),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_i16", "static const ExtcDesc extc_desc_i16 = { EXTC_D_I16, \"i16\", sizeof(int16_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i32", "static const ExtcDesc extc_desc_i32 = { EXTC_D_I32, \"i32\", sizeof(int32_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i64", "static const ExtcDesc extc_desc_i64 = { EXTC_D_I64, \"i64\", sizeof(int64_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u8", "static const ExtcDesc extc_desc_u8  = { EXTC_D_U8,  \"u8\",  sizeof(uint8_t),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_u16", "static const ExtcDesc extc_desc_u16 = { EXTC_D_U16, \"u16\", sizeof(uint16_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u32", "static const ExtcDesc extc_desc_u32 = { EXTC_D_U32, \"u32\", sizeof(uint32_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u64", "static const ExtcDesc extc_desc_u64 = { EXTC_D_U64, \"u64\", sizeof(uint64_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_f32", "static const ExtcDesc extc_desc_f32 = { EXTC_D_F32, \"f32\", sizeof(float),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_f64", "static const ExtcDesc extc_desc_f64 = { EXTC_D_F64, \"f64\", sizeof(double), 0, NULL, NULL, NULL };\n" },
    };
    for (size_t i = 0; i < sizeof SCALAR_DESC / sizeof *SCALAR_DESC; i++) {
        size_t rowOff = g->rt.len;
        bufPuts(&g->rt, SCALAR_DESC[i].row);
        Buf row;
        bufInit(&row, g->arena);
        bufPutn(&row, g->rt.data + rowOff, g->rt.len - rowOff);
        DeadDef *dd = arenaAllocZero(g->arena, sizeof *dd);
        dd->name = SCALAR_DESC[i].name;
        dd->text = bufCstr(&row);
        *(DeadDef **)vecPush(&g->deadDefs) = dd;
    }
    bufPuts(&g->rt, "\n");
    /* Split into two calls: C99 only guarantees support for string literals of
     * 4095 characters, and one large literal would trigger -Woverlength-strings.
     * That is not an error, but there is no reason to keep the noise. */
    bufPuts(&g->rtPrint,
        "/* Generic recursive printer. The output format must stay byte-for-byte identical to\n"
        " * the `_debug` helpers it replaces; the truth table lives in\n"
        " * tools/print-formats.txt (floats use %g, [N]u8 prints numerically, and\n"
        " * slice<u8> prints as text). */\n"
        /* No `__attribute__((unused))` here: this section is emitted **as a whole block**, and the printing
        * function is called only when the program really prints a structured type => any program that
        * needs the runtime block for another reason but prints no structured type would hit
        * `-Werror=unused-function`; `static inline` is no way out either (GCC accepts it, Clang counters
        * with `-Wunneeded-internal-declaration`).
        * This section is emitted **on demand**: `rtPrint` goes out only when there really is a call
        * site for it (`needPrint`), so the attribute can be dropped -- a function nobody calls never
        * enters the product. */
        "static void extc_print(const void *p, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:   extc_printf(\"%d\", (int)*(const int8_t *)p); return;\n"
        "    case EXTC_D_I16:  extc_printf(\"%d\", (int)*(const int16_t *)p); return;\n"
        "    case EXTC_D_I32:  extc_printf(\"%d\", (int)*(const int32_t *)p); return;\n"
        "    case EXTC_D_I64:  extc_printf(\"%lld\", (long long)*(const int64_t *)p); return;\n"
        "    case EXTC_D_U8:   extc_printf(\"%u\", (unsigned)*(const uint8_t *)p); return;\n"
        "    case EXTC_D_U16:  extc_printf(\"%u\", (unsigned)*(const uint16_t *)p); return;\n"
        "    case EXTC_D_U32:  extc_printf(\"%u\", (unsigned)*(const uint32_t *)p); return;\n"
        "    case EXTC_D_U64:  extc_printf(\"%llu\", (unsigned long long)*(const uint64_t *)p); return;\n"
        "    case EXTC_D_F32:  extc_printf(\"%g\", (double)*(const float *)p); return;\n"
        "    case EXTC_D_F64:  extc_printf(\"%g\", (double)*(const double *)p); return;\n"
        "    case EXTC_D_BOOL: extc_printf(\"%s\", *(const bool *)p ? \"true\" : \"false\"); return;\n"
        "    case EXTC_D_REF:  extc_printf(\"<ref>\"); return;\n"
        "    case EXTC_D_TEXT: {\n"
        "        int64_t n = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "        extc_printf(\"%.*s\", (int)n, (const char *)*(const void *const *)p);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ENUM: {\n"
        "        const char *const *names = (const char *const *)d->table;\n"
        "        int tag = *(const int *)p;\n"
        "        if (tag < 0 || (size_t)tag >= d->count) extc_printf(\"<%s>\", d->name);\n"
        "        else                                    extc_printf(\"%s\", names[tag]);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *f = (const ExtcField *)d->table;\n"
        "        extc_printf(\"%s { \", d->name);\n"
        "        for (size_t i = 0; i < d->count; i++) {\n"
        "            if (i) extc_printf(\", \");\n"
        "            extc_printf(\"%s: \", f[i].name);\n"
        "            extc_print((const char *)p + f[i].off, f[i].desc);\n"
        "        }\n"
        "        extc_printf(\" }\");\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *data;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t len = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "            data = (const char *)*(const void *const *)p;\n"
        "            n = len > 0 ? (size_t)len : 0;\n"
        "        } else {\n"
        "            data = (const char *)p;\n"
        "            n = d->count;\n"
        "        }\n"
        "        extc_printf(\"[\");\n"
        "        for (size_t i = 0; i < n; i++) {\n"
        "            if (i) extc_printf(\", \");\n"
        "            extc_print(data + i * d->size, d->elem);\n"
        "        }\n"
        "        extc_printf(\"]\");\n"
        "        return;\n"
        "    }\n"
        "    }\n"
        "}\n\n");

    /* Structural `==`, sharing the same descriptor table as `extc_print`.
     *
     * The semantics have to match the old per-array-type `_eq` bit for bit:
     *   - a scalar or an enum compares with `==` directly; an enum compares its
     *     tag only, and an enum with payloads is not comparable at all, which
     *     the checker rejects long before this point
     *   - an array recurses element by element, and a slice compares lengths
     *     first and then recurses element by element, exactly like
     *     `slice<T>::==` in the prelude
     *   - a struct delegates to the `fn ==` written by the user or the library,
     *     which is the `eq` slot of its descriptor, filled in by a one-line
     *     adapter that code generation emits
     * So one `_eq` function per array type became one unit of data per type,
     * rather than derived code per array type.
     */
    bufPuts(&g->rtEq,
        "static bool extc_eq(const void *a, const void *b, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:  return *(const int8_t *)a  == *(const int8_t *)b;\n"
        "    case EXTC_D_I16: return *(const int16_t *)a == *(const int16_t *)b;\n"
        "    case EXTC_D_I32: return *(const int32_t *)a == *(const int32_t *)b;\n"
        "    case EXTC_D_I64: return *(const int64_t *)a == *(const int64_t *)b;\n"
        "    case EXTC_D_U8:  return *(const uint8_t *)a  == *(const uint8_t *)b;\n"
        "    case EXTC_D_U16: return *(const uint16_t *)a == *(const uint16_t *)b;\n"
        "    case EXTC_D_U32: return *(const uint32_t *)a == *(const uint32_t *)b;\n"
        "    case EXTC_D_U64: return *(const uint64_t *)a == *(const uint64_t *)b;\n"
        "    case EXTC_D_F32: return *(const float *)a  == *(const float *)b;\n"
        "    case EXTC_D_F64: return *(const double *)a == *(const double *)b;\n"
        "    case EXTC_D_BOOL: return *(const bool *)a == *(const bool *)b;\n"
        "    case EXTC_D_REF:  return *(const void *const *)a == *(const void *const *)b;\n"
        "    case EXTC_D_ENUM: return *(const int *)a == *(const int *)b;\n"
        "    case EXTC_D_TEXT: {\n"
        "        const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "        const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "        if (la != lb) return false;\n"
        "        if (la <= 0) return true;      /* empty view: `data` may be null, so nothing to compare */\n"
        "        return memcmp(*(const void *const *)a, *(const void *const *)b, (size_t)la) == 0;\n"
        "    }\n"
        "    case EXTC_D_STRUCT:\n"
        "        /* The user-written `fn ==` through its adapter; a NULL slot means the\n"
        "         * two values should never have been compared. */\n"
        "        return d->eq ? d->eq(a, b) : false;\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *pa, *pb;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "            const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "            if (la != lb) return false;\n"
        "            pa = (const char *)*(const void *const *)a;\n"
        "            pb = (const char *)*(const void *const *)b;\n"
        "            n = la > 0 ? (size_t)la : 0;\n"
        "        } else {\n"
        "            pa = (const char *)a;\n"
        "            pb = (const char *)b;\n"
        "            n = d->count;\n"
        "        }\n"
        "        for (size_t i = 0; i < n; i++)\n"
        "            if (!extc_eq(pa + i * d->size, pb + i * d->size, d->elem)) return false;\n"
        "        return true;\n"
        "    }\n"
        "    }\n"
        "    return false;\n"
        "}\n\n");
}

/**
 * @brief Emit every C type definition this unit needs, in dependency order.
 *
 * An enum precedes the structs that use it: C11 cannot forward-declare an enum tag. A payload-carrying
 * enum emits only its tag constants here and leaves the `struct { tag; union }` definition to the
 * dependency-sorted section, because a payload can contain another struct by value. The closure of
 * generic instances is expanded to a fixed point here, exiting with an error after 64 rounds rather
 * than emitting fewer types.
 *
 * @param g     The code generation context; type text goes into its output buffer and pending-prune
 *              records are registered there.
 * @param m     The current module, which provides `types`, `structs` and `funcs`.
 * @param tt    The type table, which provides `instances`, `enumInstances` and `fnTypes`.
 * @param arena The scratch allocator for this round's type units and temporary text.
 */
static void emitTypes(CG *g, Module *m, TypeTable *tt, Arena *arena) {
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        if (!enumHasPayload(td)) {
            bufPuts(&b, "typedef enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&b, ", ");
                bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&b, " } %s;", td->name);
            cgLine(g, "%s", bufCstr(&b));
            cgLine(g, "");
        } else if (td->typeParams.len > 0) {
            continue;      /* generic enum: the instances own constants and definition */
        } else {
            /* With payloads: the tag constants can be emitted now, since they
             * depend on nothing, while the `struct { tag; union }` definition is
             * left to the dependency-ordered section below, because a payload
             * may contain another struct, as in a variant holding a
             * `slice<u8>`. */
            Buf e2;
            bufInit(&e2, arena);
            bufPrintf(&e2, "enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&e2, ", ");
                bufPrintf(&e2, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&e2, " };");
            cgLine(g, "%s", bufCstr(&e2));
            cgLine(g, "");
            continue;               /* the name table is deferred too: it reads the tag */
        }

        /* Nothing follows: an enum has textual names automatically, and they
         * come from the array of variant names in its descriptor, the `table`
         * of `<Type>_desc`, rather than from a derived `<Type>_name` function
         * emitted here. */
    }

    /* Collect everything that becomes a struct in C: ordinary structs, generic
     * instances, arrays, and enums with payloads. */
    Vec units;
    vecInit(&units, arena, sizeof(void *));
    /* Deduplicated by C name: `slice<mut slice<T>>` and `slice<slice<T>>` are
     * both `slice_slice_T`, while `ttEquals` counts `mut` as part of the
     * identity, so two instances can share one name - emitting both would
     * redefine the struct. */
    for (size_t i = 0; i < g->structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g->structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = it->sdef;              /* NULL for an array */
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* An enum with payloads belongs here too: its union holds the payload
     * types by value. */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!enumHasPayload(td)) continue;
        if (td->typeParams.len > 0) continue;    /* generic enum: instances below */
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = td;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* Each instance of a generic enum (`option<i64>`) is a struct definition of
     * its own. Two instances can share one C name -- `option<mut slice<u8>>` and
     * `option<slice<u8>>` are both `option_slice_u8`, because `mut` changes the
     * extC type but not the C layout -- and emitting both redefines the struct,
     * the typedef and the tag enum. The first one wins, which is the same rule
     * `addInstanceUnit` applies to instances reached through method signatures. */
    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        bool dup = false;
        for (size_t k = 0; k < units.len && !dup; k++)
            dup = strcmp(unitName(*(SUnit **)vecAt(&units, k)), it->name) == 0;
        if (dup) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = it->edef;
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* Coroutine frames are synthesized structs but real types: scanning their
     * fields is what makes the instances those fields mention (`vector<i32>`,
     * behind a container) get generated at all. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || planTemplate(cf) || !planCoroFrameType(cf) || !planCoroFrameType(cf)->sdef) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = planCoroFrameType(cf)->sdef;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&g->funcs, i);
        if (!cf || !planIsCoro(cf) || !planTemplate(cf) || !planCoroFrameType(cf) || !planCoroFrameType(cf)->sdef) continue;
        if (coroProvisional(&cf->targs)) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = planCoroFrameType(cf)->sdef;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    /* Run to a fixed point, adding the instances that method signatures
     * mention. The round count is capped, and passing the cap means something
     * unexpected is happening, so it is reported loudly rather than quietly
     * generating too little. */
    {
        bool grew = true;
        int round = 0;
        const char *srcName = NULL, *newName = NULL;   /* names the expansion */
        while (grew && round < 64) {
            grew = false;
            round++;
            size_t cur = units.len;                 /* only this round's entries */
            for (size_t i = 0; i < cur; i++) {
                SUnit *u = *(SUnit **)vecAt(&units, i);
                size_t before = units.len;
                scanUnitForUnits(arena, tt, &units, u);
                if (units.len != before) {
                    grew = true;
                    srcName = unitName(u);
                    for (size_t k = before; k < units.len; k++)
                        newName = unitName(*(SUnit **)vecAt(&units, k));
                }
            }
        }
        if (grew) {
            /* The cap guards against instances that nest without end; passing
             * it also names which instance expanded into which. */
            fprintf(stderr,
                    "extc: error: generic instance closure did not settle in 64 rounds.\n"
                    "      last expansion: `%s` mentions `%s`, which needs more instances.\n"
                    "      This usually means instances nest without bound (such as\n"
                    "      `option<option<option<...>>>`). Use a concrete type, or report\n"
                    "      this program if you think it should compile.\n",
                    srcName ? srcName : "?", newName ? newName : "?");
            exit(1);
        }
    }

    /* Every typedef comes first: a pointer field (`ref T`) needs nothing more. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        cgLine(g, "typedef struct %s %s;", unitName(u), unitName(u));
    }
    if (units.len) cgLine(g, "");

    /* The function-type typedefs (`fn(A) -> R`), one per signature, go after the
     * enum typedefs and the struct forward typedefs (a signature names the types
     * it takes and returns) and before the struct definitions (a field of
     * function type names its typedef). A function type cannot be spelled as a
     * prefix of a declaration, so it is spelled by a typedef and `cType` answers
     * with the typedef's name. `tt->fnTypes` is the demand set, and a nested
     * signature is always interned before the one that mentions it. */
    if (tt->fnTypes.len) {
        cgLine(g, "/* ---- function types (`fn(A) -> R`): one typedef per signature ---- */");
        for (size_t i = 0; i < tt->fnTypes.len; i++) {
            Type *ft = *(Type **)vecAt(&tt->fnTypes, i);
            if (!ft->name) continue;
            /* Two distinct signatures must never share one C name; a collision is
             * reported rather than emitted as C that does not compile. */
            bool clash = false;
            for (size_t j = 0; j < i && !clash; j++)
                clash = strcmp((*(Type **)vecAt(&tt->fnTypes, j))->name, ft->name) == 0;
            for (size_t j = 0; j < m->structs.len && !clash; j++)
                clash = strcmp((*(StructDef **)vecAt(&m->structs, j))->name, ft->name) == 0;
            for (size_t j = 0; j < m->types.len && !clash; j++)
                clash = strcmp((*(TypeDef **)vecAt(&m->types, j))->name, ft->name) == 0;
            if (clash) {
                Buf tb;
                bufInit(&tb, g->arena);
                ttRender(ft, &tb);
                ctxError(g->ctx, 1, 1,
                         "The compiler names the C typedef of a function type `extc_fn_<return>__"
                         "<argument>...`. Give this declaration another name.",
                         "`%s` is a name the compiler needs for the function type `%s`",
                         ft->name, bufCstr(&tb));
                continue;
            }
            cgLine(g, "typedef %s (*%s)(%s);", cType(g, ft->ret), ft->name, fnParamList(g, ft));
        }
        cgLine(g, "");
    }

    /* Compute the dependencies: another struct-like type held by value in a
     * field. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (u->td) {                    /* enum: payloads live in the union by value */
            for (size_t j = 0; j < u->td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&u->td->variants, j);
                for (size_t k = 0; k < v->types.len; k++) {
                    Type *pt = *(Type **)vecAt(&v->types, k);
                    if (u->inst)                 /* instance: substitute the arguments */
                        pt = ttSubstitute(tt, pt, &u->td->typeParams, &u->inst->targs);
                    if (pt->kind == TY_REF) continue;
                    int d = unitFind(&units, pt);
                    if (d >= 0 && d != (int)i) *(int *)vecPush(&u->deps) = d;
                }
            }
            continue;
        }
        if (u->inst && u->inst->kind == TY_ARRAY) {
            int j = unitFind(&units, u->inst->inner);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
            continue;
        }
        for (size_t k = 0; k < u->sd->fields.len; k++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, k);
            Type *ft = fd->type;
            if (u->inst) ft = ttSubstitute(tt, ft, &u->sd->typeParams, &u->inst->targs);
            if (ft->kind == TY_REF) continue;
            int j = unitFind(&units, ft);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
        }
    }

    /* Emit the definitions in dependency order. A cycle, which would have to go
     * through a value and which C forbids anyway, is emitted regardless and left
     * to the C compiler to report. */
    for (;;) {
        bool progressed = false, allDone = true;
        for (size_t i = 0; i < units.len; i++) {
            SUnit *u = *(SUnit **)vecAt(&units, i);
            if (u->done) continue;
            allDone = false;
            bool ready = true;
            for (size_t k = 0; k < u->deps.len && ready; k++)
                ready = (*(SUnit **)vecAt(&units, *(int *)vecAt(&u->deps, k)))->done;
            if (!ready) continue;
            unitBody(g, u);
            u->done = true;
            progressed = true;
        }
        if (allDone || !progressed) break;
    }
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (!u->done) { unitBody(g, u); u->done = true; }
    }


    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        TypeDef *td = it->edef;
        /* Skip a repeat of a C name (see the unit list above): its tag constants
         * were emitted by the first instance, and a second enum with the same
         * enumerators is a redefinition. */
        bool dup = false;
        for (size_t k = 0; k < i && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&tt->enumInstances, k))->name, it->name) == 0;
        if (dup) continue;
        /* The tag constants of the instance (`maybe_i64_nothing = 0`) carry the
         * same values as the base type, both following the variant order, so the
         * rule that the zero value has tag 0 holds for an instance as well. */
        Buf tb;
        bufInit(&tb, arena);
        bufPuts(&tb, "enum { ");
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&tb, ", ");
            bufPrintf(&tb, "%s_%s = %zu", it->name, v->name, j);
        }
        bufPuts(&tb, " };");
        cgLine(g, "%s", bufCstr(&tb));
    }
}

/**
 * @brief One module-level binding whose type carries an `@drop` hook.
 *
 * Collected by `emitGlobals` so the exit hook can walk them; see `CG.globalDrops`.
 */
typedef struct {
    const char *name;    /**< The C name of the `static` object. */
    Type       *declT;   /**< Its declared type, for the drop walk. */
} GlobalDrop;

/**
 * @brief Emit `static` definitions for the module's globals and constants, registering every line in
 *        the unreferenced-pruning table.
 *
 * A fixed-size global needs no arena; C zeroes static storage itself, so a global without an
 * initializer gets none written. `static` is deliberate: external linkage makes gcc give up vectorizing
 * the outer loop. A global initializer can mention function names (a `fn` field table), and C requires
 * them to be declared first, so initializers are walked once to emit their prototypes beforehand.
 *
 * @param g  The code generation context; `out` receives the definition text and `deadDefs` the
 *           prunable entries.
 * @param m  The current module; `globals` is the list to emit.
 */
static void emitGlobals(CG *g, Module *m) {
    /* Globals and constants are plain C static objects; a global of fixed size
     * needs no arena, and C zeroes a static object by itself. `static` is
     * required for performance: external linkage makes gcc reject outer-loop
     * vectorization. */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(g, gd->ann);
        /* A global may name functions (a table of `fn` fields); C needs them declared first. */
        if (gd->init) {
            AstVisit v = { globalFnProto, globalFnProtoStmt, g };
            astWalkExprChildren(gd->init, &v);
        }
        size_t before = g->out->len;                 /* recorded for dropUnreferenced */
        if (gd->init) {
            cgLine(g, "static %s %s = %s;", ct, gd->name, genGlobalInit(g, gd->init));
        } else {
            cgLine(g, "static %s %s;", ct, gd->name);
        }
        /* A global the program never names costs a warning in every translation
         * unit that carries it - and a `use std::io::*` carries the library's
         * streams and constants into every program. The line just written is
         * remembered; dropUnreferenced takes it back out if nothing names it. */
        Buf line;
        bufInit(&line, g->arena);
        bufPutn(&line, g->out->data + before, g->out->len - before);
        DeadDef *d = arenaAllocZero(g->arena, sizeof *d);
        d->name = gd->name;
        d->text = bufCstr(&line);
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        /* With reclamation on, a global reference slot is a **process-level root**: every trigger point's
         * root table must list it. A global is file-level `static`, so its name is visible in every
         * function body. When a type's references cannot be enumerated => every function's trigger is
         * turned off. */
        if (g->rc.reclaim && gd->ann) {
            RcScan sc = rcScanType(g, gd->ann, 0);
            if (sc == RC_CONSERVATIVE) g->rcGlobalsUnreliable = true;
            else if (sc == RC_LEAF_REFS)
                *(const char **)vecPush(&g->rcGlobalRoots) = gd->name;
        }
        /* A global that owns state gets its hook run at exit; see `CG.globalDrops`. */
        if (gd->ann && typeNeedsDrop(g, gd->ann, 0)) {
            GlobalDrop *d = arenaAllocZero(g->arena, sizeof *d);
            d->name = gd->name;
            d->declT = gd->ann;
            *(GlobalDrop *)vecPush(&g->globalDrops) = *d;
            g->needGlobalDrops = true;
        }
    }
    if (m->globals.len) cgLine(g, "");
}

/* The scan state: the generator plus the visitor, so the callbacks can recurse (the `astWalk*`
 * helpers visit one level of children, and a function-pointer use can sit any number of levels
 * down). */
typedef struct {
    CG       *g;
    AstVisit  v;
} LamThunkScan;

static bool lamThunkScanExpr(void *ctx, Expr *e);
static bool lamThunkScanStmt(void *ctx, Stmt *s);

/**
 * @brief Inspect one expression and recurse into its children.
 *
 * @param ctx  The scan state.
 * @param e    The expression to inspect.
 * @return     Always true: the walk never stops early.
 */
static bool lamThunkScanExpr(void *ctx, Expr *e) {
    LamThunkScan *sc = (LamThunkScan *)ctx;
    if (e && e->kind == EX_LAMBDA && planLamFnPtr(e)) {
        StructDef *sd = e->u.lambda.sdef;
        /* An environment with one layout per instance gets its thunk from that layout
         * (`emitLamVars`), not from the unit-wide list: its signature is the instance's. */
        if (sd && sd->methods.len == 1 && !lamEnvNeedsVariants(sd)) {
            bool seen = false;
            for (size_t i = 0; i < sc->g->lamThunks.len && !seen; i++)
                seen = *(StructDef **)vecAt(&sc->g->lamThunks, i) == sd;
            if (!seen) *(StructDef **)vecPush(&sc->g->lamThunks) = sd;
        }
    }
    return astWalkExprChildren(e, &sc->v);
}

/**
 * @brief Inspect one statement's children.
 *
 * @param ctx  The scan state.
 * @param s    The statement to walk.
 * @return     Always true: the walk never stops early.
 */
static bool lamThunkScanStmt(void *ctx, Stmt *s) {
    LamThunkScan *sc = (LamThunkScan *)ctx;
    return astWalkStmtChildren(s, &sc->v);
}

/**
 * @brief Collect the lambda environments that need a function-pointer thunk.
 *
 * The prototypes have to be emitted before any body is generated, and a use site can sit in any
 * body, so the whole unit is scanned first. `g->funcs` is the set that will be emitted, which is
 * exactly the set whose bodies can mention a thunk.
 *
 * @param g  The generator state; `lamThunks` is filled in.
 */
static void collectLamThunks(CG *g) {
    LamThunkScan sc;
    sc.g = g;
    sc.v.expr = lamThunkScanExpr;
    sc.v.stmt = lamThunkScanStmt;
    sc.v.ctx  = &sc;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && f->body) astWalkStmtChildren(f->body, &sc.v);
    }
}

/**
 * @brief Emit every collected lambda thunk (prototypes were already emitted).
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamThunks(CG *g) {
    for (size_t i = 0; i < g->lamThunks.len; i++)
        emitLamThunk(g, *(StructDef **)vecAt(&g->lamThunks, i));
}

