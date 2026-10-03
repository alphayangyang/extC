/* 契约验证的**正面**靶子：签名说"我不留你的指针"（`effects Addr=0 Cont=0`），它确实不留 ——
 * 只在调用期间读。与 tests/hostile/mod_keeper.c 是同一个形状的两面：
 * hostile 问"食言的库能把宿主怎么样"，contract 问"食言能不能被当场证伪"。 */
#include <stdint.h>

int64_t sum(const uint8_t *p, int64_t n) {
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += p[i];
    return s;
}

/* 调用它什么都不碰：用来证明"隔离之后程序照常跑"，崩溃只可能来自食言那一侧。 */
int64_t touch_nothing(void) { return 1; }
