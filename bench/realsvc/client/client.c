/* client/client.c — the one load generator every realsvc server arm is measured
 * with, so the client is never part of the comparison.
 *
 *   cc -O2 -std=c11 -o build/client client/client.c
 *   ./build/client <log.tsv> <port> [requests] [connections] [depth]
 *
 * The client builds the expected per-site answers from the dataset itself, opens
 * `connections` TCP connections, keeps `depth` requests in flight per connection,
 * and verifies every reply. It prints one line:
 *
 *   client req=<n> bad=<n> ms=<ms> reqps=<n> cs=<fnv of every reply field>
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SITES 16
#define STATUSES 9

static const int kStatusTable[STATUSES] = {200, 201, 204, 301, 400, 403, 404, 500, 502};
static const uint64_t kFnvBasis = 14695981039346656037ULL;
static const uint64_t kFnvPrime = 1099511628211ULL;

typedef struct {
    long long count, sumLat, maxLat, sumBytes;
} SiteAgg;

static SiteAgg gExpected[SITES];

static uint64_t fnv(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }

static char *readFile(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("open");
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) exit(1);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *size = got;
    return buf;
}

/* The aggregate the server is expected to answer with. */
static void buildExpected(const char *buf, size_t size) {
    const char *p = buf, *end = buf + size;
    while (p < end) {
        long long v[7];
        for (int f = 0; f < 7; f++) {
            long long x = 0;
            while (p < end && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
            if (p < end) p++;
            v[f] = x;
        }
        SiteAgg *a = &gExpected[v[2]];
        a->count++;
        a->sumLat += v[5];
        if (v[5] > a->maxLat) a->maxLat = v[5];
        a->sumBytes += v[6];
    }
}

typedef struct {
    int fd;
    long long left;        /* requests still to send on this connection */
    long long inFlight;
    size_t inLen;
    char in[1 << 16];
} Conn;

static long long gRequests, gBad, gReplies;
static uint64_t gCs = kFnvBasis;

static double nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void setNonBlock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Pull every complete reply out of c->in and check it. */
static void consumeReplies(Conn *c, int site) {
    size_t at = 0;
    while (at < c->inLen) {
        char *nl = memchr(c->in + at, '\n', c->inLen - at);
        if (!nl) break;
        long long v[4] = {0, 0, 0, 0};
        int field = 0;
        for (char *q = c->in + at; q < nl; q++) {
            if (*q == ' ') {
                field++;
                if (field > 3) field = 3;
            } else if (*q >= '0' && *q <= '9') {
                v[field] = v[field] * 10 + (*q - '0');
            }
        }
        SiteAgg *e = &gExpected[site];
        gCs = fnv(gCs, (uint64_t)site);
        gCs = fnv(gCs, (uint64_t)v[0]);
        gCs = fnv(gCs, (uint64_t)v[1]);
        gCs = fnv(gCs, (uint64_t)v[2]);
        gCs = fnv(gCs, (uint64_t)v[3]);
        if (v[0] != e->count || v[1] != e->sumLat || v[2] != e->maxLat || v[3] != e->sumBytes) gBad++;
        gReplies++;
        c->inFlight--;
        at = (size_t)(nl - c->in) + 1;
    }
    if (at > 0) {
        memmove(c->in, c->in + at, c->inLen - at);
        c->inLen -= at;
    }
}

static void fillWindow(Conn *c, int site, int depth) {
    char req[32];
    int n = snprintf(req, sizeof req, "G %d\n", site);
    while (c->left > 0 && c->inFlight < depth) {
        ssize_t wrote = write(c->fd, req, (size_t)n);
        if (wrote != n) break;
        c->left--;
        c->inFlight++;
        gRequests++;
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "data/log.tsv";
    int port = argc > 2 ? atoi(argv[2]) : 9100;
    long long requests = argc > 3 ? atoll(argv[3]) : 600000;
    int conns = argc > 4 ? atoi(argv[4]) : 64;
    int depth = argc > 5 ? atoi(argv[5]) : 8;
    size_t size = 0;
    char *buf = readFile(path, &size);
    buildExpected(buf, size);
    free(buf);

    Conn *cs = calloc((size_t)conns, sizeof(Conn));
    int ep = epoll_create1(0);
    for (int i = 0; i < conns; i++) {
        cs[i].fd = socket(AF_INET, SOCK_STREAM, 0);
        setNonBlock(cs[i].fd);
        int one = 1;
        setsockopt(cs[i].fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (connect(cs[i].fd, (struct sockaddr *)&a, sizeof a) != 0 && errno != EINPROGRESS) {
            perror("connect");
            return 2;
        }
        cs[i].left = requests / conns + (i < requests % conns ? 1 : 0);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.u32 = (uint32_t)i;
        epoll_ctl(ep, EPOLL_CTL_ADD, cs[i].fd, &ev);
    }
    struct epoll_event events[256];
    long long remainingConns = conns;
    int closed = 0;
    double t0 = -1, t1 = -1;
    double startMs = nowMs();
    while (remainingConns > 0) {
        int n = epoll_wait(ep, events, 256, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* A server that never accepted (port already in use, crashed on startup)
         * would otherwise leave this loop waiting forever; fail loudly instead. */
        if (nowMs() - startMs > 30000.0) {
            fprintf(stderr, "client: timed out after %.0f ms with %lld connection(s) still open\n",
                    nowMs() - startMs, remainingConns);
            return 3;
        }
        for (int i = 0; i < n; i++) {
            uint32_t idx = events[i].data.u32;
            Conn *c = &cs[idx];
            if (c->fd < 0) continue;
            int site = (int)(idx % SITES);
            if (events[i].events & (EPOLLOUT | EPOLLIN)) {
                if (t0 < 0) t0 = nowMs();
                fillWindow(c, site, depth);
                ssize_t got = read(c->fd, c->in + c->inLen, sizeof c->in - c->inLen);
                if (got > 0) {
                    c->inLen += (size_t)got;
                    consumeReplies(c, site);
                    fillWindow(c, site, depth);
                } else if (got == 0) {
                    if (c->left == 0 && c->inFlight == 0) {
                        close(c->fd);
                        c->fd = -1;
                        remainingConns--;
                        closed++;
                        continue;
                    }
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(c->fd);
                    c->fd = -1;
                    remainingConns--;
                    continue;
                }
                if (c->left == 0 && c->inFlight == 0) {
                    shutdown(c->fd, SHUT_WR);
                }
            }
        }
        if (t0 >= 0 && gReplies >= requests) t1 = nowMs();
    }
    if (t1 < 0) t1 = nowMs();
    double ms = t1 - t0;
    printf("client req=%lld bad=%lld ms=%.1f reqps=%.0f closed=%d cs=%llu\n", gRequests, gBad, ms,
           ms > 0 ? (double)gReplies / (ms / 1000.0) : 0.0, closed, (unsigned long long)gCs);
    return gBad == 0 ? 0 : 1;
}
