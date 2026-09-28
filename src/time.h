/* The time runtime: a clock and a sleep, both in **nanoseconds as i64**.
 *
 * Why a runtime at all: `clock_gettime` takes a `struct timespec`, and this project's boundary rule
 * is that platform struct layouts never become extC's business (`stdlib/std/sys/net.extc` says the
 * same about `epoll_event`). So the layout stays here, in emitted C, and extC sees one scalar.
 *
 * Emitted on demand (`CG.needTime`, set by a declaration from `extern!("extc-time")`), so a program
 * that never asks for the time carries none of it -- the same shape as `memfind.h` / `coroutine.h`.
 */
#ifndef EXTC_TIME_H
#define EXTC_TIME_H

#include "base.h"

void timeEmitRuntime(Arena *a, Buf *out);

#endif
