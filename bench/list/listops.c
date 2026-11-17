/* bench/list/listops.c — 链表写法的三种代价。
 *
 *   e1  扫一遍按序号摘链：朴素 prev 跟踪  vs  两星指针（node **）
 *   e2  给定节点 O(1) 摘链：指针 hlist / 双向 list_head / 索引 hlist（i32 next + i32 pprev）
 *   e3  遍历一遍：同样三种布局
 *
 * 每个实验跑 R 轮取最小值。结构类结果（rem / chk / sum）每轮必须相同，run.sh 用它判红：
 * 摘链逻辑写错会立刻体现为 rem 或 chk 不一致，而不是"跑得快"。
 * 输出是制表符分隔的行：shape <TAB> arm <TAB> ns <TAB> verdict
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static volatile int64_t sink;          /* 防止整段被优化掉 */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---------------- e1：扫一遍，按 idx % 4 == 0 摘链 ---------------- */

typedef struct E1 { struct E1 *next; int64_t k; } E1;

static void e1(const char *label, int64_t n, int reps)
{
    E1 *pool = malloc(sizeof(E1) * (size_t)n);
    double b_prev = 1e30, b_two = 1e30;
    int64_t rem = 0, chk = 0;

    for (int r = 0; r < reps; r++) {
        for (int64_t i = 0; i < n; i++) {
            pool[i].k = i;
            pool[i].next = (i + 1 < n) ? &pool[i + 1] : (E1 *)0;
        }
        E1 *head = &pool[0];
        double t0 = now_ns();
        E1 *prev = (E1 *)0, *cur = head;
        int64_t i = 0;
        while (cur) {
            E1 *nx = cur->next;
            if ((i & 3) == 0) {
                if (prev) prev->next = nx; else head = nx;
            } else {
                prev = cur;
            }
            cur = nx;
            i++;
        }
        double dt = now_ns() - t0;
        if (dt < b_prev) b_prev = dt;

        rem = 0; chk = 0;
        for (E1 *p = head; p; p = p->next) { rem++; chk += p->k; }
        sink += chk;

        for (int64_t j = 0; j < n; j++) {
            pool[j].k = j;
            pool[j].next = (j + 1 < n) ? &pool[j + 1] : (E1 *)0;
        }
        head = &pool[0];
        t0 = now_ns();
        E1 **pp = &head;
        i = 0;
        while (*pp) {
            E1 *c = *pp;
            if ((i & 3) == 0) *pp = c->next; else pp = &c->next;
            i++;
        }
        dt = now_ns() - t0;
        if (dt < b_two) b_two = dt;

        int64_t rem2 = 0, chk2 = 0;
        for (E1 *p = head; p; p = p->next) { rem2++; chk2 += p->k; }
        sink += chk2;
        if (rem2 != rem || chk2 != chk) {
            printf("%s\tBAD\t0\tn=%lld prev=rem%lld,chk%lld two=rem%lld,chk%lld\n",
                   label, (long long)n, (long long)rem, (long long)chk,
                   (long long)rem2, (long long)chk2);
            free(pool);
            return;
        }
    }
    printf("%s\tprev\t%.1f\tn=%lld rem=%lld chk=%lld\n", label, b_prev, (long long)n, (long long)rem, (long long)chk);
    printf("%s\ttwo-star\t%.1f\tn=%lld rem=%lld chk=%lld\n", label, b_two, (long long)n, (long long)rem, (long long)chk);
    free(pool);
}

/* ---------------- e2 / e3：三种布局 ---------------- */

typedef struct H { struct H *next; struct H **pprev; int64_t k; } H;   /* 24 B */
typedef struct L { struct L *next; struct L *prev; int64_t k; } L;    /* 24 B */

static void e2(int64_t n, int reps)
{
    int32_t *order = malloc((size_t)n * sizeof(int32_t));
    for (int64_t i = 0; i < n; i++) order[i] = (int32_t)((uint32_t)i * 2654435761u & (uint32_t)(n - 1));
    if (n < (1 << 20)) {                       /* 只有 2 的幂时乘奇数是双射；小规模下退化为顺序 */
        for (int64_t i = 0; i < n; i++) order[i] = (int32_t)i;
    }

    H *hn = malloc((size_t)n * sizeof(H));
    L *ln = calloc((size_t)n, sizeof(L));
    int32_t *nx = malloc((size_t)(n + 1) * sizeof(int32_t)); /* 索引 hlist：next 列 + 一个头哨兵（下标 n） */
    int32_t *pp = malloc((size_t)n * sizeof(int32_t));
    double b_h = 1e30, b_l = 1e30, b_i = 1e30;

    for (int r = 0; r < reps; r++) {
        /* 指针 hlist */
        H *hh = (H *)0;
        for (int64_t i = 0; i < n; i++) {
            hn[i].k = i;
            hn[i].next = (i + 1 < n) ? &hn[i + 1] : (H *)0;
            hn[i].pprev = (i == 0) ? &hh : &hn[i - 1].next;
        }
        double t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            H *x = &hn[order[i]];
            *x->pprev = x->next;
            if (x->next) x->next->pprev = x->pprev;
        }
        double dt = now_ns() - t0;
        if (dt < b_h) b_h = dt;
        sink += hh ? 1 : 0;

        /* 双向 list_head（环形） */
        L *lh = &ln[0];
        lh->next = lh; lh->prev = lh;
        for (int64_t i = 1; i < n; i++) {
            ln[i].k = i;
            ln[i].next = lh;
            ln[i].prev = lh->prev;
            lh->prev->next = &ln[i];
            lh->prev = &ln[i];
        }
        t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            L *x = &ln[order[i]];
            x->prev->next = x->next;
            x->next->prev = x->prev;
        }
        dt = now_ns() - t0;
        if (dt < b_l) b_l = dt;
        sink += lh->next->k;

        /* 索引 hlist：i32 next + i32 "prev 的 next 槽" 的下标（n 表示头） */
        int64_t head = n;
        nx[head] = (int32_t)0;
        for (int64_t i = 0; i < n; i++) {
            nx[i] = (i + 1 < n) ? (int32_t)(i + 1) : (int32_t)-1;
            pp[i] = (i == 0) ? (int32_t)n : (int32_t)(i - 1);
        }
        t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            int32_t x = order[i];
            int32_t p = pp[x];
            nx[p] = nx[x];
            if (nx[x] >= 0) pp[nx[x]] = p;
        }
        dt = now_ns() - t0;
        if (dt < b_i) b_i = dt;
        sink += nx[head];
    }

    printf("e2-perm\thlist\t%.2f\tn=%lld\n", b_h / (double)n, (long long)n);
    printf("e2-perm\tlist_head\t%.2f\tn=%lld\n", b_l / (double)n, (long long)n);
    printf("e2-perm\tindex-hlist\t%.2f\tn=%lld\n", b_i / (double)n, (long long)n);

    /* 顺序摘链：把 order 换成 0,1,2… 再跑一遍 */
    for (int64_t i = 0; i < n; i++) order[i] = (int32_t)i;
    b_h = b_l = b_i = 1e30;
    for (int r = 0; r < reps; r++) {
        H *hh = (H *)0;
        for (int64_t i = 0; i < n; i++) {
            hn[i].k = i;
            hn[i].next = (i + 1 < n) ? &hn[i + 1] : (H *)0;
            hn[i].pprev = (i == 0) ? &hh : &hn[i - 1].next;
        }
        double t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            H *x = &hn[i];
            *x->pprev = x->next;
            if (x->next) x->next->pprev = x->pprev;
        }
        double dt = now_ns() - t0;
        if (dt < b_h) b_h = dt;
        sink += hh ? 1 : 0;

        L *lh = &ln[0];
        lh->next = lh; lh->prev = lh;
        for (int64_t i = 1; i < n; i++) {
            ln[i].k = i;
            ln[i].next = lh;
            ln[i].prev = lh->prev;
            lh->prev->next = &ln[i];
            lh->prev = &ln[i];
        }
        t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            L *x = &ln[i];
            x->prev->next = x->next;
            x->next->prev = x->prev;
        }
        dt = now_ns() - t0;
        if (dt < b_l) b_l = dt;
        sink += lh->next->k;

        int64_t head = n;
        nx[head] = (int32_t)0;
        for (int64_t i = 0; i < n; i++) {
            nx[i] = (i + 1 < n) ? (int32_t)(i + 1) : (int32_t)-1;
            pp[i] = (i == 0) ? (int32_t)n : (int32_t)(i - 1);
        }
        t0 = now_ns();
        for (int64_t i = 0; i < n; i++) {
            int32_t x = (int32_t)i;
            int32_t p = pp[x];
            nx[p] = nx[x];
            if (nx[x] >= 0) pp[nx[x]] = p;
        }
        dt = now_ns() - t0;
        if (dt < b_i) b_i = dt;
        sink += nx[head];
    }
    printf("e2-seq\thlist\t%.2f\tn=%lld\n", b_h / (double)n, (long long)n);
    printf("e2-seq\tlist_head\t%.2f\tn=%lld\n", b_l / (double)n, (long long)n);
    printf("e2-seq\tindex-hlist\t%.2f\tn=%lld\n", b_i / (double)n, (long long)n);

    free(order); free(hn); free(ln); free(nx); free(pp);
}

static void e3(int64_t n, int reps)
{
    H *hn = malloc((size_t)n * sizeof(H));
    L *ln = malloc((size_t)n * sizeof(L));
    int32_t *nx = malloc((size_t)n * sizeof(int32_t));
    int32_t *pp = malloc((size_t)n * sizeof(int32_t));
    int64_t *keys = malloc((size_t)n * sizeof(int64_t));
    double b_h = 1e30, b_l = 1e30, b_i = 1e30;
    int64_t sum_h = 0, sum_l = 0, sum_i = 0;

    for (int64_t i = 0; i < n; i++) {
        keys[i] = i;
        hn[i].k = i;
        hn[i].next = (i + 1 < n) ? &hn[i + 1] : (H *)0;
        ln[i].k = i;
    }
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        int64_t s = 0;
        for (H *p = hn; p; p = p->next) s += p->k;
        double dt = now_ns() - t0;
        if (dt < b_h) b_h = dt;
        sum_h = s;

        L *lh = &ln[0];
        lh->next = lh; lh->prev = lh;
        for (int64_t i = 1; i < n; i++) {
            ln[i].next = lh;
            ln[i].prev = lh->prev;
            lh->prev->next = &ln[i];
            lh->prev = &ln[i];
        }
        t0 = now_ns();
        s = 0;
        for (L *p = lh->next; p != lh; p = p->next) s += p->k;
        dt = now_ns() - t0;
        if (dt < b_l) b_l = dt;
        sum_l = s;

        nx[0] = (n > 1) ? 1 : -1;
        for (int64_t i = 0; i < n; i++) nx[i] = (i + 1 < n) ? (int32_t)(i + 1) : (int32_t)-1;
        t0 = now_ns();
        s = 0;
        for (int32_t x = 0; x >= 0; x = nx[x]) s += keys[x];
        dt = now_ns() - t0;
        if (dt < b_i) b_i = dt;
        sum_i = s;
    }
    sink += sum_h + sum_l + sum_i;
    printf("e3\thlist\t%.2f\tn=%lld sum=%lld\n", b_h / (double)n, (long long)n, (long long)sum_h);
    printf("e3\tlist_head\t%.2f\tn=%lld sum=%lld\n", b_l / (double)n, (long long)n, (long long)sum_l);
    printf("e3\tindex-hlist\t%.2f\tn=%lld sum=%lld\n", b_i / (double)n, (long long)n, (long long)sum_i);
    free(hn); free(ln); free(nx); free(pp); free(keys);
}

int main(int argc, char **argv)
{
    int reps = (argc > 1) ? atoi(argv[1]) : 5;
    if (reps < 1) reps = 1;
    e1("e1-4k", 4096, reps);
    e1("e1-64k", 65536, reps);
    e2(1 << 20, reps);
    e3(1 << 20, reps);
    printf("# sink=%lld\n", (long long)sink);
    return 0;
}
