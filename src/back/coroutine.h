/**
 * @file coroutine.h
 * @brief Coroutine task table: the runtime half of `coroutine<T>`.
 *
 * It is a file of its own for the same reason as the place registry (places.h): coroutines
 * touch several passes, and spreading the runtime through `codegen.c` would repeat the
 * pattern of "a little in every file, complete in none". Work that belongs to the compiler
 * stays in the compiler's files (`yield` desugaring, frames, stepping); what belongs to
 * the **runtime** lives here.
 *
 * The text is emitted on demand: a program that never spawns a coroutine carries not one
 * byte of it.
 */
#ifndef EXTC_COROUTINE_H
#define EXTC_COROUTINE_H

#include "base.h"

/**
 * @brief Append the task table to the generated translation unit.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void coroutineEmitRuntime(Arena *a, Buf *out);

/**
 * @brief Append the file-layer shim.
 *
 * `struct stat`, `off_t`, and NUL termination of paths all stay on the C side; extC only
 * sees scalars. It is emitted independently rather than riding on the coroutine table:
 * that table's final emission is gated by other conditions and cannot carry this block.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void fileLayerEmitRuntime(Arena *a, Buf *out);

/**
 * @brief Append the small DNS block (synchronous `getaddrinfo`).
 *
 * It has nothing to do with coroutines or epoll, and the runtime is emitted on demand, so
 * putting it in the coroutine block would break linking for a program that only wants to
 * resolve names. It is emitted on its own.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void dnsEmitRuntime(Arena *a, Buf *out);

/**
 * @brief Append the event layer (epoll + AF_UNIX sockets).
 *
 * Only scalars and one pointer cross the boundary: the `epoll_event` buffers stay on the C
 * side, and the platform struct never enters extC. It is emitted independently, because a
 * program may drive fds without ever spawning a coroutine.
 *
 * @param arena  Arena for temporary buffers.
 * @param out    Generated C being written.
 */
void eventEmitRuntime(Arena *arena, Buf *out);

#endif
