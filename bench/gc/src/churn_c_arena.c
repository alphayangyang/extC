/* 形状 1 的同模型对照：C 手写 bump arena（不逐个 free），与 churn.extc 逐字同语义。
 * acc += buffered 就是"被覆盖那个槽里的旧值" —— 环形缓冲区自然表达这件事 ✓ */
#include <stdio.h>
#include <stdint.h>
typedef struct node { int64_t v; struct node *next; } node;
#define ARENA_NODES 2

int main(void) {
    const int64_t ROUNDS = 32000000;
    node arena[ARENA_NODES];
    int used = 0;
    int64_t acc = 0, buffered = 0;
    for (int64_t i = 0; i < ROUNDS; i++) {
        if (used == ARENA_NODES) used = 0;        /* 出块即还 */
        node *p = &arena[used++];
        p->v = i;
        p->next = NULL;
        acc += buffered;                          /* 被覆盖的旧值 */
        buffered = p->v;
    }
    printf("%lld\n", (long long)(acc + buffered));
    return 0;
}
