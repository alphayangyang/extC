/**
 * @file domain.h
 * @brief Scheduling-domain runtime: a task table driven by one domain.
 *
 * A task is a frame plus the step function that advances it -- exactly the pair the
 * coroutine machinery already produces (`<fn>$frame` / `<fn>$next`), so this layer knows
 * nothing about the inside of a frame. Emitted on demand: a program that never starts a
 * task does not pull it in.
 */
#ifndef EXTC_DOMAIN_H
#define EXTC_DOMAIN_H

#include "base.h"

/**
 * @brief Append the scheduling-domain runtime to `out`.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void domainEmitRuntime(Arena *a, Buf *out);

#endif
