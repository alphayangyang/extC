/* Probe: every error path of `extc_unix_listen` / `extc_unix_connect` must close the socket
 * it created. Before the fix an invalid path length, a failed bind and a failed connect each
 * returned -1 while leaking the fd, so a caller that retried exhausted its fd table.
 *
 * The probe loops the three failure shapes and compares the number of open fds before and
 * after; then it makes sure a valid socketpair still succeeds. The event-layer text is
 * emitted by `src/back/coroutine.c`; the suite drops it here as `extc_event_runtime.inc`. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Provided by the generated TU prologue; the event text uses inet_pton. */
#include <arpa/inet.h>

#include "extc_event_runtime.inc"

/* Open fds, DIR's own fd included: the same measurement before and after. */
static int openFds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
    closedir(d);
    return n;
}

int main(void) {
    int before = openFds();
    if (before < 0) {
        printf("  FAIL cannot count fds (/proc/self/fd)\n");
        return 1;
    }

    /* bad length: both sides, both "too short" and "longer than sun_path" */
    char path[8] = "probe";
    for (int i = 0; i < 10000; i++) {
        if (extc_unix_listen((uint8_t *)path, 0, 4) != -1) { printf("  FAIL listen(len=0)\n"); return 1; }
        if (extc_unix_listen((uint8_t *)path, 200, 4) != -1) { printf("  FAIL listen(len=200)\n"); return 1; }
        if (extc_unix_connect((uint8_t *)path, 0) != -1) { printf("  FAIL connect(len=0)\n"); return 1; }
        if (extc_unix_connect((uint8_t *)path, 200) != -1) { printf("  FAIL connect(len=200)\n"); return 1; }
    }

    /* bind failure: bind the same abstract name twice. The first listener is closed again. */
    for (int i = 0; i < 200; i++) {
        char name[64];
        name[0] = 0;   /* leading NUL selects the abstract namespace */
        snprintf(name + 1, sizeof name - 1, "extc_rt_taken_%d", i);
        int64_t len = (int64_t)strlen(name + 1) + 1;
        int64_t fd = extc_unix_listen((uint8_t *)name, len, 4);
        if (fd < 0) { printf("  FAIL first abstract bind %d\n", i); return 1; }
        if (extc_unix_listen((uint8_t *)name, len, 4) != -1) {
            printf("  FAIL second bind to the same name must fail\n");
            return 1;
        }
        close((int)fd);
    }

    /* connect failure: nobody listens on this abstract name. */
    for (int i = 0; i < 200; i++) {
        char name[64];
        name[0] = 0;   /* leading NUL selects the abstract namespace */
        snprintf(name + 1, sizeof name - 1, "extc_rt_nobody_%d", i);
        int64_t len = (int64_t)strlen(name + 1) + 1;
        if (extc_unix_connect((uint8_t *)name, len) != -1) {
            printf("  FAIL connect to a name with no listener must fail\n");
            return 1;
        }
    }

    int after = openFds();
    printf("fds before=%d after=%d\n", before, after);
    if (after < 0) { printf("  FAIL fd table exhausted\n"); return 1; }
    if (after != before) {
        printf("  FAIL leaked %d fds over the failed calls\n", after - before);
        return 1;
    }

    int64_t fds[2];
    if (extc_sock_pair(fds) != 0) { printf("  FAIL socketpair unavailable after the loop\n"); return 1; }
    close((int)fds[0]);
    close((int)fds[1]);
    printf("  ok 10400 rejected calls, fd count unchanged\n");
    return 0;
}
