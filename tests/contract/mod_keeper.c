/* 契约验证的**反面**靶子：声明写着"我不留你的指针"，它留了 —— 存进静态变量，下次调用再读。
 * 行为与 tests/hostile/mod_keeper.c 同源；那边等宿主 `close()` 板，这边等宿主**隔离**。
 * 两种处置都必须给出同一个结论：契约与库不符，而且是响亮的。 */
#include <stdint.h>

static uint8_t *kept;

int64_t sum_keep(uint8_t *p, int64_t n) {
    kept = p;
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += p[i];
    return s;
}

int64_t read_later(void) {
    if (!kept) return -1;
    return (int64_t)kept[0];
}
