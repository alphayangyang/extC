/* The domain runtime (docs/topics/CONCURRENCY.md「`ext` 与调度域」).
 *
 * A **domain** is the thing a task is handed to: it owns the tasks started inside its block and it
 * is what makes the block *structured* -- the block cannot end while a task it started is still
 * unfinished, because ending the block runs them (`extc_dom_run`).
 *
 * A task is `(frame, step)`: the frame is the coroutine's own storage and `step` is the generated
 * `$next` that advances it one suspension at a time. This layer therefore never looks inside a
 * frame; it only knows how to hold a pair and how to drive it to completion.
 *
 * Emitted on demand (`CG.needDomain`): a program that starts no task carries none of this.
 *
 * The domain is a pointer, and the extC side sees it as an `i64` -- the same convention the rest of
 * the runtime uses to hand opaque handles across the boundary (`extc_par_run` gets its worker that
 * way too).
 */
#include "domain.h"

void domainEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    bufPuts(out,
        "/* ---- domain: the tasks started in one block, driven to completion (CONCURRENCY.md) ----\n"
        " * A task is a frame plus the step function that advances it; the domain owns the list, and\n"
        " * `run` steps every task until it is done. That is what makes the block structured: it\n"
        " * cannot end while a task it started is still unfinished. */\n"
        "typedef struct { void *frame; bool (*step)(void *); } extc_dom_task;\n"
        "typedef struct { extc_dom_task *tasks; int64_t len, cap; } extc_dom;\n");
    bufPuts(out,
        "void *extc_dom_new(void) {\n"
        "    extc_dom *d = (extc_dom *)calloc(1, sizeof(extc_dom));\n"
        "    if (!d) { fputs(\"extC: out of memory creating a domain\\n\", stderr); exit(70); }\n"
        "    return (void *)d;\n"
        "}\n");
    bufPuts(out,
        "void extc_dom_add(void *p, void *frame, bool (*step)(void *)) {\n"
        "    extc_dom *d = (extc_dom *)p;\n"
        "    if (d->len == d->cap) {\n"
        "        int64_t cap = d->cap ? d->cap * 2 : 8;\n"
        "        extc_dom_task *t = (extc_dom_task *)realloc(d->tasks, (size_t)cap * sizeof *t);\n"
        "        if (!t) { fputs(\"extC: out of memory growing a domain\\n\", stderr); exit(70); }\n"
        "        d->tasks = t; d->cap = cap;\n"
        "    }\n"
        "    d->tasks[d->len].frame = frame;\n"
        "    d->tasks[d->len].step  = step;\n"
        "    d->len++;\n"
        "}\n");
    bufPuts(out,
        "/* Drive every task to completion. A task that never suspends is simply called once. */\n"
        "void extc_dom_run(void *p) {\n"
        "    extc_dom *d = (extc_dom *)p;\n"
        "    for (int64_t i = 0; i < d->len; i++)\n"
        "        while (d->tasks[i].step(d->tasks[i].frame)) { }\n"
        "}\n");
    bufPuts(out,
        "void extc_dom_free(void *p) {\n"
        "    extc_dom *d = (extc_dom *)p;\n"
        "    free(d->tasks);\n"
        "    free(d);\n"
        "}\n");
}
