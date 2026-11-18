/**
 * @file places.c
 * @brief Emits the place registry and the dyn object table: the storage a value borrows for a
 *        scope it does not own, and the type-erased handle that names it.
 *
 * Two pieces of runtime live here, and both are about one idea: a lifetime decided at compile time
 * and enforced by a comparison at run time.
 *
 *  - A place is a slot in a table plus a chunk chain it bump-allocates payloads from. Entering one
 *    pushes it (the mark handed back is its slot id), leaving pops to a mark, and a task owns a
 *    detached root slot. Whatever the place allocated goes back in one release, so the cost of a
 *    leave is bounded by the number of chunks the place took -- a compile-time constant times the
 *    chunk count, never a walk over the objects it held.
 *  - A dyn value is {place slot, table slot, slot generation, place generation}. Copying it copies
 *    four integers and touches no counter, which is exactly why its payload cannot live in an rc
 *    slot: every copy of a reference-counted handle would need a registration. The payload goes
 *    into the place instead, and staleness is two comparisons (the slot is still ours, the place
 *    is still the same generation), never a read of freed storage.
 *
 * The text is one C literal per line, in emission order, and the concatenation is the runtime that
 * really runs in the generated file. That layout is not cosmetic: one literal per function would
 * pass 4095 characters and trip -Woverlength-strings, and this runtime is readable generated
 * output -- one line per entry reviews better than one escaped block.
 *
 * Both emitters are called at most once per translation unit, gated by the code generator: the
 * place registry only when the unit has a place at all (g->needPool) and the dyn table only when
 * the unit puts a value behind a trait object (planUsesDyn). A program that never enters a place
 * carries neither a slot table nor a chunk.
 */

#include "places.h"


static const char *const PLACE_RT[] = {
    "/* ---- places: the lifetime scopes a dyn value borrows [place] ----\n",
    " *\n",
    " * A place is a scope of storage, not a value: entering one pushes a slot id, leaving pops to a\n",
    " * mark, and everything the place allocated is handed back at once. The checker decides at compile\n",
    " * time which place a value belongs to, so the runtime never searches: it keeps a slot table (a\n",
    " * parent link for the outer place, a depth to tell a task's root from a pushed place), one stack\n",
    " * of live slots, and per slot a chain of malloc chunks that payloads bump-allocate from.\n",
    " *\n",
    " * Slots are recycled through a free list and every birth bumps the slot's generation, so a handle\n",
    " * that outlived its place fails a comparison instead of reading freed storage.\n",
    " */\n",
    "typedef struct ExtcPlaceChunk { struct ExtcPlaceChunk *next; size_t cap, used; } ExtcPlaceChunk;\n",
    "typedef struct ExtcPlace {\n",
    "    int64_t         gen;      /* bumped on every birth: a handle from an earlier life is stale */\n",
    "    int64_t         parent;   /* enclosing place, -1 for a root; the free-list link while dead */\n",
    "    int64_t         depth;    /* 0 for a root (a task's place), >= 1 for a pushed place */\n",
    "    int64_t         live;\n",
    "    ExtcPlaceChunk *chunks;\n",
    "} ExtcPlace;\n",
    "static ExtcPlace *extc_places = NULL;\n",
    "static int64_t    extc_placeCap = 0;\n",
    "static int64_t    extc_placeN = 0;\n",
    "static int64_t    extc_placeFree = -1;\n",
    "int64_t extc_placeTop = -1;\n",
    "/* Chunk sizes: the first chunk is one page, each later chunk doubles up to 256 KiB and stays there, and a payload\n",
    " * larger than that gets a chunk of exactly its own size. Both numbers are compile-time constants,\n",
    " * so the space a place can waste is bounded, not a function of how the program used it. */\n",
    "#ifndef EXTC_PLACE_CHUNK_MIN\n",
    "#define EXTC_PLACE_CHUNK_MIN 4096\n",
    "#endif\n",
    "#ifndef EXTC_PLACE_CHUNK_MAX\n",
    "#define EXTC_PLACE_CHUNK_MAX (256 << 10)\n",
    "#endif\n",
    "static int64_t extc_placeSlot(void) {\n",
    "    int64_t zi;\n",
    "    ExtcPlace *p;\n",
    "    if (extc_placeFree >= 0) {\n",
    "        zi = extc_placeFree;\n",
    "        extc_placeFree = extc_places[zi].parent;   /* the free list threads through parent */\n",
    "    } else {\n",
    "        if (extc_placeN == extc_placeCap) {\n",
    "            int64_t want = extc_placeCap ? extc_placeCap * 2 : 64;\n",
    "            ExtcPlace *grown = (ExtcPlace *)realloc(extc_places, (size_t)want * sizeof *grown);\n",
    "            if (!grown) return -1;\n",
    "            memset(grown + extc_placeCap, 0, (size_t)(want - extc_placeCap) * sizeof *grown);\n",
    "            extc_places = grown;\n",
    "            extc_placeCap = want;\n",
    "        }\n",
    "        zi = extc_placeN++;\n",
    "    }\n",
    "    p = &extc_places[zi];\n",
    "    p->gen++;\n",
    "    p->parent = -1;\n",
    "    p->depth  = 0;\n",
    "    p->live   = 1;\n",
    "    p->chunks = NULL;\n",
    "    return zi;\n",
    "}\n",
    "/* Giving a place back: free its chunk chain and put the slot on the free list. The work is\n",
    " * bounded by the number of chunks the place took, never by the number of payloads it holds. */\n",
    "static void extc_placeDrop(int64_t zi) {\n",
    "    ExtcPlaceChunk *c;\n",
    "    if (zi < 0 || zi >= extc_placeN || !extc_places[zi].live) return;\n",
    "    c = extc_places[zi].chunks;\n",
    "    while (c) { ExtcPlaceChunk *next = c->next; free(c); c = next; }\n",
    "    extc_places[zi].chunks = NULL;\n",
    "    extc_places[zi].live   = 0;\n",
    "    extc_places[zi].parent = extc_placeFree;\n",
    "    extc_placeFree = zi;\n",
    "}\n",
    "/* Push a place: the mark it hands back is its slot id. */\n",
    "int64_t extc_place_enter(void) {\n",
    "    int64_t zi = extc_placeSlot();\n",
    "    if (zi < 0) return -1;\n",
    "    extc_places[zi].parent = extc_placeTop;\n",
    "    extc_places[zi].depth  = (extc_placeTop >= 0) ? extc_places[extc_placeTop].depth + 1 : 1;\n",
    "    extc_placeTop = zi;\n",
    "    return zi;\n",
    "}\n",
    "/* Leave down to a mark, the mark included: a mark is a slot id, so this walks the parent chain --\n",
    " * return, break and continue each leave one, and the walk cannot overrun an unrelated task's\n",
    " * place. */\n",
    "void extc_place_leaveTo(int64_t mark) {\n",
    "    while (extc_placeTop >= 0) {\n",
    "        int64_t zi     = extc_placeTop;\n",
    "        int64_t parent = extc_places[zi].parent;\n",
    "        extc_placeDrop(zi);\n",
    "        extc_placeTop = parent;\n",
    "        if (zi == mark) break;\n",
    "    }\n",
    "}\n",
    "/* The place a task owns: a slot that is NOT on the block stack, so ending the task releases\n",
    " * exactly its own storage and never pops a place that is still alive. While the task is stepped,\n",
    " * extc_placeTop points at it, so block places inside the step nest above it normally. */\n",
    "int64_t extc_place_newRoot(void) { return extc_placeSlot(); }\n",
    "void extc_place_freeRoot(int64_t zi) { extc_placeDrop(zi); }\n",
    "/* The outer place of the current one: what a promotion names when it hands storage outwards. */\n",
    "int64_t extc_place_outer(void) { return (extc_placeTop >= 0) ? extc_places[extc_placeTop].parent : -1; }\n",
    "/* How deep the place stack is right now: a count, not an id. */\n",
    "int64_t extc_place_depth(void) { return (extc_placeTop >= 0) ? extc_places[extc_placeTop].depth : 0; }\n",
    "int64_t extc_place_live(int64_t zi) {\n",
    "    return (zi >= 0 && zi < extc_placeN && extc_places[zi].live) ? 1 : 0;\n",
    "}\n",
    "/* 0 is never a live generation (a slot is born at 1): a handle that stored 0 cannot match. */\n",
    "int64_t extc_place_gen(int64_t zi) { return extc_place_live(zi) ? extc_places[zi].gen : 0; }\n",
    "/* The slot table as a whole, for the task collector: it walks every slot and asks which are roots\n",
    " * that no registered handle reaches. A root is born at depth 0 and a pushed place never is, so the\n",
    " * shape is decided here, not by the walker. */\n",
    "int64_t extc_place_count(void) { return extc_placeN; }\n",
    "int64_t extc_place_isRoot(int64_t zi) {\n",
    "    return (zi >= 0 && zi < extc_placeN && extc_places[zi].live && extc_places[zi].depth == 0) ? 1 : 0;\n",
    "}\n",
    "/* Payload memory: bump-allocate from chunks this place owns, zeroed. Zeroed because the language\n",
    " * promises that fresh storage reads as zero -- chunks are calloc'ed and bump allocation never\n",
    " * revisits a byte. */\n",
    "void *extc_place_alloc(int64_t zi, int64_t bytes) {\n",
    "    ExtcPlace *p;\n",
    "    ExtcPlaceChunk *c;\n",
    "    int64_t cap;\n",
    "    void *r;\n",
    "    if (zi < 0 || zi >= extc_placeN || !extc_places[zi].live) return NULL;\n",
    "    if (bytes < 0) bytes = 0;\n",
    "    bytes = (bytes + 15) & ~(int64_t)15;   /* 16-byte aligned: a payload may hold any type */\n",
    "    p = &extc_places[zi];\n",
    "    c = p->chunks;\n",
    "    if (!c || c->used + (size_t)bytes > c->cap) {\n",
    "        cap = EXTC_PLACE_CHUNK_MIN;\n",
    "        if (c) {\n",
    "            cap = (int64_t)c->cap * 2;\n",
    "            if (cap > EXTC_PLACE_CHUNK_MAX) cap = EXTC_PLACE_CHUNK_MAX;\n",
    "        }\n",
    "        if (cap < bytes) cap = bytes;\n",
    "        c = (ExtcPlaceChunk *)calloc(1, sizeof *c + (size_t)cap);\n",
    "        if (!c) return NULL;\n",
    "        c->cap  = (size_t)cap;\n",
    "        c->used = 0;\n",
    "        c->next = p->chunks;\n",
    "        p->chunks = c;\n",
    "    }\n",
    "    r = (void *)((char *)(c + 1) + c->used);\n",
    "    c->used += (size_t)bytes;\n",
    "    return r;\n",
    "}\n",
};

static const char *const PLACE_DYN_RT[] = {
    "/* ---- dyn: a type-erased value whose lifetime is its place [dyn] ----\n",
    " *\n",
    " * The handle is {place slot, table slot, two generations}. Copying a dyn value copies four\n",
    " * integers and touches no counter -- that is the whole reason the payload lives in a place and\n",
    " * not in an rc slot: a reference-counted copy would have to be registered at every copy site, and\n",
    " * the type is copied everywhere by design.\n",
    " *\n",
    " * A table slot is dead when its place is gone, and also when the place SLOT it names has been\n",
    " * handed out again: a loop body's place reuses one slot every round, so a live place is not\n",
    " * enough -- the generation the slot was filled with has to match too. Both checks are\n",
    " * comparisons, never a read of freed storage, and extc_dynSweep acts on them only when the table\n",
    " * is full, so a program that never piles up dead handles never pays for it.\n",
    " */\n",
    "#ifndef EXTC_DYN_HANDLE_DEFINED\n",
    "#define EXTC_DYN_HANDLE_DEFINED 1\n",
    "/* Tagged, and four fields: gen is the SLOT generation (recycling) and zgen the PLACE generation\n",
    " * (the place going away, or its slot being handed out again) -- two independent ways to become\n",
    " * stale, both checked before dispatch. The tag matters: emitted twice, and two anonymous structs\n",
    " * differ. */\n",
    "struct ExtcDynHandleS { int64_t zi, slot, gen, zgen; };\n",
    "typedef struct ExtcDynHandleS ExtcDynHandle;\n",
    "#endif\n",
    "typedef struct ExtcDynSlot {\n",
    "    int64_t     zi;     /* place holding the payload, -1 while the slot is free */\n",
    "    int64_t     zgen;   /* that place's generation when this slot was filled */\n",
    "    int64_t     gen;    /* bumps every time this slot is reused */\n",
    "    int64_t     next;   /* free-list link */\n",
    "    void       *addr;\n",
    "    const void *vt;\n",
    "} ExtcDynSlot;\n",
    "static ExtcDynSlot *extc_dynSlots = NULL;\n",
    "static int64_t      extc_dynCap   = 0;\n",
    "static int64_t      extc_dynN     = 0;\n",
    "static int64_t      extc_dynFree  = -1;\n",
    "static int64_t      extc_dynLiveN = 0;\n",
    "EXTC_NORETURN static void extc_dynTrap(const char *file, int line, const char *why) {\n",
    "    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, why);\n",
    "    extc_die(1);\n",
    "}\n",
    "static void extc_dynSweep(void) {\n",
    "    int64_t i;\n",
    "    for (i = 0; i < extc_dynN; i++) {\n",
    "        int64_t zi = extc_dynSlots[i].zi;\n",
    "        if (zi < 0) continue;\n",
    "        if (!extc_place_live(zi) || extc_place_gen(zi) != extc_dynSlots[i].zgen) {\n",
    "            extc_dynSlots[i].gen++;\n",
    "            extc_dynSlots[i].zi = -1;\n",
    "            extc_dynSlots[i].next = extc_dynFree;\n",
    "            extc_dynFree = i;\n",
    "            extc_dynLiveN--;\n",
    "        }\n",
    "    }\n",
    "}\n",
    "int64_t extc_dyn_liveSlots(void) { return extc_dynLiveN; }\n",
    "ExtcDynHandle extc_dyn_put(const void *payload, int64_t size, const void *vt, int64_t zi) {\n",
    "    ExtcDynHandle h;\n",
    "    ExtcDynSlot *s;\n",
    "    int64_t slot;\n",
    "    /* The place is named by the caller (zoneArgRef), not read off the stack: a promoted site\n",
    "     * constructs inside an inner block while its payload has to be born in an outer place. A place\n",
    "     * that is not live means the value outlived the level the checker gave it -- a trap, not a\n",
    "     * silent attachment to whatever place happens to be current. */\n",
    "    if (!extc_place_live(zi))\n",
    "        extc_dynTrap(\"?\", 0, \"a dyn value needs a live place to live in\");\n",
    "    if (extc_dynFree < 0 && extc_dynN >= extc_dynCap) extc_dynSweep();\n",
    "    if (extc_dynFree >= 0) {\n",
    "        slot = extc_dynFree;\n",
    "        extc_dynFree = extc_dynSlots[slot].next;\n",
    "    } else {\n",
    "        if (extc_dynN >= extc_dynCap) {\n",
    "            int64_t want = extc_dynCap ? extc_dynCap * 2 : 64;\n",
    "            ExtcDynSlot *grown = (ExtcDynSlot *)realloc(extc_dynSlots, (size_t)want * sizeof *grown);\n",
    "            if (!grown) extc_dynTrap(\"?\", 0, \"out of memory (dyn slots)\");\n",
    "            for (int64_t i = extc_dynCap; i < want; i++) {\n",
    "                grown[i].zi = -1; grown[i].zgen = 0; grown[i].gen = 0; grown[i].next = -1;\n",
    "                grown[i].addr = NULL; grown[i].vt = NULL;\n",
    "            }\n",
    "            extc_dynSlots = grown; extc_dynCap = want;\n",
    "        }\n",
    "        slot = extc_dynN++;\n",
    "    }\n",
    "    s = &extc_dynSlots[slot];\n",
    "    s->zi   = zi;\n",
    "    s->zgen = extc_place_gen(zi);\n",
    "    s->addr = extc_place_alloc(zi, size);\n",
    "    s->vt   = vt;\n",
    "    if (!s->addr) extc_dynTrap(\"?\", 0, \"out of memory (dyn payload)\");\n",
    "    if (payload) memcpy(s->addr, payload, (size_t)size);\n",
    "    extc_dynLiveN++;\n",
    "    h.zi = zi; h.slot = slot; h.gen = s->gen; h.zgen = s->zgen;\n",
    "    return h;\n",
    "}\n",
    "EXTC_INLINE ExtcDynSlot *extc_dyn_slot(ExtcDynHandle h, const char *file, int line) {\n",
    "    ExtcDynSlot *s;\n",
    "    if (h.slot < 0 || h.slot >= extc_dynN) extc_dynTrap(file, line, \"stale `dyn` value\");\n",
    "    s = &extc_dynSlots[h.slot];\n",
    "    if (s->zi < 0)                      extc_dynTrap(file, line, \"stale `dyn` value (slot recycled)\");\n",
    "    if (s->zi != h.zi)                  extc_dynTrap(file, line, \"stale `dyn` value\");\n",
    "    if (s->gen != h.gen)                extc_dynTrap(file, line, \"stale `dyn` value (slot recycled)\");\n",
    "    if (!extc_place_live(h.zi))         extc_dynTrap(file, line, \"stale `dyn` value (its place is gone)\");\n",
    "    if (extc_place_gen(h.zi) != h.zgen) extc_dynTrap(file, line, \"stale `dyn` value (its place is gone)\");\n",
    "    return s;\n",
    "}\n",
};

/**
 * @brief Append the place registry to `out`.
 *
 * Emitted only when the unit has a place at all. Defines ExtcPlace, the slot table, the free list,
 * the pushed-place stack (extc_placeTop) and the payload allocator.
 *
 * @param a    unused: the runtime is pure text.
 * @param out  output buffer receiving the generated code.
 */
void placesEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    size_t n = sizeof PLACE_RT / sizeof PLACE_RT[0];
    for (size_t i = 0; i < n; i++) bufPuts(out, PLACE_RT[i]);
}

/**
 * @brief Append the dyn object table to `out`.
 *
 * Emitted only when the unit erases a type behind a trait object. Depends on the place registry:
 * a dyn payload lives in a place, and the staleness check asks that place for its generation, so
 * the two sections ship together (the caller emits the place registry first).
 *
 * @param a    unused: the runtime is pure text.
 * @param out  output buffer receiving the generated code.
 */
void placesEmitDynRuntime(Arena *a, Buf *out) {
    (void)a;
    size_t n = sizeof PLACE_DYN_RT / sizeof PLACE_DYN_RT[0];
    for (size_t i = 0; i < n; i++) bufPuts(out, PLACE_DYN_RT[i]);
}
