/* The coroutine task table (docs/topics/CONCURRENCY.md 4.4, slice C).
 *
 * What a task is: the **place** a coroutine's storage hangs off. `spawn` enters that place
 * lazily and records it here; every step makes it current; the task's end releases the whole
 * place in one go.
 *
 * Why a table and not just the frame's mark: the author's decision for slice C is that the task
 * is **owned** rather than merely scoped. Keeping the record here is what makes
 *
 *   - running to completion and an explicit drop the *same* release (`extc_task_end`),
 *   - a scheduler able to enumerate what is still live (`extc_task_live`), and
 *   - the frame able to hold only its own id (`task`), with the zone read once at the spawn and
 *     stored beside it for the step to use -- a resume never re-derives the place from the
 *     environment (rule 1).
 *
 * The frame still carries the zone itself: a step reads it from there, which is what the J1
 * check in `tools/check_concurrency_guards.py` enforces (no `extc_zoneTop` inside a `$step`).
 */
#include "coroutine.h"

void coroutineEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    bufPuts(out,
        "/* The two pool primitives this table sits on: declared here because the pool runtime is\n"
        " * emitted on demand, and a program that only spawns coroutines never pulls it in by itself\n"
        " * (`genCoroDecls` marks the pool runtime needed as well, which is what defines them). */\n"
        "int64_t extc_pool_zoneEnter(void);\n"
        "void    extc_pool_zoneLeaveTo(int64_t mark);\n"
        "/* ---- coroutine task table (docs/topics/CONCURRENCY.md 4.4) ----\n"
        " * A task is the place its coroutine's storage hangs off: entered at the spawn, made\n"
        " * current for every step, and released in one go when the task ends. The table is what\n"
        " * makes the task owned rather than scoped -- running to completion and an explicit drop\n"
        " * are the same release, and a scheduler can ask what is still live. */\n"
        "static int64_t *extc_taskZone;\n"
        "static uint8_t *extc_taskLive;\n"
        "static int64_t  extc_taskCap;\n"
        "static int64_t  extc_taskNext;\n"
        "/* Begins a task: enters its own place, records it, and hands both back -- the zone for the\n"
        " * frame and the id for the table. */\n"
        "int64_t extc_task_begin(void) {\n"
        "    int64_t zone = extc_pool_zoneEnter();\n"
        "    if (extc_taskNext == extc_taskCap) {\n"
        "        int64_t cap = extc_taskCap ? extc_taskCap * 2 : 16;\n"
        "        int64_t *z = (int64_t *)realloc(extc_taskZone, (size_t)cap * sizeof *z);\n"
        "        uint8_t *l = (uint8_t *)realloc(extc_taskLive, (size_t)cap * sizeof *l);\n"
        "        if (!z || !l) { fputs(\"extc: out of memory\\n\", stderr); exit(70); }\n"
        "        extc_taskZone = z; extc_taskLive = l;\n"
        "        for (int64_t i = extc_taskCap; i < cap; i++) extc_taskLive[i] = 0;\n"
        "        extc_taskCap = cap;\n"
        "    }\n"
        "    extc_taskZone[extc_taskNext] = zone;\n"
        "    extc_taskLive[extc_taskNext] = 1;\n"
        "    return extc_taskNext++;\n"
        "}\n"
        "/* The place a task's storage hangs off: the frame stores it, so a step reads the zone from\n"
        " * there instead of re-deriving it (rule 1). */\n"
        "int64_t extc_task_zone(int64_t id) {\n"
        "    if (id < 0 || id >= extc_taskCap) return -1;\n"
        "    return extc_taskZone[id];\n"
        "}\n"
        "/* Ends a task at any time: the same release whether it ran to completion or was dropped. */\n"
        "void extc_task_end(int64_t id) {\n"
        "    if (id < 0 || id >= extc_taskCap || !extc_taskLive[id]) return;\n"
        "    extc_taskLive[id] = 0;\n"
        "    extc_pool_zoneLeaveTo(extc_taskZone[id]);\n"
        "}\n"
        "/* How many tasks are still live: what a scheduler (or a judge) asks. */\n"
        "int64_t extc_task_live(void) {\n"
        "    int64_t n = 0;\n"
        "    for (int64_t i = 0; i < extc_taskCap; i++) if (extc_taskLive[i]) n++;\n"
        "    return n;\n"
        "}\n");
}
