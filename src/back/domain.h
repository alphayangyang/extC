/* The domain runtime: the task table a domain drives (docs/topics/CONCURRENCY.md「`ext` 与调度域」).
 *
 * A task is a frame plus the step function that advances it -- exactly the pair the coroutine
 * machinery already produces (`<fn>$frame` / `<fn>$next`), so this layer knows nothing about
 * frames. Emitted on demand: a program that never starts a task does not pull it in. */
#ifndef EXTC_DOMAIN_H
#define EXTC_DOMAIN_H

#include "base.h"

void domainEmitRuntime(Arena *a, Buf *out);

#endif
