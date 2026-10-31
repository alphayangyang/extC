/**
 * @file extctime.h
 * @brief The time runtime: one clock and one sleep, both **i64 nanoseconds**.
 *
 * `clock_gettime` needs a `struct timespec`, and the layout of that platform struct
 * never crosses into the extC side: the layout stays here (in the generated C) and
 * extC sees only a scalar.
 *
 * Emitted on demand: a program that never asks for time does not carry it.
 */
#ifndef EXTC_TIME_H
#define EXTC_TIME_H

#include "base.h"

/**
 * @brief Emit the time runtime (clocks and sleep).
 * @param a    Arena.
 * @param out  Buffer that receives the generated text.
 */
void timeEmitRuntime(Arena *a, Buf *out);

#endif
