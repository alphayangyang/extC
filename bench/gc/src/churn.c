/* 形状 1：churn —— 与 churn.extc 逐字同语义（见那边的注释）。
 * acc = Σ(每当 i ≥ 2 时，被覆盖那个槽里旧节点的值)；输出 = acc + A.v + (B 若存在) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct node { int64_t v; struct node *next; } node;

/* ⚠️ 必须让分配器**对优化器不透明**：实测 GCC 会证明这里的 free 可省
 * （32e6 次 malloc、**0 次 free**）⇒ 那个数字不代表 malloc+free 的代价 ✗
 * 用 volatile 函数指针挡住内建替换 ⇒ 两边都是真的调用 ✓ */
static void *(*volatile v_alloc)(size_t) = malloc;
static void  (*volatile v_free)(void *)  = free;

int main(void) {
    const int64_t ROUNDS = 32000000;
    int64_t acc = 0;
    for (int64_t i = 0; i < ROUNDS; i++) {
        node *a = NULL, *b = NULL;
        node *p = (node *)v_alloc(sizeof(node));
        p->v = i; p->next = NULL;
        if ((i & 1) == 0) {
            if (a) { acc += a->v; v_free(a); }
            a = p;
        } else {
            if (b) { acc += b->v; v_free(b); }
            b = p;
        }
        if (a) acc += a->v;
        if (b) acc += b->v;
    }
    printf("%lld\n", (long long)acc);
    return 0;
}
