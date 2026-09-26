/* The coroutine task table - the runtime half of `coroutine<T>` (docs/topics/CONCURRENCY.md 4.4).
 *
 * This subsystem lives in its own file for the same reason the pool registry does
 * (`pools.h`): a coroutine reaches into several passes, and scattering its runtime over
 * `codegen.c` would repeat the mistake this project has already paid for - "a little bit in
 * every file, complete in none". What is compiler work stays compiler work (the `yield`
 * desugaring, the frame, the step); what is *runtime* lives here.
 *
 * The text is emitted on demand: a program that never spawns a coroutine with a task place
 * carries none of it.
 */
#ifndef EXTC_COROUTINE_H
#define EXTC_COROUTINE_H

#include "base.h"

/* Append the task table to the generated translation unit. */
void coroutineEmitRuntime(Arena *a, Buf *out);

#endif
