/* 同一份**压力形状**的 C 手写内存管理：双向链表（LRU）+ 带父指针的树 + 缓存 + 索引。
   手写引用计数在这里对环无能为力：next/prev、child/parent 互指 ⇒ 计数永不归零 ⇒ 全部泄漏。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define BUCKETS 64
#define BUCKET_MAX 8
#define CACHE 1024
#define INDEXMAX 200000
typedef struct Node { long key, val; struct Node *next, *prev, *parent, *child; int refs; } Node;
static Node *mknode(long k, long v) { Node *n = calloc(1, sizeof *n); n->key=k; n->val=v; return n; }
static void ref(Node *n){ if(n) n->refs++; }
static void unref(Node *n){ if (n && --n->refs == 0) { unref(n->next); unref(n->prev); unref(n->parent); unref(n->child); free(n); } }
int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    Node *head[BUCKETS], *tail[BUCKETS]; int size[BUCKETS];
    Node *cache[CACHE]; Node **idx = malloc(sizeof(Node*)*(INDEXMAX+1)); size_t nidx=0;
    Node *nodes[8192]; long nnodes=0; long sink=0;
    memset(head,0,sizeof head); memset(tail,0,sizeof tail); memset(size,0,sizeof size); memset(cache,0,sizeof cache);
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for (long i=0;i<n;i++){
        int b=(int)(i%BUCKETS);
        sink += i+(i+1)+(i+2);
        Node *nd = mknode(i, i*3);
        /* 双向链入 */
        ref(head[b]); nd->next = head[b];
        if (head[b]) { ref(nd); head[b]->prev = nd; }
        unref(head[b]); head[b] = nd; ref(nd);
        if (!tail[b]) tail[b] = nd;
        size[b]++;
        ref(nd); unref(cache[i%CACHE]); cache[i%CACHE]=nd;
        if (i < INDEXMAX) { ref(nd); idx[nidx++]=nd; }
        /* 带父指针的树：child/parent 互指 */
        if ((i & 15) == 0) { nodes[nnodes & 8191] = nd;
            if (nnodes > 0) { Node *par = nodes[(nnodes-1)&8191];
                if (par) { ref(nd); par->child = nd; ref(par); nd->parent = par; } }
            nnodes++; }
        for (Node *q=head[b]; q; q=q->next) if (q->key==i-7){ q->val++; break; }
        if (size[b] > BUCKET_MAX && tail[b]) {          /* 摘尾：prev/next 仍互指 ⇒ 环留下 */
            Node *tl=tail[b], *pr=tl->prev;
            if (pr) { unref(tl->next); tail[b]=pr; } else { unref(head[b]); head[b]=NULL; tail[b]=NULL; }
            size[b]--;
        }
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    printf("c-manual-stress 耗时=%.1fms sink=%ld idx=%zu\n",
           ((t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6), sink, nidx);
    return 0;
}
