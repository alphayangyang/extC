/* Probe: `extc_sock_pair` must reject a NULL `out` with -1 instead of writing through it,
 * and its success path (both fds written, 0 returned) must not change.
 *
 * The event-layer text is emitted by `src/back/coroutine.c`; the suite drops it here as
 * `extc_event_runtime.inc`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Provided by the generated TU prologue; the event text uses inet_pton. */
#include <arpa/inet.h>

#include "extc_event_runtime.inc"

int main(void) {
    int64_t fds[2] = { -1, -1 };

    int64_t bad = extc_sock_pair((int64_t *)0);
    printf("sock_pair(NULL) = %lld\n", (long long)bad);
    if (bad != -1) {
        printf("  FAIL NULL out must be reported as -1\n");
        return 1;
    }

    int64_t ok = extc_sock_pair(fds);
    if (ok != 0 || fds[0] < 0 || fds[1] < 0) {
        printf("  FAIL success path changed: rc=%lld fd0=%lld fd1=%lld\n", (long long)ok,
               (long long)fds[0], (long long)fds[1]);
        return 1;
    }
    close((int)fds[0]);
    close((int)fds[1]);
    printf("  ok NULL -> -1, out[0]/out[1] still receive the fds\n");
    return 0;
}
