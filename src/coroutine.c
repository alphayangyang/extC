/* The coroutine task table (docs/topics/CONCURRENCY.md 4.4, slice C).
 *
 * What a task is: the **place** a coroutine's storage hangs off. `spawn` enters that place
 * lazily and records it here; every step makes it current; the task's end releases the whole
 * place in one go.
 *
 * Why a table and not just the frame's mark: the author's decision for slice C is that the task
 * is **owned** rather than merely scoped. Keeping the record here is what makes
 *
 *   - running to completion and an explicit drop the *same* release (`extc_task_end`),
 *   - a scheduler able to enumerate what is still live (`extc_task_live`), and
 *   - the frame able to hold only its own id (`task`), with the zone read once at the spawn and
 *     stored beside it for the step to use -- a resume never re-derives the place from the
 *     environment (rule 1).
 *
 * The frame still carries the zone itself: a step reads it from there, which is what the J1
 * check in `tools/check_concurrency_guards.py` enforces (no `extc_zoneTop` inside a `$step`).
 */
#include "coroutine.h"

void coroutineEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    bufPuts(out,
        "/* The two pool primitives this table sits on: declared here because the pool runtime is\n"
        " * emitted on demand, and a program that only spawns coroutines never pulls it in by itself\n"
        " * (`genCoroDecls` marks the pool runtime needed as well, which is what defines them). */\n"
        "int64_t extc_pool_zoneEnter(void);\n"
        "void    extc_pool_zoneLeaveTo(int64_t mark);\n"
        "/* ---- coroutine task table (docs/topics/CONCURRENCY.md 4.4) ----\n"
        " * A task is the place its coroutine's storage hangs off: entered at the spawn, made\n"
        " * current for every step, and released in one go when the task ends. The table is what\n"
        " * makes the task owned rather than scoped -- running to completion and an explicit drop\n"
        " * are the same release, and a scheduler can ask what is still live. */\n"
        "static int64_t *extc_taskZone;\n"
        "static uint8_t *extc_taskLive;\n"
        "static int64_t  extc_taskCap;\n"
        "static int64_t  extc_taskNext;\n"
        "/* The task's **own arena**: what a task allocates lives here and goes in one release, the\n"
        " * same shape as a block arena or a pool plate. A boxed coroutine's frame is the first\n"
        " * tenant -- it must outlive the C scope that spawned it. */\n"
        "static extc_arena *extc_taskArena;\n"
        "/* Begins a task: enters its own place, records it, and hands both back -- the zone for the\n"
        " * frame and the id for the table. */\n"
        "int64_t extc_task_begin(void) {\n"
        "    int64_t zone = extc_pool_zoneEnter();\n"
        "    if (extc_taskNext == extc_taskCap) {\n"
        "        int64_t cap = extc_taskCap ? extc_taskCap * 2 : 16;\n"
        "        int64_t *z = (int64_t *)realloc(extc_taskZone, (size_t)cap * sizeof *z);\n"
        "        uint8_t *l = (uint8_t *)realloc(extc_taskLive, (size_t)cap * sizeof *l);\n"
        "        extc_arena *a = (extc_arena *)realloc(extc_taskArena, (size_t)cap * sizeof *a);\n"
        "        if (!z || !l || !a) { fputs(\"extc: out of memory\\n\", stderr); exit(70); }\n"
        "        extc_taskZone = z; extc_taskLive = l; extc_taskArena = a;\n"
        "        for (int64_t i = extc_taskCap; i < cap; i++) {\n"
        "            extc_taskLive[i] = 0;\n"
        "            extc_taskArena[i].top = NULL; extc_taskArena[i].spare = NULL;\n"
        "        }\n"
        "        extc_taskCap = cap;\n"
        "    }\n"
        "    extc_taskZone[extc_taskNext] = zone;\n"
        "    extc_taskLive[extc_taskNext] = 1;\n"
        "    extc_taskArena[extc_taskNext].top = NULL;\n"
        "    extc_taskArena[extc_taskNext].spare = NULL;\n"
        "    return extc_taskNext++;\n"
        "}\n"
        "/* A task's own place as an allocator: the frame of a boxed coroutine lives here, and so does\n"
        " * anything the coroutine allocates with `new` (slice C's promotion). */\n"
        "void *extc_task_alloc(int64_t id, int64_t n, const char *path, int line) {\n"
        "    if (id < 0 || id >= extc_taskCap || !extc_taskLive[id]) {\n"
        "        fputs(\"extc: allocation in a dead task\\n\", stderr); exit(70);\n"
        "    }\n"
        "    return extc_arena_alloc(&extc_taskArena[id], n, path, line);\n"
        "}\n"
        "/* The place a task's storage hangs off: the frame stores it, so a step reads the zone from\n"
        " * there instead of re-deriving it (rule 1). */\n"
        "int64_t extc_task_zone(int64_t id) {\n"
        "    if (id < 0 || id >= extc_taskCap) return -1;\n"
        "    return extc_taskZone[id];\n"
        "}\n"
        "/* Ends a task at any time: the same release whether it ran to completion or was dropped. */\n"
        "void extc_task_end(int64_t id) {\n"
        "    if (id < 0 || id >= extc_taskCap || !extc_taskLive[id]) return;\n"
        "    extc_taskLive[id] = 0;\n"
        "    extc_arena_release(&extc_taskArena[id]);   /* the task's own arena, in one go */\n"
        "    extc_pool_zoneLeaveTo(extc_taskZone[id]);\n"
        "}\n"
        "/* Is this one task still alive? A handle outlives its task only by mistake: driving one\n"
        " * must trap loudly, not touch the freed place the frame lived in. */\n"
        "int64_t extc_task_alive(int64_t id) {\n"
        "    return (id >= 0 && id < extc_taskCap && extc_taskLive[id]) ? 1 : 0;\n"
        "}\n"
        "/* How many tasks are still live: what a scheduler (or a judge) asks. */\n"
        "int64_t extc_task_live(void) {\n"
        "    int64_t n = 0;\n"
        "    for (int64_t i = 0; i < extc_taskCap; i++) if (extc_taskLive[i]) n++;\n"
        "    return n;\n"
        "}\n");



}

/* ---- The event layer: epoll + AF_UNIX sockets (the scheduler's real event source) ----
 * Only scalars and one pointer cross the boundary: the `epoll_event` buffer lives in here, so a
 * platform struct never crosses into extC. Emitted on its own (not with the task table) because a
 * program may drive fds without ever spawning a coroutine. */
void eventEmitRuntime(Arena *arena, Buf *out) {
    (void)arena;
    bufPuts(out,
        "\n/* ---- The event layer: epoll + AF_UNIX sockets ----\n"
        " * Only scalars and one pointer cross the boundary: the `epoll_event` buffer lives in here. */\n"
        "#include <sys/epoll.h>\n"
        "#include <sys/socket.h>\n"
        "#include <sys/un.h>\n"
        "#include <fcntl.h>\n\n"
        "int64_t extc_epoll_new(void) { return (int64_t)epoll_create1(0); }\n\n"
        "int64_t extc_epoll_add(int64_t ep, int64_t fd, int64_t readable) {\n"
        "    struct epoll_event ev;\n"
        "    memset(&ev, 0, sizeof ev);\n"
        "    ev.events = readable ? EPOLLIN : EPOLLOUT;\n"
        "    ev.data.fd = (int)fd;\n"
        "    return (int64_t)epoll_ctl((int)ep, EPOLL_CTL_ADD, (int)fd, &ev);\n"
        "}\n\n"
        "/* One ready fd per call, out of a small queue kept in here. The queue is what makes the\n"
        " * event loop allocation-free: a round that has events left over costs no syscall and no\n"
        " * buffer, and extC never sees an `epoll_event`. Returns -1 on timeout. */\n"
        "int64_t extc_epoll_wait(int64_t ep, int64_t timeout_ms) {\n"
        "    static int64_t pending[16];\n"
        "    static int npend = 0;\n"
        "    if (npend > 0) return pending[--npend];\n"
        "    struct epoll_event evs[16];\n"
        "    int n = epoll_wait((int)ep, evs, 16, (int)timeout_ms);\n"
        "    if (n <= 0) return -1;\n"
        "    for (int i = 1; i < n; i++) pending[npend++] = (int64_t)evs[i].data.fd;\n"
        "    return (int64_t)evs[0].data.fd;\n"
        "}\n\n"
        "int64_t extc_sock_pair(int64_t *out) {\n"
        "    int sv[2];\n"
        "    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;\n"
        "    out[0] = (int64_t)sv[0];\n"
        "    out[1] = (int64_t)sv[1];\n"
        "    return 0;\n"
        "}\n\n"
        "int64_t extc_sock_read(int64_t fd, uint8_t *buf, int64_t n) {\n"
        "    /* recv/send, not read/write: `std::sys::io` already claims those two names with a\n"
        "     * different signature, and both would land in the same translation unit. */\n"
        "    return (int64_t)recv((int)fd, buf, (size_t)n, 0);\n"
        "}\n\n"
        "int64_t extc_sock_write(int64_t fd, uint8_t *buf, int64_t n) {\n"
        "    return (int64_t)send((int)fd, buf, (size_t)n, 0);\n"
        "}\n\n"
        "int64_t extc_sock_nonblock(int64_t fd) {\n"
        "    int fl = fcntl((int)fd, F_GETFL, 0);\n"
        "    return fl < 0 ? -1 : fcntl((int)fd, F_SETFL, fl | O_NONBLOCK);\n"
        "}\n\n"
        "/* An AF_UNIX listener. A path whose first byte is NUL is the abstract namespace: no file on\n"
        " * disk, no cleanup, and it disappears with the process -- which is what a test wants. */\n"
        "int64_t extc_unix_listen(uint8_t *path, int64_t len, int64_t backlog) {\n"
        "    struct sockaddr_un sa;\n"
        "    int fd = socket(AF_UNIX, SOCK_STREAM, 0);\n"
        "    if (fd < 0 || len <= 0 || len > (int64_t)sizeof sa.sun_path) return -1;\n"
        "    memset(&sa, 0, sizeof sa);\n"
        "    sa.sun_family = AF_UNIX;\n"
        "    memcpy(sa.sun_path, path, (size_t)len);\n"
        "    if (bind(fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) return -1;\n"
        "    if (listen(fd, (int)backlog) != 0) return -1;\n"
        "    extc_sock_nonblock(fd);          /* the loop accepts when epoll says readable */\n"
        "    return (int64_t)fd;\n"
        "}\n\n"
        "int64_t extc_unix_connect(uint8_t *path, int64_t len) {\n"
        "    struct sockaddr_un sa;\n"
        "    int fd = socket(AF_UNIX, SOCK_STREAM, 0);\n"
        "    if (fd < 0 || len <= 0 || len > (int64_t)sizeof sa.sun_path) return -1;\n"
        "    memset(&sa, 0, sizeof sa);\n"
        "    sa.sun_family = AF_UNIX;\n"
        "    memcpy(sa.sun_path, path, (size_t)len);\n"
        "    if (connect(fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) return -1;\n"
        "    return (int64_t)fd;\n"
        "}\n\n"
        "/* Non-blocking accept: -1 with EAGAIN when the backlog is empty. */\n"
        "int64_t extc_sock_accept(int64_t listener) {\n"
        "    int fd = accept((int)listener, NULL, NULL);\n"
        "    if (fd >= 0) extc_sock_nonblock(fd);\n"
        "    return (int64_t)fd;\n"
        "}\n\n"
        "/* No close(): `close` is `std::sys::io`'s, and the fds die with the process. */\n");
}
