/* GC 敏感形状 2：每轮重建（回收压力）。
 * 每轮建 k 个节点的链、求和、**全部 free**，共 R 轮。
 * arena 出块即还；malloc 要逐个 free；tracing GC 要扫。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct node { int64_t v; struct node *next; } node;

/* 与 churn.c 同理：分配器要对优化器**不透明**，否则 GCC 会把循环里的分配提出来
 * （实测：整段只剩 1 个 malloc 调用）⇒ 那个数字不代表 malloc+free 的代价 ✗ */
static void *(*volatile v_alloc)(size_t) = malloc;
static void  (*volatile v_free)(void *)  = free;

int main(void) {
    const int64_t ROUNDS = 20000, K = 1000;
    int64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        node *h = NULL;
        for (int64_t i = 0; i < K; i++) {
            node *c = (node *)v_alloc(sizeof(node));
            c->v = i; c->next = h; h = c;
        }
        for (node *p = h; p; p = p->next) acc += p->v;
        while (h) { node *n = h->next; v_free(h); h = n; }
    }
    printf("%lld\n", (long long)acc);
    return 0;
}
