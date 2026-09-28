/* prototype-heap/host.c —— 边界层（Heap）的最小原型：连续板 + 一道门。
 *
 * 只回答四个问题（每个都打印数字，不打印形容词）：
 *   1. reserve —— 一次 mmap 保留一大段连续区、按需 commit，代价与 RSS 各是多少
 *   2. gate    —— 每次分配调用加一次区间检查，相对于 malloc 本身是多少
 *   3. close   —— close 即 munmap ⇒ 旧指针再用是**硬缺页**（fork 出来验，父进程活着）
 *   4. loan    —— 大输入三种借出法：拷进板 / mprotect 只读借出 / 直接给
 *
 * 编译契约与产物一致：gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror
 */
#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PLATE_RESERVE (4ull << 30)      /* 保留 4 GiB 连续地址（虚拟，不占物理页） */
#define CHUNK         (1u << 20)        /* 每次 commit 1 MiB */
#define NCLS          10                /* 16B .. 8KiB 的 size class */
#define ALIGN         16u

typedef struct blk { size_t size; struct blk *next; } blk;

typedef struct {
    unsigned char *base;
    size_t reserve, committed, bump;
    blk   *cls[NCLS];
    size_t n_alloc, n_free, n_reject, n_grow_fail;
    size_t page;
} plate;

static size_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (size_t)ts.tv_sec * 1000000000u + (size_t)ts.tv_nsec;
}
static size_t rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long total = 0, res = 0;
    if (f) { if (fscanf(f, "%ld %ld", &total, &res) != 2) res = 0; fclose(f); }
    return (size_t)res * (size_t)(sysconf(_SC_PAGESIZE) / 1024);
}

/* ---------------- 板：保留 + 按需 commit ---------------- */
static int plate_open(plate *p) {
    memset(p, 0, sizeof *p);
    p->page = (size_t)sysconf(_SC_PAGESIZE);
    p->reserve = PLATE_RESERVE;
    void *m = mmap(NULL, p->reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return -1;
    p->base = (unsigned char *)m;
    return 0;
}
static int plate_commit(plate *p, size_t need) {
    size_t want = p->committed + ((need + CHUNK - 1) / CHUNK) * CHUNK;
    if (want > p->reserve) { p->n_grow_fail++; return -1; }
    if (mprotect(p->base + p->committed, want - p->committed, PROT_READ | PROT_WRITE) != 0) return -1;
    p->committed = want;
    return 0;
}
static void plate_close(plate *p) {
    if (p->base) munmap(p->base, p->reserve);
    p->base = NULL;
}

/* ---------------- 那道门：一次（或两次）比较 ---------------- */
static int in_plate(const plate *p, const void *q) {
    const unsigned char *b = (const unsigned char *)q;
    return b >= p->base && b < p->base + p->committed;
}
static int range_in_plate(const plate *p, const void *q, uint64_t n) {
    const unsigned char *b = (const unsigned char *)q;
    return b >= p->base && b <= p->base + p->committed && (uint64_t)(p->base + p->committed - b) >= n;
}

/* ---------------- 板内分配器（size class + bump） ---------------- */
static int cls_of(size_t n) {
    size_t s = ALIGN; int c = 0;
    while (s < n && c < NCLS - 1) { s <<= 1; c++; }
    return (s < n) ? -1 : c;
}
static void *plate_alloc(plate *p, size_t n) {
    if (n == 0) n = 1;
    int c = cls_of(n + sizeof(blk));
    if (c < 0) {                     /* 大块：只 bump，不回收（close 时整块还回去 —— 板的意义）*/
        size_t off = (p->bump + ALIGN - 1) & ~(size_t)(ALIGN - 1);
        size_t need = n + sizeof(blk);
        if (off + need > p->committed && plate_commit(p, off + need - p->committed) != 0) return NULL;
        blk *b = (blk *)(p->base + off);
        b->size = need; b->next = NULL;
        p->bump = off + need; p->n_alloc++;
        return (unsigned char *)b + sizeof(blk);
    }
    if (p->cls[c]) { blk *b = p->cls[c]; p->cls[c] = b->next; p->n_alloc++; return (unsigned char *)b + sizeof(blk); }
    size_t sz = (size_t)ALIGN << c;
    size_t off = (p->bump + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if (off + sz > p->committed && plate_commit(p, off + sz - p->committed) != 0) return NULL;
    blk *b = (blk *)(p->base + off);
    b->size = sz; b->next = NULL;
    p->bump = off + sz;
    p->n_alloc++;
    return (unsigned char *)b + sizeof(blk);
}
static int plate_free(plate *p, void *q) {
    if (!in_plate(p, q)) { p->n_reject++; return -1; }
    blk *b = (blk *)((unsigned char *)q - sizeof(blk));
    int c = cls_of(b->size);
    if (c < 0) { p->n_free++; return 0; }        /* 大块不回收：整板 close 时一起还 */
    b->next = p->cls[c]; p->cls[c] = b;
    p->n_free++;
    return 0;
}

/* ---------------- 交给模块的那张表（版本握手的第一个字段） ---------------- */
typedef struct extc_heap_api {
    uint64_t size;
    void    *ctx;                    /* 这是哪一只板：模块把它原样传回来 */
    void   *(*alloc)(void *plate, uint64_t n);
    int32_t (*release)(void *plate, void *q);
    int32_t (*resize)(void *plate, void **q, uint64_t n);
    void   *(*copyIn)(void *plate, const void *src, uint64_t n);
    int32_t (*copyOut)(void *plate, const void *q, uint64_t n, void *dst);
    int64_t (*len)(void *plate, const void *q);
} extc_heap_api;

static void *api_alloc(void *pl, uint64_t n) { return plate_alloc((plate *)pl, (size_t)n); }
static int32_t api_release(void *pl, void *q) { return (int32_t)plate_free((plate *)pl, q); }
static int32_t api_resize(void *pl, void **q, uint64_t n) {
    plate *p = (plate *)pl;
    if (!q || !*q) return -1;
    if (!in_plate(p, *q)) { p->n_reject++; return -1; }
    if (n > p->committed) { p->n_reject++; return -1; }         /* 长度也要管 */
    blk *b = (blk *)((unsigned char *)*q - sizeof(blk));
    if (n <= b->size - sizeof(blk)) return 0;                    /* 装得下就原地留着 */
    void *nw = plate_alloc(p, (size_t)n);
    if (!nw) return -1;
    memcpy(nw, *q, b->size - sizeof(blk));
    plate_free(p, *q);
    *q = nw;
    return 0;
}
static void *api_copyIn(void *pl, const void *src, uint64_t n) {
    void *d = plate_alloc((plate *)pl, (size_t)n);
    if (d) memcpy(d, src, (size_t)n);
    return d;
}
static int32_t api_copyOut(void *pl, const void *q, uint64_t n, void *dst) {
    plate *p = (plate *)pl;
    if (!range_in_plate(p, q, n)) { p->n_reject++; return -1; }  /* 起点 + 长度一起查 */
    memcpy(dst, q, (size_t)n);
    return 0;
}
static int64_t api_len(void *pl, const void *q) {
    plate *p = (plate *)pl;
    if (!in_plate(p, q)) { p->n_reject++; return -1; }
    return (int64_t)((const blk *)((const unsigned char *)q - sizeof(blk)))->size - (int64_t)sizeof(blk);
}

typedef void *(*build_fn)(const extc_heap_api *, uint64_t);
typedef int32_t (*sum_fn)(const extc_heap_api *, const uint64_t *, uint64_t);

/* ---------------- 测量 ---------------- */
static void m_reserve(void) {
    size_t r0 = rss_kb(), t0 = now_ns();
    plate p; if (plate_open(&p) != 0) { printf("reserve: mmap 失败\n"); return; }
    size_t r1 = rss_kb();
    /* 逐页首次触碰 64 MiB，量缺页 */
    size_t pages = (64u << 20) / p.page;
    if (plate_commit(&p, 64u << 20) != 0) { printf("reserve: commit 失败\n"); plate_close(&p); return; }
    volatile unsigned char sink = 0;
    size_t t1 = now_ns();
    for (size_t i = 0; i < pages; i++) { p.base[i * p.page] = (unsigned char)i; sink ^= p.base[i * p.page]; }
    size_t t2 = now_ns();
    size_t r2 = rss_kb();
    printf("reserve: 保留 %llu MiB → RSS %zu→%zu KB（保留本身不占物理页）\n",
           (unsigned long long)((p.reserve) >> 20), r0, r1);
    printf("reserve: 触碰 %zu 页（64 MiB）用 %zu ms ⇒ 每页缺页 %.0f ns；触碰后 RSS %zu KB（sink=%u）\n",
           pages, (t2 - t1) / 1000000u, (double)(t2 - t1) / (double)pages, r2, sink);
    printf("reserve: open+commit 自身 %zu us\n", (t1 - t0) / 1000u);
    plate_close(&p);
}

static void *xmalloc(size_t n) { void *q = malloc(n); return q; }

static void m_gate(void) {
    enum { N = 2000000 };
    static const size_t sizes[8] = { 16, 32, 48, 64, 96, 128, 256, 1024 };
    /* (a) 裸 malloc/free */
    size_t t0 = now_ns();
    for (int i = 0; i < N; i++) { void *q = xmalloc(sizes[i & 7]); if (((uintptr_t)q >> 4) & 1) *(volatile char *)q = 1; free(q); }
    size_t t1 = now_ns();
    /* (b) 板 + 那道门（区已预先 commit 好，不含缺页） */
    plate p; plate_open(&p); plate_commit(&p, 16u << 20);
    size_t t2 = now_ns();
    for (int i = 0; i < N; i++) {
        void *q = plate_alloc(&p, sizes[i & 7]);
        if (((uintptr_t)q >> 4) & 1) *(volatile char *)q = 1;
        plate_free(&p, q);
    }
    size_t t3 = now_ns();
    printf("gate: %d 次 malloc+free —— 裸 malloc %.1f ns/op · 板+检查 %.1f ns/op ⇒ 检查本身的代价在此之内\n",
           N, (double)(t1 - t0) / N, (double)(t3 - t2) / N);
    printf("gate: 分配 %zu 次 · 释放 %zu 次 · 拒绝 %zu 次 · commit %zu MiB · 增长失败 %zu\n",
           p.n_alloc, p.n_free, p.n_reject, p.committed >> 20, p.n_grow_fail);
    /* 门的正例/反例 */
    int host_stack = 0;
    printf("gate: free(宿主指针) → %s（拒绝 %zu）；copyOut(区内, 超出长度) → %s；copyOut(区内, 合法) → %s\n",
           api_release(&p, &host_stack) == 0 ? "接受(错!)" : "拒绝", p.n_reject,
           api_copyOut(&p, p.base + p.page, (uint64_t)p.committed, (void *)&host_stack) == 0 ? "接受(错!)" : "拒绝",
           api_copyOut(&p, p.base + p.page, 8, (void *)&host_stack) == 0 ? "接受" : "拒绝(错!)");
    plate_close(&p);
}

static void m_close_fault(void) {
    plate p; plate_open(&p); plate_commit(&p, 1u << 20);
    unsigned char *keep = p.base + 4096; keep[0] = 7;
    pid_t pid = fork();
    if (pid == 0) {                       /* 子进程：close 之后碰旧指针 */
        struct rlimit rl = { 0, 0 }; setrlimit(RLIMIT_CORE, &rl);
        plate_close(&p);
        volatile unsigned char v = keep[0];
        printf("close: 子进程读到 %u —— 没崩（不该发生）\n", v);
        _exit(0);
    }
    int st = 0; waitpid(pid, &st, 0);
    printf("close: close 即 munmap ⇒ 旧指针再用：%s\n",
           WIFSIGNALED(st) ? (WTERMSIG(st) == SIGSEGV ? "SIGSEGV（硬缺页 ✓）" : "别的信号") : "退出码正常（错!）");
    plate_close(&p);
}

static void m_loan(void) {
    size_t n = 64u << 20;
    plate p; plate_open(&p);
    unsigned char *src = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) { printf("loan: mmap 失败\n"); return; }
    memset(src, 0x5a, n);
    volatile size_t acc = 0;
    /* (a) 拷进板再读 */
    size_t t0 = now_ns(); void *in = api_copyIn(&p, src, n); size_t t1 = now_ns();
    if (!in) { printf("loan: (a) 拷进板失败（commit 超出保留）\n"); }
    for (size_t i = 0; i < n; i += 64) acc += ((unsigned char *)in)[i];
    size_t t2 = now_ns();
    printf("loan: (a) 拷进板 %.0f MB/s（%zu MiB）· 读板内 %.0f MB/s；板 commit %zu MiB\n",
           (double)n / (double)(t1 - t0) * 1000.0, n >> 20, (double)n / (double)(t2 - t1) * 1000.0, p.committed >> 20);
    /* (b) mprotect 只读借出：零拷贝 */
    size_t t3 = now_ns();
    mprotect(src, n, PROT_READ);
    size_t t3b = now_ns();
    for (size_t i = 0; i < n; i += 64) acc += src[i];
    size_t t4 = now_ns();
    mprotect(src, n, PROT_READ | PROT_WRITE);
    size_t t4b = now_ns();
    printf("loan: (b) 只读借出：读 %.0f MB/s（零拷贝）· mprotect 各 %zu us / %zu us\n",
           (double)n / (double)(t4 - t3b) * 1000.0, (t3b - t3) / 1000u, (t4b - t4) / 1000u);
    /* (b2) 写只读借出区 ⇒ 应当 SIGSEGV（fork 验） */
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 0, 0 }; setrlimit(RLIMIT_CORE, &rl);
        mprotect(src, n, PROT_READ);
        src[0] = 1;
        printf("loan: 子进程写成功了 —— 没崩（不该发生）\n"); _exit(0);
    }
    int st = 0; waitpid(pid, &st, 0);
    printf("loan: (b2) 写只读借出区 ⇒ %s\n", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV ? "SIGSEGV（内核保证 ✓）" : "没崩（错!）");
    printf("loan: (c) 直接给宿主指针（可写借出）读 %.0f MB/s（对照）· acc=%zu\n",
           (double)n / (double)(t2 - t1) * 1000.0, acc);
    munmap(src, n);
    plate_close(&p);
}

static void m_plugin(void) {
    void *h = dlopen("./plugin.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("plugin: dlopen 失败: %s\n", dlerror()); return; }
    build_fn build = (build_fn)dlsym(h, "plugin_build");
    sum_fn   sum   = (sum_fn)dlsym(h, "plugin_sum");
    const char *(*abi)(void) = (const char *(*)(void))dlsym(h, "plugin_abi");
    if (!build || !sum || !abi) { printf("plugin: dlsym 失败\n"); dlclose(h); return; }
    printf("plugin: ABI=%s\n", abi());
    plate p; plate_open(&p);
    extc_heap_api api = { .size = sizeof(extc_heap_api), .ctx = &p, .alloc = api_alloc, .release = api_release,
                          .resize = api_resize, .copyIn = api_copyIn, .copyOut = api_copyOut, .len = api_len };
    uint64_t n = 100000;
    uint64_t *arr = (uint64_t *)build(&api, n);       /* 模块在板里建数组，返回**指针** */
    if (!arr) { printf("plugin: build 返回空\n"); plate_close(&p); dlclose(h); return; }
    printf("plugin: 模块返回指针 %s（区间检查：%s）· 宿主校验后求和=%lld\n",
           in_plate(&p, arr) ? "在板内 ✓" : "在板外", in_plate(&p, arr) ? "通过" : "拒绝",
           (long long)sum(&api, arr, n));
    /* 反例：把宿主自己的指针递给模块，让 gate 拒绝 */
    uint64_t outside[4] = { 1, 2, 3, 4 };
    printf("plugin: 把宿主栈上的数组当模块返回值递进 copyOut → %s\n",
           api_copyOut(&p, outside, sizeof outside, outside) == 0 ? "接受(错!)" : "拒绝 ✓");
    plate_close(&p); dlclose(h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩了也要看得见每一行 */
    printf("== Heap 原型：连续板 + 一道门（%zu 字节页）==\n", (size_t)sysconf(_SC_PAGESIZE));
    m_reserve();
    m_gate();
    m_close_fault();
    m_loan();
    m_plugin();
    return 0;
}
