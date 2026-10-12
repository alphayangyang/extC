/* 同一份功能，用**标准 C 手工内存管理**：malloc/free + 手写引用计数，
   每个引用赋值点都要手动 ref/unref（这正是本设计自动化的那件事）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define BUCKETS 64
#define BUCKET_MAX 8
#define CACHE 1024
#define INDEXMAX 200000
typedef struct Node { long key, val; struct Node *next; int refs; } Node;
static Node *mknode(long k, long v) { Node *n = malloc(sizeof *n); n->key=k; n->val=v; n->next=NULL; n->refs=0; return n; }
static void ref(Node *n) { if (n) n->refs++; }
static void unref(Node *n) { while (n && --n->refs == 0) { Node *nx = n->next; free(n); n = nx; } }
int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    Node *head[BUCKETS]; int size[BUCKETS]; Node *cache[CACHE];
    Node **idx = malloc(sizeof(Node*) * (size_t)(INDEXMAX + 1)); size_t nidx = 0;
    memset(head, 0, sizeof head); memset(size, 0, sizeof size); memset(cache, 0, sizeof cache);
    long sink = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < n; i++) {
        int b = (int)(i % BUCKETS);
        sink += i + (i+1) + (i+2);                       /* 解析：3 个临时值是算术，不分配 */
        Node *nd = mknode(i, i*3);
        ref(head[b]); nd->next = head[b];                /* 手动 ref 旧头 */
        unref(head[b]); head[b] = nd; ref(nd);           /* 手动 unref 旧头、ref 新头 */
        size[b]++;
        ref(nd); unref(cache[i % CACHE]); cache[i % CACHE] = nd;   /* 缓存槽：ref 新 / unref 旧 */
        if (i < INDEXMAX) { ref(nd); idx[nidx++] = nd; }           /* 索引：ref */
        Node *q = head[b];
        while (q) { if (q->key == i-7) { q->val++; break; } q = q->next; }
        if (size[b] > BUCKET_MAX) {                      /* 淘汰尾节点 */
            Node *p = head[b], *c = p->next;
            while (c && c->next) { p = c; c = c->next; }
            if (c) { p->next = NULL; unref(c); } else { unref(head[b]); head[b] = NULL; }
            size[b]--;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("c-manual 耗时=%.1fms sink=%ld idx=%zu\n",
           ((t1.tv_sec-t0.tv_sec)*1000.0 + (t1.tv_nsec-t0.tv_nsec)/1e6), sink, nidx);
    return 0;
}
