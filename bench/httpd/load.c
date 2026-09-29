/* bench/httpd/load.c —— 忙连接压测器（C，无依赖）。
 *
 * 为什么不用 wrk：① 它**不能绑源地址** ⇒ 单源只有 ~28k 个临时端口，加不上去 ✗（实测 wrk -c 10000
 * 直接 0 req/s）；② 连接是一条条慢慢加的。这里两个都解决：
 *   · **每个连接绑一个源地址**（127.0.0.x 轮换，loopback 整个 127/8 都是本机 ✓）⇒ 上限 ≈ 地址数 × 28k
 *   · 非阻塞 `connect` + epoll ⇒ **并行快加**（十万条几秒 ✓）
 *
 * 用法：load <host> <port> <path> <conns> <threads> <seconds> [pipeline]
 * 输出：一条汇总行（连接数 / req/s / MB/s / p50 / p99 / 错误数）。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFCAP 2048          /* 每条连接 2 KB（小对象够用 ✓；十万条 = 200 MB） */
#define MAXEV 1024

typedef struct {
    int fd;
    uint8_t buf[BUFCAP];
    int len;                 /* 已收到的字节 */
    int hdrEnd;              /* -1 = 还没找到头结束 */
    long clen;               /* Content-Length */
    int id;                  /* 在本线程里的序号（用来挑源地址 ✓）*/
    int state;               /* 0 = 连接中，1 = 等响应，2 = 用完，3 = 出错 */
    int inflight;            /* 已发出、还没收回的响应数 */
    long long sent_us;       /* 最后一个请求发出的时刻（算延迟 ✓）*/
} conn;

typedef struct {
    int idx, conns, pipeline, seconds, naddr;
    struct sockaddr_in dst;
    long long done, bytes, errors, connfail;
    long long hist[64];      /* log2 微秒桶 */
    int ok;                  /* 建连成功数 */
    long long ramp_us;       /* 全部握手完成用了多久 ✓ */
} targ;

static long long now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void bump(targ *t, long long us) {
    int b = 0; while (us > 1 && b < 63) { us >>= 1; b++; }
    t->hist[b]++;
}

/* 一条连接的请求（HTTP/1.1 + keep-alive；服务器回的 Content-Length 决定"一个响应"的边界 ✓） */
static const char *REQ = "GET %s HTTP/1.1\r\nHost: x\r\n\r\n";
static char reqbuf[256];
static int reqlen;

static int start_connect(targ *t, conn *c, int ep) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in src;
    memset(&src, 0, sizeof src);
    src.sin_family = AF_INET;
    /* 源地址：按连接序号轮换（loopback 整个 127.0.0.0/8 都是本机 ✓）⇒ 每条地址约 28k 个端口 ✓ */
    char ip[32];
    int which = 2 + (c->id + t->idx * 31) % t->naddr;
    snprintf(ip, sizeof ip, "127.0.0.%d", which);
    inet_pton(AF_INET, ip, &src.sin_addr);
    if (bind(fd, (struct sockaddr *)&src, sizeof src) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&t->dst, sizeof t->dst) != 0 && errno != EINPROGRESS) {
        close(fd); return -1;
    }
    struct epoll_event ev; memset(&ev, 0, sizeof ev);
    ev.events = EPOLLOUT; ev.data.ptr = c;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) { close(fd); return -1; }
    c->fd = fd; c->state = 0; c->len = 0; c->hdrEnd = -1; c->clen = -1; c->inflight = 0;
    return 0;
}

/* 把 in-flight 补到 pipeline 个；EAGAIN 就盯着可写（**只在欠请求时** —— 一直盯着 EPOLLOUT
 * 会让每个已连接 socket 每次 epoll_wait 都报一遍 ✗ 客户端自己淹死 ✓ 实测踩过）。 */
static void fill(targ *t, conn *c, int ep) {
    while (c->inflight < t->pipeline) {
        ssize_t w = send(c->fd, reqbuf, reqlen, MSG_NOSIGNAL);
        if (w > 0) { c->inflight++; c->sent_us = now_us(); continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        t->errors++; c->state = 3; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); return;
    }
    struct epoll_event e2; memset(&e2, 0, sizeof e2);
    e2.events = (c->inflight < t->pipeline) ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
    e2.data.ptr = c;
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e2);
}

static void *worker(void *arg) {
    targ *t = arg;
    int per = t->conns / 1;          /* 这个线程负责全部（线程数由进程数决定，见 run.sh）*/
    (void)per;
    conn *cs = calloc(t->conns, sizeof(conn));
    if (!cs) { t->errors++; return NULL; }
    int ep = epoll_create1(0);
    struct epoll_event evs[MAXEV];
    for (int i = 0; i < t->conns; i++) {
        cs[i].id = i;
        if (start_connect(t, &cs[i], ep) != 0) { t->errors++; t->connfail++; }
        else { for (int k = 0; k < t->pipeline; k++) { if (send(cs[i].fd, reqbuf, reqlen, MSG_NOSIGNAL) > 0) cs[i].inflight++; } }
    }
    /* 计时窗口**在握手全部完成之后**才开（大 N 时建连比窗口还久 ⇒ 否则量出来是 0 ✗ 实测踩过）；
     * 建连最多等 60s，等不到就如实报 `ok` 与 `ramp` ✗。 */
    long long t0 = now_us(), ramp_limit = t0 + 60000000LL, deadline = 0;
    while (1) {
        long long now = now_us();
        if (!deadline) {
            if (t->ok >= t->conns) { t->ramp_us = now - t0; deadline = now + (long long)t->seconds * 1000000; }
            else if (now >= ramp_limit) { t->ramp_us = now - t0; break; }
        } else if (now >= deadline) break;
        int n = epoll_wait(ep, evs, MAXEV, 200);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            conn *c = evs[i].data.ptr;
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) { t->errors++; c->state = 3; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); continue; }
            if (c->state == 0) {                     /* 连上了 ⇒ 开始打 */
                int err = 0; socklen_t el = sizeof err;
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err != 0) { t->errors++; c->state = 3; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); continue; }
                t->ok++;
                if (t->ok == t->conns) t->ramp_us = now_us() - t0;
                c->state = 1;
                fill(t, c, ep);        /* 连上之后才发第一批（连接期间的 send 是 EAGAIN ✗）*/
                continue;
            }
            if (c->inflight < t->pipeline) { fill(t, c, ep); if (c->state == 3) continue; }
            for (;;) {                               /* 一次把能读的都读了 */
                ssize_t r = recv(c->fd, c->buf + c->len, BUFCAP - c->len, 0);
                if (r > 0) {
                    c->len += (int)r;
                    if (c->hdrEnd < 0) {
                        for (int k = 3; k < c->len; k++)
                            if (c->buf[k] == '\n' && c->buf[k-1] == '\r' && c->buf[k-2] == '\n' && c->buf[k-3] == '\r') { c->hdrEnd = k + 1; break; }
                        if (c->hdrEnd > 0) {
                            c->clen = 0;
                            for (int k = 0; k + 15 < c->hdrEnd; k++)
                                if (!strncasecmp((char *)c->buf + k, "content-length:", 15)) c->clen = atol((char *)c->buf + k + 15);
                        }
                    }
                    if (c->hdrEnd > 0 && c->len >= c->hdrEnd + c->clen) {
                        t->done++; t->bytes += c->hdrEnd + c->clen;
                        if (c->sent_us) bump(t, now_us() - c->sent_us);
                        c->inflight--;
                        if (c->inflight < t->pipeline) fill(t, c, ep);   /* 立刻补下一个 ✓ */
                        c->len = 0; c->hdrEnd = -1;
                        if (send(c->fd, reqbuf, reqlen, MSG_NOSIGNAL) > 0) c->inflight++;
                        else { t->errors++; c->state = 3; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); }
                    }
                    continue;
                }
                if (r == 0) { t->errors++; c->state = 3; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); }
                break;
            }
        }
    }
    for (int i = 0; i < t->conns; i++) if (cs[i].fd >= 0) close(cs[i].fd);
    free(cs);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s host port path conns threads seconds [pipeline]\n", argv[0]); return 2; }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int conns = atoi(argv[4]), threads = atoi(argv[5]), seconds = atoi(argv[6]);
    int pipeline = argc > 7 ? atoi(argv[7]) : 1;
    if (pipeline < 1) pipeline = 1;
    snprintf(reqbuf, sizeof reqbuf, REQ, argv[3]);
    reqlen = (int)strlen(reqbuf);

    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET; dst.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &dst.sin_addr);

    targ ts[threads];
    pthread_t th[threads];
    int per = conns / threads, rem = conns % threads;
    long long c0 = now_us();
    for (int i = 0; i < threads; i++) {
        memset(&ts[i], 0, sizeof ts[i]);
        ts[i].idx = i; ts[i].conns = per + (i < rem ? 1 : 0);
        ts[i].pipeline = pipeline; ts[i].seconds = seconds; ts[i].naddr = 250; ts[i].dst = dst;
        pthread_create(&th[i], NULL, worker, &ts[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    long long dt = now_us() - c0;

    long long done = 0, bytes = 0, errors = 0, ok = 0, tot = 0, hist[64] = {0};
    for (int i = 0; i < threads; i++) {
        done += ts[i].done; bytes += ts[i].bytes; errors += ts[i].errors; ok += ts[i].ok;
        for (int b = 0; b < 64; b++) hist[b] += ts[i].hist[b];
    }
    long long p50 = 0, p99 = 0, acc = 0;
    for (int b = 0; b < 64; b++) { tot += hist[b]; }
    for (int b = 0; b < 64 && tot; b++) { acc += hist[b]; if (!p50 && acc >= tot / 2) p50 = 1LL << b; if (acc >= tot * 99 / 100) { p99 = 1LL << b; break; } }
    long long connfail = 0, ramp_us = 0;
    for (int i = 0; i < threads; i++) { connfail += ts[i].connfail; if (ts[i].ramp_us > ramp_us) ramp_us = ts[i].ramp_us; }
    printf("conns=%-7d ok=%-7lld connfail=%-6lld req/s=%-9.0f MB/s=%-8.1f p50=%-7lldus p99=%-8lldus errors=%-6lld ramp=%.1fs\n",
           conns, ok, connfail, done * 1e6 / dt, bytes * 1e6 / dt / 1024 / 1024, p50, p99, errors,
           ramp_us / 1e6);
    return 0;
}
