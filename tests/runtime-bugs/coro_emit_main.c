/* Dump the emitted event layer (`eventEmitRuntime` in src/back/coroutine.c) to stdout.
 *
 * The callback probe is compiled against this text, so it asserts the behavior of the code
 * that generated programs really get. */
#include <stdio.h>

#include "base.h"
#include "coroutine.h"

int main(void) {
    Arena a;
    Buf out;
    arenaInit(&a, 1 << 16);
    bufInit(&out, &a);
    eventEmitRuntime(&a, &out);
    fputs(bufCstr(&out), stdout);
    return 0;
}
